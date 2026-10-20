import hashlib, json, logging, os, secrets, ssl, time
from collections import defaultdict, deque
from contextlib import asynccontextmanager
from pathlib import Path
import httpx
from fastapi import FastAPI, HTTPException, Request, Depends, Query
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field
from .store import Store
from .collector import Collector
from .wechat_tls import wechat_ssl_context

ROOT=Path(__file__).resolve().parents[1]
logger=logging.getLogger('d121.auth')

def login_failure(error):
    """Expose bounded categories, never exception text or credential-bearing URLs."""
    chain=[];current=error
    while current is not None and len(chain)<8 and all(current is not previous for previous in chain):
        chain.append(current);current=current.__cause__ or current.__context__
    if any(isinstance(item,ssl.SSLCertVerificationError) for item in chain): code='WX_UPSTREAM_TLS'
    elif isinstance(error,httpx.TimeoutException): code='WX_UPSTREAM_TIMEOUT'
    elif isinstance(error,httpx.ConnectError): code='WX_UPSTREAM_CONNECT'
    elif isinstance(error,httpx.HTTPStatusError): code='WX_UPSTREAM_HTTP'
    elif isinstance(error,json.JSONDecodeError): code='WX_UPSTREAM_JSON'
    else: code='WX_UPSTREAM_RUNTIME'
    return code,','.join(type(item).__name__ for item in chain)

class Login(BaseModel):
    code: str = Field(min_length=1,max_length=256)
    pairing_code: str = Field(default='',max_length=128)

def load_config():
    raw=os.environ.get('D121_CONFIG_JSON')
    path=Path(os.environ.get('D121_CONFIG',ROOT/'config.private.json'))
    cfg=json.loads(raw) if raw else json.loads(path.read_text(encoding='utf-8-sig'))
    server=cfg['server']
    if not 5<=int(server['offline_seconds'])<=300 or not 1<=int(server['retention_days'])<=365:
        raise ValueError('Invalid history or offline thresholds')
    if not server.get('local_dev'):
        if not cfg['wechat'].get('appid') or not cfg['wechat'].get('appsecret'):
            raise ValueError('Production needs WeChat login credentials')
        if not cfg.get('deployment',{}).get('cloud_env_id') and not server.get('public_url','').startswith('https://'):
            raise ValueError('Production needs a cloud hosting environment or an HTTPS URL')
        if not cfg['wechat'].get('allowed_openids') and not server.get('pairing_code'):
            raise ValueError('Production needs an owner allowlist or a private pairing code')
        if server.get('database_driver') != 'mysql':
            raise ValueError('Cloud production requires a durable MySQL database')
    return cfg

