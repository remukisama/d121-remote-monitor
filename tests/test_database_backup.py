import json
import sqlite3
from pathlib import Path

import pytest

from backend.protocol import decode
from backend.store import Store
from tools import database_backup as backup


@pytest.fixture
def archive(tmp_path):
    store = Store(tmp_path/'live.db')
    cfg = {'state_topic': 'state', 'event_topic': 'event'}
    packets = [
        ('state', {'v': 2, 'd': [20, 4, 10, 1, [1, 1, 33, 0, 0], [1, 1, 15, 0, 32768, 0, 120]]}),
        ('event', {'v': 4, 'd': [20, 4, 10, 120, 10]}),
        ('event', {'v': 3, 'd': [20, 4, 10, 2, 0, 0, 10, 0, 0, 410]}),
        ('event', {'v': 1, 'd': [20, 4, 1, 1, 24, 7, 1, 11000]}),
    ]
    for topic, packet in packets:
        kind, value = decode(topic, json.dumps(packet).encode(), cfg)
        store.ingest(kind, value, stamp=1000)
    with store.db:
        store.db.execute('INSERT INTO members(openid,created) VALUES(?,?)', ('Z-owner', 1000))
        store.db.execute('INSERT INTO members(openid,created) VALUES(?,?)', ('a-owner', 1001))
        store.db.execute('INSERT INTO sessions(hash,expires,openid) VALUES(?,?,?)', ('hashed-session', 999999, 'Z-owner'))
        store.db.execute('INSERT INTO contexts(identity,received,boot,collection,kind,uptime,payload) VALUES(?,?,?,?,?,?,?)',
                         ('20:4:0:10', 1000, 20, 4, 0, 10, store.db.execute('SELECT payload FROM samples').fetchone()[0]))
    yield store
    store.close()


def test_online_wal_backup_restore_preserves_business_queries_and_authorization(tmp_path, archive):
    # Keep the live connection open with committed WAL data; add an uncommitted record separately.
    assert Path(str(tmp_path/'live.db')+'-wal').stat().st_size > 0
    archive.db.execute('INSERT INTO members(openid,created) VALUES(?,?)', ('uncommitted', 0))
    before = archive.event_detail(archive.event_rows()[0]['id'])
    report = backup.backup_sqlite(tmp_path/'live.db', tmp_path/'backup.db')
    assert report['tables']['members']['rows'] == 2
    assert report['tables']['contexts']['rows'] == 1
    archive.db.rollback()
    # Later source writes must not enter the already completed snapshot.
    with archive.db:
        archive.db.execute('INSERT INTO members(openid,created) VALUES(?,?)', ('after-backup', 1))
    restored = backup.restore_sqlite(tmp_path/'backup.db', tmp_path/'restored.db')
    assert report['tables'] == restored['tables']
    store = Store(tmp_path/'restored.db')
    try:
        assert store.event_detail(store.event_rows()[0]['id']) == before
        assert store.latest()['nodes'][0]['temperature'] == 0
        assert store.latest()['nodes'][0]['vibration'] == .12
        assert store.db.execute('SELECT hash FROM sessions').fetchone()['hash'] == 'hashed-session'
        assert [r['openid'] for r in store.db.execute('SELECT openid FROM members ORDER BY openid')] == ['Z-owner', 'a-owner']
    finally:
        store.close()


def test_existing_target_and_manifest_are_never_replaced(tmp_path, archive):
    target = tmp_path/'saved.db'
    target.write_bytes(b'existing')
    with pytest.raises(FileExistsError):
        backup.backup_sqlite(tmp_path/'live.db', target)
    assert target.read_bytes() == b'existing'
    target.unlink()
    backup.manifest_path(target).write_text('existing')
    with pytest.raises(FileExistsError):
        backup.backup_sqlite(tmp_path/'live.db', target)
    assert not target.exists()
    assert not list(tmp_path.glob('.d121-backup-*'))


def test_corrupt_or_changed_backups_refuse_restoration(tmp_path, archive):
    source = tmp_path/'backup.db'
    backup.backup_sqlite(tmp_path/'live.db', source)
    with sqlite3.connect(source) as db:
        db.execute('DELETE FROM sessions')
    with pytest.raises(ValueError, match='invalid'):
        backup.restore_sqlite(source, tmp_path/'restored.db')
    assert not (tmp_path/'restored.db').exists()


