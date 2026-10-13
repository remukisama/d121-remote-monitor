"""Copy archived telemetry into an EMPTY cloud database; never copy local sessions."""
import argparse, sqlite3, sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from backend.mysql_store import MySQLStore

def migrate(source,destination):
    # SQLite backup includes committed WAL data and gives a consistent read snapshot.
    snapshot=sqlite3.connect(':memory:');snapshot.row_factory=sqlite3.Row
    with sqlite3.connect(source) as original: original.backup(snapshot)
    tables=('samples','diagnostics','events','motions')
    counts={}
    try:
        with destination.lock,destination.db:
            for table in ('samples','diagnostics','events','motions','sessions','members'):
                row=destination.db.execute('SELECT COUNT(*) AS total FROM '+table).fetchone()
                if row['total']:raise ValueError('Destination must be empty; existing data is never overwritten')
            for table in tables:
                if table=='motions' and not snapshot.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='motions'").fetchone():
                    counts[table]=0;continue # Backward compatibility with pre-motion backups.
                rows=snapshot.execute('SELECT * FROM '+table+' ORDER BY id')
                columns=[item[0] for item in rows.description]
                query='INSERT INTO '+table+'('+','.join(columns)+') VALUES('+','.join('?' for _ in columns)+')'
                counts[table]=0
                for row in rows:
                    destination.db.execute(query,tuple(row));counts[table]+=1
        return counts
    finally:snapshot.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'data/monitor.db')
    parser.add_argument('--execute',action='store_true',help='Write to the configured empty MySQL database')
    args=parser.parse_args()
    if not args.source.is_file():parser.error('Source database does not exist')
    if not args.execute:parser.error('Explicit --execute is required; stop the destination collector first')
    # MYSQL_* are private environment variables supplied for this invocation.
    store=MySQLStore({'database':'d121_monitor'})
    try:print('Migrated historical rows (local login sessions excluded):',migrate(args.source,store))
    finally:store.close()