def create_app(cfg=None, collect=True):
    cfg=cfg or load_config()
    server=cfg['server']
    database=Path(server['database'])
    if not database.is_absolute(): database=ROOT/database
    if server.get('database_driver') == 'mysql':
        from .mysql_store import MySQLStore
        store=MySQLStore(cfg.get('mysql',{}),server['device_id'],server['offline_seconds'])
    else: store=Store(database,server['device_id'],server['offline_seconds'])
    collector=Collector(store,cfg['bemfa'],server['retention_days'],acknowledge=not server.get('local_dev'))
    attempts=defaultdict(deque)

    @asynccontextmanager
    async def lifespan(app):
        if collect: collector.start()
        yield
        collector.stop(); store.close()

    app=FastAPI(title='D121 远程监控',lifespan=lifespan,docs_url='/docs' if server.get('local_dev') else None,redoc_url=None)
    app.state.store,app.state.collector=store,collector

    @app.middleware('http')
    async def security_headers(request,call_next):
        response=await call_next(request)
        response.headers['Cache-Control']='no-store'
        response.headers['X-Content-Type-Options']='nosniff'
        return response

    def rate_limit(request):
        key=request.client.host if request.client else 'unknown'
        now=time.monotonic()
        q=attempts[key]
        while q and now-q[0]>60: q.popleft()
        if len(q)>=10: raise HTTPException(429,'尝试过于频繁，请稍后再试')
        q.append(now)
        if len(attempts)>1024:
            for k in list(attempts):
                if not attempts[k] or now-attempts[k][-1]>60: attempts.pop(k,None)

    def issue_session(openid):
        token=secrets.token_urlsafe(32); expiry=time.time()+server.get('session_hours',24)*3600
        with store.lock,store.db:
            store.db.execute('INSERT INTO sessions(hash,expires,openid) VALUES(?,?,?)',
                              (hashlib.sha256(token.encode()).hexdigest(),expiry,openid))
        return dict(token=token,expires_at=expiry,device_id=server['device_id'])

    def auth(request:Request):
        header=request.headers.get('authorization','')
        if not header.startswith('Bearer ') or len(header)>256: raise HTTPException(401,'请先登录')
        digest=hashlib.sha256(header[7:].encode()).hexdigest()
        with store.lock:
            row=store.db.execute('SELECT openid FROM sessions WHERE hash=? AND expires>?',(digest,time.time())).fetchone()
        if not row: raise HTTPException(401,'登录已过期，请重新登录')
        return row['openid']

    @app.get('/health')
    def health(): return dict(service='d121-monitor',ok=True)

    @app.get('/ready')
    def ready():
        database=store.database_ready()
        worker=bool(collector.thread and collector.thread.is_alive())
        subscribed=bool(store.linked)
        archive=store.archive_error is None
        ok=database and worker and subscribed and archive
        return JSONResponse(status_code=200 if ok else 503,content=dict(
            service='d121-monitor',ok=ok,database=database,worker=worker,
            subscribed=subscribed,archive=archive))

    @app.post('/api/auth/local')
    def local_login(body:Login,request:Request):
        rate_limit(request)
        if not server.get('local_dev') or not request.client or request.client.host not in ('127.0.0.1','::1','testclient'):
            raise HTTPException(403,'本地联调登录不可用于公网')
        token=server.get('development_token','')
        if not token or not secrets.compare_digest(body.code,token): raise HTTPException(403,'联调密钥不正确')
        return issue_session('local-owner')

    @app.post('/api/auth/wechat')
    async def wechat_login(body:Login,request:Request):
        rate_limit(request)
        wx=cfg['wechat']
        if not wx.get('appsecret'): raise HTTPException(503,'微信登录配置尚未完成')
        stage='client'
        try:
            async with httpx.AsyncClient(timeout=10,verify=wechat_ssl_context()) as client:
                stage='request'
                response=await client.get('https://api.weixin.qq.com/sns/jscode2session',params={
                    'appid':wx['appid'],'secret':wx['appsecret'],'js_code':body.code,'grant_type':'authorization_code'})
                stage='http_status';response.raise_for_status()
                stage='json';result=response.json()
            stage='identity'
            openid=result.get('openid')
            if not isinstance(openid,str) or not openid: raise HTTPException(401,'微信登录校验失败，请重试')
        except HTTPException: raise
        except Exception as error:
            code,kinds=login_failure(error)
            logger.warning('wechat_login_unavailable stage=%s code=%s exceptions=%s',stage,code,kinds)
            return JSONResponse(status_code=503,content={'detail':'微信登录服务暂不可用','diagnostic_code':code})
        with store.lock:
            member=store.db.execute('SELECT 1 FROM members WHERE openid=?',(openid,)).fetchone()
        allowed=openid in wx.get('allowed_openids',[]) or member
        pairing=server.get('pairing_code','')
        if not allowed and pairing and secrets.compare_digest(body.pairing_code,pairing):
            with store.lock,store.db: store.db.execute('INSERT OR IGNORE INTO members VALUES(?,?)',(openid,time.time()))
            allowed=True
        if not allowed: raise HTTPException(403,'此微信尚未绑定设备，请填写设备配对码')
        return issue_session(openid)

    @app.post('/api/auth/logout')
    def logout(request:Request,user=Depends(auth)):
        with store.lock,store.db: store.db.execute('DELETE FROM sessions WHERE hash=?',
            (hashlib.sha256(request.headers['authorization'][7:].encode()).hexdigest(),))
        return dict(ok=True)

    @app.get('/api/status')
    def status(user=Depends(auth)):
        value=store.status(); value['device_name']=server['device_name']
        value['retention_days']=server['retention_days']
        value['collector'].update(received=collector.received,dropped=collector.dropped)
        value['collector'].update(acks_submitted=collector.acks_submitted,ack_errors=collector.ack_errors)
        value['collector'].update(write_errors=collector.write_errors,observer_errors=collector.observer_errors)
        return value

    @app.get('/api/events')
    def events(node:int|None=Query(default=None,ge=0,le=2),before:int|None=Query(default=None,ge=1),
               limit:int=Query(default=20,ge=1,le=50),user=Depends(auth)):
        rows=store.event_rows(node,before,limit)
        return dict(items=rows,next_before=rows[-1]['id'] if len(rows)==limit else None)

    @app.get('/api/events/{event_id}')
    def detail(event_id:int,user=Depends(auth)):
        value=store.event_detail(event_id)
        if not value: raise HTTPException(404,'这条事件不存在或已超出保留时间')
        return value

    @app.get('/api/series')
    def series(minutes:int=Query(default=10,ge=1,le=1440),user=Depends(auth)):
        return dict(items=store.series(minutes),resolution='约 1 秒；最多返回最新 600 个已上传摘要')

    @app.exception_handler(Exception)
    async def generic_error(request,error):
        return JSONResponse(status_code=500,content={'detail':'服务暂时不可用，请稍后重试'})

    return app