def test_missing_source_does_not_create_a_database(tmp_path):
    with pytest.raises(FileNotFoundError):
        backup.backup_sqlite(tmp_path/'missing.db', tmp_path/'backup.db')
    assert not (tmp_path/'missing.db').exists()
    assert not (tmp_path/'backup.db').exists()


def test_old_archive_adds_only_optional_empty_tables_in_the_copy(tmp_path, archive):
    with archive.db:
        archive.db.execute('DROP TABLE motions')
        archive.db.execute('DROP TABLE contexts')
    report = backup.backup_sqlite(tmp_path/'live.db', tmp_path/'backup.db')
    assert set(report['schema_added_empty_tables']) == {'motions', 'contexts'}
    assert report['tables']['motions']['rows'] == report['tables']['contexts']['rows'] == 0
    assert not archive.db.execute("SELECT 1 FROM sqlite_master WHERE name='motions'").fetchone()
    backup.restore_sqlite(tmp_path/'backup.db', tmp_path/'restored.db')
    with backup.readonly(tmp_path/'restored.db') as db:
        assert db.execute('SELECT COUNT(*) FROM events').fetchone()[0] == 1


def test_missing_required_table_refuses_backup_without_publishing(tmp_path, archive):
    with archive.db:
        archive.db.execute('DROP TABLE samples')
    with pytest.raises(ValueError, match='Unsupported table schema'):
        backup.backup_sqlite(tmp_path/'live.db', tmp_path/'backup.db')
    assert not (tmp_path/'backup.db').exists()
    assert not list(tmp_path.glob('.d121-backup-*'))


def test_failure_during_final_publish_preserves_existing_destination(tmp_path, archive, monkeypatch):
    original = backup.os.link
    def race(source, target):
        if str(target).endswith('.db'):
            Path(target).write_bytes(b'created-by-another-process')
        return original(source, target)
    monkeypatch.setattr(backup.os, 'link', race)
    with pytest.raises(FileExistsError):
        backup.backup_sqlite(tmp_path/'live.db', tmp_path/'backup.db')
    assert (tmp_path/'backup.db').read_bytes() == b'created-by-another-process'
    assert not backup.manifest_path(tmp_path/'backup.db').exists()
    assert not list(tmp_path.glob('.d121-backup-*'))


class FakeCursor:
    def __init__(self, owner):
        self.owner, self.rows = owner, []
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def execute(self, query, args=()):
        self.owner.queries.append(query)
        if query.startswith(('SET TRANSACTION', 'START TRANSACTION')):
            self.rows = []
        elif 'information_schema.TABLES' in query:
            self.rows = [{'name': t, 'engine': self.owner.engine} for t in backup.TABLES]
        elif query == 'SELECT DATABASE() AS name':
            self.rows = [{'name': 'd121_restore_drill'}]
        elif query.startswith('CREATE DATABASE'):
            self.rows = []
        elif query.startswith('CREATE TABLE'):
            self.rows = []  # Fixture has the real SQLite model, not a MySQL DDL engine.
        else:
            if self.owner.fail_table and 'FROM `'+self.owner.fail_table+'`' in query:
                raise OSError('injected read failure')
            query = query.replace('ORDER BY BINARY ', 'ORDER BY ')
            self.rows = [dict(row) for row in self.owner.store.db.execute(query, args)]
    def fetchall(self):
        rows, self.rows = self.rows, []
        return rows
    def fetchmany(self, size):
        rows, self.rows = self.rows[:size], self.rows[size:]
        return rows
    def fetchone(self): return self.rows.pop(0) if self.rows else None
    def executemany(self, query, values):
        if self.owner.fail_insert and 'INSERT INTO `events`' in query:
            raise OSError('injected restore write failure')
        self.owner.store.db.executemany(query.replace('%s', '?'), values)


def test_runtime_config_is_explicit_and_primary_database_remains_protected(tmp_path, archive, monkeypatch):
    monkeypatch.setenv('MYSQL_ADDRESS', 'database.invalid:3307')
    monkeypatch.setenv('MYSQL_USERNAME', 'backup-operator')
    monkeypatch.setenv('MYSQL_PASSWORD', 'example-not-a-real-password')
    monkeypatch.setenv('MYSQL_DATABASE', 'd121_restore_primary')
    cfg = backup.runtime_mysql()
    assert cfg['host'] == 'database.invalid' and cfg['port'] == 3307
    backup.backup_sqlite(tmp_path/'live.db', tmp_path/'backup.db')
    def forbidden(*args): raise AssertionError('Must reject the runtime primary database')
    monkeypatch.setattr(backup, 'mysql_connection', forbidden)
    with pytest.raises(ValueError):
        backup.restore_mysql(tmp_path/'backup.db', cfg, 'd121_restore_primary')


