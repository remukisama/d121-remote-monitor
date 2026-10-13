import copy,json
import logging
import httpx
import pytest
from pathlib import Path
from fastapi.testclient import TestClient
from backend.api import create_app,load_config

@pytest.mark.parametrize('failure,expected',[
    (httpx.ConnectError('sensitive-secret-in-connection-error'),'WX_UPSTREAM_CONNECT'),
    (httpx.ReadTimeout('sensitive-secret-in-timeout'),'WX_UPSTREAM_TIMEOUT'),
    (json.JSONDecodeError('sensitive-secret-in-response','not-json',0),'WX_UPSTREAM_JSON')
])
def test_wechat_failure_diagnostics_do_not_expose_secrets(tmp_path,monkeypatch,caplog,failure,expected):
    cfg=json.loads((Path(__file__).parents[1]/'config.example.json').read_text(encoding='utf-8'))
    cfg['server'].update(database=str(tmp_path/'auth-error.db'))
    cfg['wechat'].update(appid='test-appid',appsecret='sensitive-secret-config')
    class UpstreamClient:
        def __init__(self,**kwargs): pass
        async def __aenter__(self): return self
        async def __aexit__(self,*args): pass
        async def get(self,*args,**kwargs): raise failure
    monkeypatch.setattr('backend.api.httpx.AsyncClient',UpstreamClient)
    with caplog.at_level(logging.WARNING),TestClient(create_app(cfg,collect=False)) as client:
        response=client.post('/api/auth/wechat',json={'code':'sensitive-secret-login-code'})
        assert response.status_code==503
        assert response.json()['diagnostic_code']==expected
        assert expected in caplog.text
        assert 'sensitive-secret' not in response.text+caplog.text

def test_verified_wechat_owner_pairs_once_and_can_log_in_again(tmp_path,monkeypatch):
    cfg=json.loads((Path(__file__).parents[1]/'config.example.json').read_text(encoding='utf-8'))
    cfg['server'].update(database=str(tmp_path/'wechat-owner.db'),pairing_code='private-owner-pairing')
    cfg['wechat'].update(appid='test-appid',appsecret='private-wechat-secret')
    class UpstreamClient:
        def __init__(self,**kwargs): pass
        async def __aenter__(self): return self
        async def __aexit__(self,*args): pass
        async def get(self,*args,**kwargs):
            return httpx.Response(200,json={'openid':'verified-owner','session_key':'server-only-session-key'},
                                  request=httpx.Request('GET','https://api.weixin.qq.com/sns/jscode2session'))
    monkeypatch.setattr('backend.api.httpx.AsyncClient',UpstreamClient)
    with TestClient(create_app(cfg,collect=False)) as client:
        assert client.post('/api/auth/wechat',json={'code':'code-1','pairing_code':'wrong'}).status_code==403
        paired=client.post('/api/auth/wechat',json={'code':'code-2','pairing_code':'private-owner-pairing'})
        assert paired.status_code==200
        assert 'server-only-session-key' not in paired.text
        headers={'Authorization':'Bearer '+paired.json()['token']}
        assert client.get('/api/status',headers=headers).status_code==200
        assert client.post('/api/auth/wechat',json={'code':'code-3'}).status_code==200
        assert client.post('/api/auth/logout',headers=headers).status_code==200
        assert client.get('/api/status',headers=headers).status_code==401

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
