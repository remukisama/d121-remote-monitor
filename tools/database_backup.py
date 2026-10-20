"""Consistent application database backups; restoration always creates a new target.

Backups contain history, members and hashed sessions. Keep them private.
MySQL exports require InnoDB and a REPEATABLE READ, READ ONLY snapshot.
"""
import argparse
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from backend.store import Store

TABLES = ('samples', 'diagnostics', 'motions', 'contexts', 'events', 'sessions', 'members')
KEYS = dict.fromkeys(TABLES, 'id') | {'sessions': 'hash', 'members': 'openid'}
OPTIONAL_LEGACY = {'motions', 'contexts'}


def columns():
    model = Store(':memory:')
    try:
        return {table: tuple(r['name'] for r in model.db.execute('PRAGMA table_info('+table+')'))
                for table in TABLES}
    finally:
        model.close()


def readonly(path):
    path = Path(path).resolve(strict=True)
    db = sqlite3.connect(path.as_uri()+'?mode=ro', uri=True)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA trusted_schema=OFF')
    db.execute('PRAGMA query_only=ON')
    return db


def fingerprint(rows, names):
    digest = hashlib.sha256()
    total = 0
    for row in rows:
        values = [row[name] for name in names]
        digest.update(json.dumps(values, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode()+b'\n')
        total += 1
    return {'rows': total, 'sha256': digest.hexdigest()}


def mysql_order(table):
    # SQLite uses binary ordering. MySQL's default text collation can sort mixed case differently.
    return ('BINARY ' if table in ('sessions', 'members') else '')+'`'+KEYS[table]+'`'


def inspect(db):
    if db.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
        raise ValueError('SQLite integrity check failed')
    result = {}
    for table, names in columns().items():
        actual = tuple(r['name'] for r in db.execute('PRAGMA table_info('+table+')'))
        if actual != names:
            raise ValueError('Unsupported table schema: '+table)
        result[table] = fingerprint(db.execute('SELECT '+','.join(names)+' FROM '+table+' ORDER BY '+KEYS[table]), names)
    return result


def legacy_missing(db):
    missing = []
    for table, names in columns().items():
        actual = tuple(r['name'] for r in db.execute('PRAGMA table_info('+table+')'))
        if not actual and table in OPTIONAL_LEGACY:
            missing.append(table)
        elif actual != names:
            raise ValueError('Unsupported table schema: '+table)
    return missing


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            digest.update(block)
    return digest.hexdigest()


def manifest_path(path):
    return Path(str(path)+'.manifest.json')


def temporary(target):
    target = Path(target).resolve()
    if target.exists() or manifest_path(target).exists():
        raise FileExistsError('Target or manifest already exists; overwrite is disabled')
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.d121-backup-', suffix='.db', dir=target.parent)
    os.close(fd)
    return target, Path(name)


def cleanup(path):
    for suffix in ('', '-wal', '-shm', '.manifest.json'):
        Path(str(path)+suffix).unlink(missing_ok=True)


def publish(temp, target, source_kind, source_database=None, expected=None, added=()):
    with closing(readonly(temp)) as db:
        tables = inspect(db)
    if expected is not None and tables != expected:
        raise ValueError('Restored table fingerprints differ from the backup')
    metadata = {'format': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                'source_kind': source_kind, 'source_database': source_database,
                'schema_added_empty_tables': list(added),
                'sha256': file_hash(temp), 'tables': tables}
    note = manifest_path(temp)
    with note.open('x', encoding='utf-8') as stream:
        json.dump(metadata, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(note, 0o600)
    # Same-directory hard links publish complete files atomically and never replace a target.
    linked = False
    try:
        os.link(temp, target)
        linked = True
        os.link(note, manifest_path(target))
    except BaseException:
        if linked:
            target.unlink()
        raise
    return metadata


def backup_sqlite(source, target, expected=None):
    # Never use copyfile: committed records may still be in the source WAL.
    with closing(readonly(source)) as original:
        target, temp = temporary(target)
        try:
            deadline = time.monotonic()+120
            def progress(status, remaining, total):
                if time.monotonic() > deadline:
                    raise TimeoutError('Online backup deadline exceeded')
            with closing(sqlite3.connect(temp)) as snapshot:
                original.backup(snapshot, pages=128, progress=progress, sleep=.05)
            with closing(readonly(temp)) as snapshot:
                added = legacy_missing(snapshot)
            if added:
                # Upgrade only the isolated copy; pre-motion live databases remain untouched.
                model = Store(temp)
                model.close()
            return publish(temp, target, 'sqlite', expected=expected, added=added)
        finally:
            cleanup(temp)


def verify(path):
    path = Path(path).resolve(strict=True)
    metadata = json.loads(manifest_path(path).read_text(encoding='utf-8'))
    if metadata.get('format') != 1 or file_hash(path) != metadata.get('sha256'):
        raise ValueError('Backup checksum or format is invalid')
    with closing(readonly(path)) as db:
        if inspect(db) != metadata.get('tables'):
            raise ValueError('Backup table fingerprints are invalid')
    if file_hash(path) != metadata['sha256']:
        raise ValueError('Backup changed during verification')
    return metadata


def restore_sqlite(source, target):
    metadata = verify(source)
    return backup_sqlite(source, target, expected=metadata['tables'])


def mysql_connection(cfg, database):
    import pymysql
    return pymysql.connect(host=cfg['host'], port=int(cfg.get('port', 3306)),
                           user=cfg['username'], password=cfg['password'], database=database,
                           charset='utf8mb4', connect_timeout=5, read_timeout=30, write_timeout=30,
                           autocommit=False, cursorclass=pymysql.cursors.DictCursor)


def runtime_mysql():
    address = os.environ['MYSQL_ADDRESS']
    host, port = address.rsplit(':', 1) if ':' in address else (address, 3306)
    return {'host': host, 'port': int(port), 'username': os.environ['MYSQL_USERNAME'],
            'password': os.environ['MYSQL_PASSWORD'], 'database': os.environ.get('MYSQL_DATABASE', 'd121_monitor')}


def backup_mysql(cfg, target):
    import pymysql
    # An explicit config is used: MYSQL_DATABASE in the environment cannot redirect this tool.
    target, temp = temporary(target)
    try:
        model = Store(temp)
        try:
            db = mysql_connection(cfg, cfg['database'])
            try:
                with db.cursor(pymysql.cursors.SSDictCursor) as cursor:
                    cursor.execute('SET TRANSACTION ISOLATION LEVEL REPEATABLE READ')
                    cursor.execute('START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY')
                    cursor.execute('SELECT TABLE_NAME AS name,ENGINE AS engine FROM information_schema.TABLES WHERE TABLE_SCHEMA=%s', (cfg['database'],))
                    engines = {row['name']: row['engine'] for row in cursor.fetchall()}
                    added = [table for table in TABLES if table not in engines and table in OPTIONAL_LEGACY]
                    if any(engines.get(table) != 'InnoDB' for table in TABLES if table not in added):
                        raise ValueError('All application tables must exist and use InnoDB')
                    with model.db:
                        for table, names in columns().items():
                            if table in added:
                                continue
                            cursor.execute('SELECT '+','.join('`'+name+'`' for name in names)+' FROM `'+table+'` ORDER BY '+mysql_order(table))
                            while True:
                                batch = cursor.fetchmany(512)
                                if not batch:
                                    break
                                model.db.executemany('INSERT INTO '+table+'('+','.join(names)+') VALUES('+','.join('?' for _ in names)+')',
                                                     [tuple(row[name] for name in names) for row in batch])
            finally:
                try:
                    db.rollback()
                finally:
                    db.close()
        finally:
            model.close()
        return publish(temp, target, 'mysql', cfg['database'], added=added)
    finally:
        cleanup(temp)


def restore_mysql(source, cfg, database):
    import pymysql
    metadata = verify(source)
    if (not re.fullmatch(r'd121_restore_[a-z0-9_]{1,48}', database)
            or database.casefold() in {str(cfg['database']).casefold(), str(metadata.get('source_database')).casefold()}):
        raise ValueError('MySQL restore requires a new d121_restore_* database distinct from the source')
    # CREATE without IF NOT EXISTS rejects even an existing empty drill database.
    admin = mysql_connection(cfg, None)
    try:
        with admin.cursor() as cursor:
            cursor.execute('CREATE DATABASE `'+database+'` CHARACTER SET utf8mb4')
    finally:
        admin.close()
    from backend.mysql_store import DDL
    target = mysql_connection(cfg, database)
    try:
        with closing(readonly(source)) as original, target.cursor() as cursor:
            cursor.execute('SELECT DATABASE() AS name')
            if cursor.fetchone()['name'].casefold() != database.casefold():
                raise ValueError('MySQL target database mismatch')
            for statement in DDL:
                cursor.execute(statement)
            target.commit()
            for table in TABLES:
                cursor.execute('SELECT COUNT(*) AS total FROM `'+table+'`')
                if cursor.fetchone()['total']:
                    raise ValueError('MySQL restore target is not empty')
            for table, names in columns().items():
                query = 'INSERT INTO `'+table+'`('+','.join('`'+name+'`' for name in names)+') VALUES('+','.join('%s' for _ in names)+')'
                rows = original.execute('SELECT '+','.join(names)+' FROM '+table+' ORDER BY '+KEYS[table])
                while True:
                    batch = rows.fetchmany(512)
                    if not batch:
                        break
                    cursor.executemany(query, [tuple(row[name] for name in names) for row in batch])
            # Validate every row before committing any restored data.
            with target.cursor(pymysql.cursors.SSDictCursor) as check:
                for table, names in columns().items():
                    check.execute('SELECT '+','.join('`'+name+'`' for name in names)+' FROM `'+table+'` ORDER BY '+mysql_order(table))
                    if fingerprint(iter(check.fetchone, None), names) != metadata['tables'][table]:
                        raise ValueError('MySQL restored table fingerprint mismatch: '+table)
            target.commit()
    except BaseException:
        target.rollback()
        raise
    finally:
        target.close()
    return {'database': database, 'tables': metadata['tables']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='operation', required=True)
    for name in ('backup-sqlite', 'restore-sqlite'):
        command = commands.add_parser(name)
        command.add_argument('--source', type=Path, required=True)
        command.add_argument('--target', type=Path, required=True)
    command = commands.add_parser('verify')
    command.add_argument('--source', type=Path, required=True)
    for name in ('backup-mysql', 'restore-mysql'):
        command = commands.add_parser(name)
        config = command.add_mutually_exclusive_group()
        config.add_argument('--config', type=Path)
        config.add_argument('--runtime-env', action='store_true', help='Read the existing MYSQL_* container environment explicitly')
        command.add_argument('--source' if name == 'restore-mysql' else '--target', type=Path, required=True)
        if name == 'restore-mysql':
            command.add_argument('--target-database', required=True)
    args = parser.parse_args()
    try:
        if args.operation == 'backup-sqlite': result = backup_sqlite(args.source, args.target)
        elif args.operation == 'restore-sqlite': result = restore_sqlite(args.source, args.target)
        elif args.operation == 'verify': result = verify(args.source)
        else:
            config_path = args.config or Path(__file__).resolve().parents[1]/'config.private.json'
            cfg = runtime_mysql() if args.runtime_env else json.loads(config_path.read_text(encoding='utf-8-sig'))['mysql']
            result = (backup_mysql(cfg, args.target) if args.operation == 'backup-mysql'
                      else restore_mysql(args.source, cfg, args.target_database))
        print(json.dumps({'ok': True, 'operation': args.operation,
                          'rows': {table: detail['rows'] for table, detail in result['tables'].items()}}, ensure_ascii=False))
    except Exception as error:
        # Database error strings can include private addresses, users or SQL data.
        code = error.args[0] if error.args and isinstance(error.args[0], int) else None
        print(json.dumps({'ok': False, 'operation': args.operation, 'error': type(error).__name__, 'database_error_code': code}))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