class FakeMySQL:
    def __init__(self, store, engine='InnoDB', fail_table=None, fail_insert=False):
        self.store, self.engine, self.fail_table = store, engine, fail_table
        self.fail_insert = fail_insert
        self.queries, self.rollbacks, self.closed = [], 0, False
    def cursor(self, *args): return FakeCursor(self)
    def rollback(self): self.rollbacks += 1; self.store.db.rollback()
    def commit(self): self.store.db.commit()
    def close(self): self.closed = True


def test_mysql_snapshot_control_flow_uses_read_only_repeatable_read_and_keeps_auth(tmp_path, archive, monkeypatch):
    # A protocol adapter verifies the production tool's transaction flow; it is not a MySQL server.
    connection = FakeMySQL(archive)
    monkeypatch.setattr(backup, 'mysql_connection', lambda cfg, database: connection)
    result = backup.backup_mysql({'database': 'production'}, tmp_path/'mysql.db')
    assert result['tables']['members']['rows'] == 2
    assert result['tables']['sessions']['rows'] == 1
    assert connection.queries[:2] == ['SET TRANSACTION ISOLATION LEVEL REPEATABLE READ',
                                      'START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY']
    assert any('ORDER BY BINARY `openid`' in q for q in connection.queries)
    assert connection.rollbacks == 1 and connection.closed
    assert backup.verify(tmp_path/'mysql.db')['source_kind'] == 'mysql'


@pytest.mark.parametrize('options', [{'engine': 'MyISAM'}, {'fail_table': 'events'}])
def test_mysql_snapshot_refuses_nontransactional_tables_and_discards_partial_export(tmp_path, archive, monkeypatch, options):
    connection = FakeMySQL(archive, **options)
    monkeypatch.setattr(backup, 'mysql_connection', lambda cfg, database: connection)
    with pytest.raises((OSError, ValueError)):
        backup.backup_mysql({'database': 'production'}, tmp_path/'mysql.db')
    assert connection.rollbacks == 1 and connection.closed
    assert not (tmp_path/'mysql.db').exists()
    assert not list(tmp_path.glob('.d121-backup-*'))


@pytest.mark.parametrize('database', ['production', 'PRODUCTION', 'existing', 'd121_restore_x`; DROP DATABASE x'])
def test_mysql_restore_refuses_primary_or_arbitrary_database_before_connecting(tmp_path, archive, monkeypatch, database):
    backup.backup_sqlite(tmp_path/'live.db', tmp_path/'backup.db')
    def forbidden(*args): raise AssertionError('Must reject target before connecting')
    monkeypatch.setattr(backup, 'mysql_connection', forbidden)
    with pytest.raises(ValueError, match='new d121_restore_'):
        backup.restore_mysql(tmp_path/'backup.db', {'database': 'production'}, database)


@pytest.mark.parametrize('fail', [False, True])
def test_mysql_restore_flow_preserves_rows_or_rolls_back_all_data(tmp_path, archive, monkeypatch, fail):
    backup.backup_sqlite(tmp_path/'live.db', tmp_path/'backup.db')
    restored = Store(tmp_path/'target.db')
    admin = FakeMySQL(restored)
    target = FakeMySQL(restored, fail_insert=fail)
    connections = iter([admin, target])
    monkeypatch.setattr(backup, 'mysql_connection', lambda cfg, database: next(connections))
    try:
        if fail:
            with pytest.raises(OSError):
                backup.restore_mysql(tmp_path/'backup.db', {'database': 'production'}, 'd121_restore_drill')
            assert target.rollbacks == 1
            assert all(restored.db.execute('SELECT COUNT(*) FROM '+t).fetchone()[0] == 0 for t in backup.TABLES)
        else:
            result = backup.restore_mysql(tmp_path/'backup.db', {'database': 'production'}, 'd121_restore_drill')
            assert backup.inspect(restored.db) == result['tables']
            assert restored.db.execute('SELECT COUNT(*) FROM sessions').fetchone()[0] == 1
        assert admin.closed and target.closed
        assert admin.queries == ['CREATE DATABASE `d121_restore_drill` CHARACTER SET utf8mb4']
    finally:
        restored.close()
