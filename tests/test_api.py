import copy,json
from pathlib import Path
from fastapi.testclient import TestClient
from backend.api import create_app,load_config

def test_auth_expiry_permissions_and_query_bounds(tmp_path):
    cfg=json.loads((Path(__file__).parents[1]/'config.example.json').read_text(encoding='utf-8'))
    cfg['server'].update(database=str(tmp_path/'test.db'),development_token='test-private-key')
    app=create_app(cfg,collect=False)
    with TestClient(app) as c:
        assert c.get('/health').status_code==200
        assert c.get('/api/status').status_code==401
        assert c.post('/api/auth/local',json={'code':'wrong'}).status_code==403
        r=c.post('/api/auth/local',json={'code':'test-private-key'});assert r.status_code==200
        headers={'Authorization':'Bearer '+r.json()['token']}
        assert c.get('/api/status',headers=headers).status_code==200
        assert c.get('/api/events?limit=5000',headers=headers).status_code==422
        assert c.get('/api/events/999999',headers=headers).status_code==404
        assert c.post('/api/auth/wechat',json={'code':'not-real'}).status_code==503
        assert c.post('/api/auth/logout',headers=headers).status_code==200
        assert c.get('/api/status',headers=headers).status_code==401

def test_local_login_cannot_be_enabled_on_cloud(tmp_path):
    cfg=json.loads((Path(__file__).parents[1]/'config.example.json').read_text(encoding='utf-8'))
    cfg['server'].update(database=str(tmp_path/'test.db'),local_dev=False,development_token='a-secret')
    with TestClient(create_app(cfg,collect=False)) as c:
        assert c.post('/api/auth/local',json={'code':'a-secret'}).status_code==403

def test_authenticated_routes_accept_mysql_dictionary_rows(tmp_path):
    """Exercise authentication with the actual DictCursor result shape."""
    cfg=json.loads((Path(__file__).parents[1]/'config.example.json').read_text(encoding='utf-8'))
    cfg['server'].update(database=str(tmp_path/'auth.db'),development_token='local-test-key')
    app=create_app(cfg,collect=False)
    database=app.state.store.db
    class DictionaryResult:
        def __init__(self, result): self.result=result
        def fetchone(self):
            row=self.result.fetchone()
            return dict(row) if row is not None else None
    class DictionaryAuthConnection:
        def execute(self, sql, args=()):
            result=database.execute(sql,args)
            return DictionaryResult(result) if sql.startswith('SELECT openid FROM sessions') else result
        def __enter__(self): database.__enter__(); return self
        def __exit__(self,*args): return database.__exit__(*args)
        def close(self): database.close()
    app.state.store.db=DictionaryAuthConnection()
    with TestClient(app) as client:
        session=client.post('/api/auth/local',json={'code':'local-test-key'}).json()
        headers={'Authorization':'Bearer '+session['token']}
        assert client.get('/api/status',headers=headers).status_code==200
        assert client.get('/api/events',headers=headers).status_code==200
        assert client.get('/api/series',headers=headers).status_code==200
        assert client.post('/api/auth/logout',headers=headers).status_code==200
        assert client.get('/api/status',headers=headers).status_code==401

def test_cloud_container_route_can_start_without_a_public_domain(monkeypatch):
    cfg=json.loads((Path(__file__).parents[1]/'config.example.json').read_text(encoding='utf-8'))
    cfg['wechat'].update(appid='test-appid',appsecret='server-only-test-secret')
    cfg['server'].update(local_dev=False,database_driver='mysql',pairing_code='owner-test-code')
    cfg['deployment']['cloud_env_id']='test-cloud-environment'
    monkeypatch.setenv('D121_CONFIG_JSON',json.dumps(cfg))
    assert load_config()['server']['public_url']==''
