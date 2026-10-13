import pytest
from backend.store import Store
from tools.migrate_history import migrate

def test_history_migration_preserves_rows_and_excludes_local_authorization(tmp_path):
    source=Store(tmp_path/'source.db');destination=Store(tmp_path/'target.db')
    try:
        with source.db:
            source.db.execute('INSERT INTO samples(stamp,boot,collection,uptime,payload) VALUES(?,?,?,?,?)',(100,2,3,10,'{}'))
            source.db.execute('INSERT INTO sessions(hash,expires,openid) VALUES(?,?,?)',('local-token',999,'local-owner'))
            source.db.execute('INSERT INTO members(openid,created) VALUES(?,?)',('local-owner',100))
        counts=migrate(tmp_path/'source.db',destination)
        assert counts=={'samples':1,'diagnostics':0,'events':0,'motions':0}
        assert destination.db.execute('SELECT boot FROM samples').fetchone()['boot']==2
        assert not destination.db.execute('SELECT * FROM sessions').fetchall()
        assert not destination.db.execute('SELECT * FROM members').fetchall()
        with pytest.raises(ValueError,match='must be empty'):migrate(tmp_path/'source.db',destination)
        assert destination.db.execute('SELECT COUNT(*) AS total FROM samples').fetchone()['total']==1
    finally:source.close();destination.close()
