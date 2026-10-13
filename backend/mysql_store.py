"""Durable cloud store. Container files are never the production history database."""
import threading, os
import pymysql
from .store import Store

DDL = [
 '''CREATE TABLE IF NOT EXISTS samples(id BIGINT AUTO_INCREMENT PRIMARY KEY, stamp DOUBLE NOT NULL, boot BIGINT,
 collection BIGINT, uptime DOUBLE, retained INT NOT NULL DEFAULT 0, identity VARCHAR(191) UNIQUE,payload LONGTEXT NOT NULL,
 INDEX samples_time(stamp), INDEX samples_identity(boot,collection,uptime))''',
 '''CREATE TABLE IF NOT EXISTS diagnostics(id BIGINT AUTO_INCREMENT PRIMARY KEY,stamp DOUBLE,boot BIGINT,collection BIGINT,
 uptime DOUBLE,payload LONGTEXT,INDEX diagnostics_identity(boot,collection,uptime))''',
 '''CREATE TABLE IF NOT EXISTS motions(id BIGINT AUTO_INCREMENT PRIMARY KEY,identity VARCHAR(191) UNIQUE,stamp DOUBLE NOT NULL,
 boot BIGINT,collection BIGINT,uptime DOUBLE,peak_mg INT,sample_count INT NOT NULL,
 INDEX motions_identity(boot,collection,uptime),INDEX motions_time(stamp))''',
 '''CREATE TABLE IF NOT EXISTS events(id BIGINT AUTO_INCREMENT PRIMARY KEY,identity VARCHAR(191) UNIQUE,received DOUBLE,
 occurred DOUBLE,boot BIGINT,collection BIGINT,node INT,uptime DOUBLE,title VARCHAR(128),source VARCHAR(32),
 payload LONGTEXT,snapshot LONGTEXT,diagnostic LONGTEXT,diagnosis LONGTEXT,resolved DOUBLE,anchor_uptime DOUBLE,anchor_time DOUBLE,
 INDEX events_time(occurred,id))''',
 'CREATE TABLE IF NOT EXISTS sessions(hash VARCHAR(64) PRIMARY KEY,expires DOUBLE,openid VARCHAR(128))',
 'CREATE TABLE IF NOT EXISTS members(openid VARCHAR(128) PRIMARY KEY,created DOUBLE)'
]

class Result:
    def __init__(self, rows, rowcount): self.rows, self.rowcount = rows, rowcount
    def fetchone(self): return self.rows[0] if self.rows else None
    def fetchall(self): return self.rows

class Connection:
    def __init__(self, cfg):
        address=os.environ.get('MYSQL_ADDRESS','')
        host,port=address.rsplit(':',1) if ':' in address else (address or cfg.get('host',''),cfg.get('port',3306))
        self.options=dict(host=host,port=int(port),user=os.environ.get('MYSQL_USERNAME',cfg.get('username','')),
            password=os.environ.get('MYSQL_PASSWORD',cfg.get('password','')),database=os.environ.get('MYSQL_DATABASE',cfg.get('database','d121_monitor')),
            charset='utf8mb4',connect_timeout=10,read_timeout=15,write_timeout=15,autocommit=False,
            cursorclass=pymysql.cursors.DictCursor)
        if not host: raise ValueError('Production MySQL address is required')
        self.connection=pymysql.connect(**self.options)
        self.in_transaction=False
    def execute(self,sql,args=()):
        if not self.in_transaction: self.connection.ping(reconnect=True)
        sql=sql.replace('INSERT OR IGNORE','INSERT IGNORE').replace('?', '%s')
        with self.connection.cursor() as cursor:
            cursor.execute(sql,args)
            rows=cursor.fetchall() if cursor.description else []
            return Result(rows,cursor.rowcount)
    def __enter__(self):
        self.connection.ping(reconnect=True); self.in_transaction=True; return self
    def __exit__(self,kind,error,tb):
        try:
            if kind: self.connection.rollback()
            else: self.connection.commit()
        finally: self.in_transaction=False
    def commit(self): self.connection.commit()
    def close(self): self.connection.close()

class MySQLStore(Store):
    def __init__(self,cfg,device_id='d121-01',offline_seconds=15):
        self.device_id,self.offline_seconds=device_id,offline_seconds
        self.lock=threading.RLock();self.linked=False;self.link_since=None;self.last_receive=None;self.receive_clock=None
        self.error=None;self.archive_error=None;self.rejected=0;self.db=Connection(cfg)
        with self.db:
            for statement in DDL: self.db.execute(statement)
