import json, sqlite3, threading, time
from pathlib import Path
from .protocol import diagnosis, NODE_NAMES

def dump(value): return json.dumps(value, ensure_ascii=False, separators=(',', ':'), allow_nan=False)

class Store:
    def __init__(self, path, device_id='d121-01', offline_seconds=15):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        self.device_id, self.offline_seconds = device_id, offline_seconds
        self.linked = False
        self.link_since = None
        self.last_receive = None
        self.receive_clock = None
        self.error = None
        self.archive_error = None
        self.rejected = 0
        self.db.executescript('''
          PRAGMA journal_mode=WAL;
          CREATE TABLE IF NOT EXISTS samples(id INTEGER PRIMARY KEY, stamp REAL NOT NULL, boot INTEGER, collection INTEGER,
            uptime REAL, retained INTEGER NOT NULL DEFAULT 0, identity TEXT UNIQUE, payload TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS samples_time ON samples(stamp);
          CREATE INDEX IF NOT EXISTS samples_identity ON samples(boot,collection,uptime);
          CREATE TABLE IF NOT EXISTS diagnostics(id INTEGER PRIMARY KEY, stamp REAL, boot INTEGER, collection INTEGER, uptime REAL, payload TEXT);
          CREATE INDEX IF NOT EXISTS diagnostics_identity ON diagnostics(boot,collection,uptime);
          CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, identity TEXT UNIQUE, received REAL, occurred REAL,
            boot INTEGER, collection INTEGER, node INTEGER, uptime REAL, title TEXT, source TEXT,
            payload TEXT, snapshot TEXT, diagnostic TEXT, diagnosis TEXT, resolved REAL, anchor_uptime REAL, anchor_time REAL);
          CREATE INDEX IF NOT EXISTS events_time ON events(occurred DESC,id DESC);
          CREATE TABLE IF NOT EXISTS sessions(hash TEXT PRIMARY KEY, expires REAL, openid TEXT);
          CREATE TABLE IF NOT EXISTS members(openid TEXT PRIMARY KEY, created REAL);
        ''')
        self.db.commit()

    def connection(self, connected, error=None):
        with self.lock:
            self.linked = connected
            self.error = error
            if connected: self.link_since = time.monotonic()

    def latest(self):
        row = self.db.execute('SELECT * FROM samples ORDER BY stamp DESC,id DESC LIMIT 1').fetchone()
        if not row: return None
        return dict(json.loads(row['payload']), received_at=row['stamp'], retained=bool(row['retained']))

    def ingest(self, kind, value, stamp=None, retained=False):
        stamp = time.time() if stamp is None else stamp
        with self.lock, self.db:
            if kind == 'diagnostic':
                self.db.execute('INSERT INTO diagnostics(stamp,boot,collection,uptime,payload) VALUES(?,?,?,?,?)',
                    (stamp, value['boot'], value['collection'], value['uptime_s'], dump(value)))
                return
            if kind == 'state':
                before = self.latest()
                identity = f"{value['boot']}:{value['collection']}:{value['uptime_s']}" if value['boot'] else f"uncertain:{stamp}"
                self.db.execute('INSERT OR IGNORE INTO samples(stamp,boot,collection,uptime,retained,identity,payload) VALUES(?,?,?,?,?,?,?)',
                    (stamp,value['boot'],value['collection'],value['uptime_s'],int(retained),identity,dump(value)))
                if retained: return
                self.last_receive, self.receive_clock = stamp, time.monotonic()
                # Recovery refers to telemetry recovery, not proof of the original outage cause.
                outages=self.db.execute("SELECT id,boot,diagnosis FROM events WHERE node=0 AND source='heartbeat' AND resolved IS NULL").fetchall()
                for outage in outages:
                    finding=json.loads(outage['diagnosis'])
                    if outage['boot'] and value['boot']:
                        if outage['boot']!=value['boot']:
                            finding['observations'].append('恢复上传后，主站的开机编号变了，说明中断期间主站发生过重新启动。')
                            finding['checks']=['检查主站供电、复位原因和看门狗日志；重新启动本身不能证明是断电造成的。']
                            finding['confidence']='已观察到主站重新启动，触发原因待确认'
                        else:
                            finding['observations'].append('恢复上传后仍是同一次开机，未观察到主站重新启动。应继续检查网络、ESP8266 连接或主动暂停上传的情况。')
                    self.db.execute('UPDATE events SET resolved=?,diagnosis=? WHERE id=?',(stamp,dump(finding),outage['id']))
                if before and before['boot'] == value['boot'] and before['collection'] == value['collection'] and before['running'] and value['running']:
                    for node, old, new in zip((1,2), before['nodes'], value['nodes']):
                        if old['online'] and not new['online']:
                            self.add_event(dict(boot=value['boot'],collection=value['collection'],node=node,title='通信中断',
                                uptime_s=value['uptime_s'],source='observed',identity=f"observed:{identity}:{node}"), stamp)
                return
            self.add_event(dict(value, source='device', identity=f"device:{value['boot']}:{value['collection']}:{value['node']}:{value['event_id']}"), stamp)

    def add_event(self, value, stamp):
        if self.db.execute('SELECT 1 FROM events WHERE identity=?',(value['identity'],)).fetchone(): return
        # Capture the last cloud-observed valid sample from the same boot and collection BEFORE the event.
        boot, collection, uptime, node = value['boot'], value['collection'], value['uptime_s'], value['node']
        row = self.db.execute('SELECT * FROM samples WHERE boot=? AND collection=? AND uptime<=? ORDER BY uptime DESC,id DESC LIMIT 1',
                              (boot, collection, uptime)).fetchone()
        snapshot = None
        if row:
            candidate = json.loads(row['payload'])
            # Find last usable node data if the immediate frame already says offline.
            rows = self.db.execute('SELECT * FROM samples WHERE boot=? AND collection=? AND uptime<=? ORDER BY uptime DESC,id DESC LIMIT 180',
                                    (boot, collection, uptime)).fetchall()
            def usable(n):
                fields=('temperature','humidity') if n['node']==1 else ('cpu','memory_mb','load','uptime_s')
                return n['online'] and n['fresh'] and any(n.get(field) is not None for field in fields)
            for prior in rows:
                candidate = json.loads(prior['payload'])
                good = candidate['running'] and (any(usable(n) for n in candidate['nodes']) if node==0 else
                    usable(candidate['nodes'][node-1]))
                if good:
                    snapshot = dict(candidate, received_at=prior['stamp']); break
            if node==0 and snapshot:
                # Each node may have last reported usable data at a different time.
                snapshot['nodes']=[dict(n) for n in snapshot['nodes']]
                snapshot['node_received_at']=[None,None]
                for position in (0,1):
                    for prior in rows:
                        candidate=json.loads(prior['payload'])
                        if candidate['running'] and usable(candidate['nodes'][position]):
                            snapshot['nodes'][position]=dict(candidate['nodes'][position])
                            snapshot['node_received_at'][position]=prior['stamp'];break
        occurred = stamp - (row['uptime'] - uptime) if row else None
        # Anchor to the closest frame in the same collection; buffered events may arrive much later.
        anchor = self.db.execute('SELECT stamp,uptime FROM samples WHERE boot=? AND collection=? ORDER BY ABS(uptime-?) LIMIT 1',
                                (boot, collection, uptime)).fetchone()
        if anchor: occurred = anchor['stamp'] + uptime - anchor['uptime']
        if value['source']=='heartbeat': occurred=stamp
        if occurred is not None and occurred > stamp: occurred = stamp
        diagrow = self.db.execute('SELECT payload FROM diagnostics WHERE boot=? AND collection=? AND uptime<=? ORDER BY uptime DESC LIMIT 1',
                                 (boot, collection, uptime)).fetchone()
        diag = json.loads(diagrow['payload']) if diagrow else None
        # A state transition and its following hardware event represent the same incident.
        if value['source'] == 'device':
            old = self.db.execute("SELECT id FROM events WHERE boot=? AND collection=? AND node=? AND title=? AND source='observed' AND ABS(uptime-?)<=5 ORDER BY id DESC LIMIT 1",
                                  (boot, collection, node, value['title'], uptime)).fetchone()
            if old:
                self.db.execute('UPDATE events SET identity=?,payload=?,source=?,uptime=? WHERE id=?',
                                 (value['identity'],dump(value),'device',uptime,old['id']))
                return
        if value['source']=='observed' and self.db.execute("SELECT 1 FROM events WHERE boot=? AND collection=? AND node=? AND title=? AND source='device' AND ABS(uptime-?)<=5",
                 (boot,collection,node,value['title'],uptime)).fetchone(): return
        if value['title'] == '通信恢复':
            self.db.execute('UPDATE events SET resolved=? WHERE boot=? AND collection=? AND node=? AND title=? AND resolved IS NULL',
                             (occurred or stamp, boot, collection, node, '通信中断'))
        self.db.execute('''INSERT OR IGNORE INTO events(identity,received,occurred,boot,collection,node,uptime,title,source,
            payload,snapshot,diagnostic,diagnosis,anchor_uptime,anchor_time) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)''',
            (value['identity'],stamp,occurred,boot,collection,node,uptime,value['title'],value['source'],dump(value),
             dump(snapshot) if snapshot else None,dump(diag) if diag else None,dump(diagnosis(value,snapshot,diag)),
             anchor['uptime'] if anchor else None,anchor['stamp'] if anchor else None))

    def check_offline(self, now=None):
        with self.lock, self.db:
            if not self.linked or self.archive_error or self.receive_clock is None or self.link_since is None: return
            # Use monotonic time to avoid classifying host clock corrections as outages.
            age = time.monotonic() - max(self.receive_clock,self.link_since)
            if age < self.offline_seconds: return
            current = self.latest()
            if not current or current['retained']: return
            if self.db.execute("SELECT 1 FROM events WHERE source='heartbeat' AND resolved IS NULL").fetchone(): return
            value = dict(boot=current['boot'],collection=current['collection'],node=0,title='主站上传中断',
                         uptime_s=current['uptime_s'],source='heartbeat',identity=f"heartbeat:{self.last_receive}")
            self.add_event(value, time.time() if now is None else now)

    def status(self):
        with self.lock:
            latest = self.latest()
            age = time.monotonic()-self.receive_clock if self.receive_clock is not None else None
            state = 'waiting' if not latest or self.receive_clock is None else 'unknown' if not self.linked or self.archive_error else 'offline' if age > self.offline_seconds else 'online'
            diag = self.db.execute('SELECT * FROM diagnostics ORDER BY id DESC LIMIT 1').fetchone()
            diagnostic = json.loads(diag['payload']) if diag else None
            if diagnostic and latest and (diagnostic['boot'],diagnostic['collection']) != (latest['boot'],latest['collection']): diagnostic=None
            return dict(device_id=self.device_id,state=state,age_s=round(age,1) if age is not None else None,
                collector=dict(connected=self.linked,error=self.archive_error or self.error,rejected=self.rejected),latest=latest,diagnostic=diagnostic,
                offline_seconds=self.offline_seconds,server_time=time.time())

    def event_rows(self, node=None, before=None, limit=20):
        with self.lock:
            clauses, args = [], []
            if node is not None: clauses.append('node=?'); args.append(node)
            if before is not None: clauses.append('id<?'); args.append(before)
            where = ' WHERE '+' AND '.join(clauses) if clauses else ''
            rows=self.db.execute('SELECT * FROM events'+where+' ORDER BY id DESC LIMIT ?',[*args,limit]).fetchall()
            return [self.event_view(r) for r in rows]

    @staticmethod
    def event_view(row):
        value={key:row[key] for key in ('id','received','occurred','boot','collection','node','uptime','title','source','resolved')}
        for key in ('snapshot','diagnostic','diagnosis'): value[key]=json.loads(row[key]) if row[key] else None
        value['name']=NODE_NAMES[value['node']]
        value['time_note']=('这是云端发现上传中断的时间，实际中断可能更早' if value['source']=='heartbeat' else
            '发生时间由主站运行时间推算；接收时间单独显示' if value['occurred'] is not None else
            '设备没有提供可确定的日期时间，这里显示云端收到记录的时间')
        return value

    def event_detail(self, identity):
        with self.lock:
            row=self.db.execute('SELECT * FROM events WHERE id=?',(identity,)).fetchone()
            if not row: return None
            result=self.event_view(row)
            points=self.db.execute('SELECT stamp,payload FROM samples WHERE boot=? AND collection=? AND uptime BETWEEN ? AND ? ORDER BY uptime LIMIT 181',
                                   (row['boot'],row['collection'],max(0,row['uptime']-60),row['uptime']+30)).fetchall()
            result['points']=[dict(json.loads(p['payload']), received_at=p['stamp']) for p in points]
            result['available_points']=len(points)
            result['sample_note']='云端约每秒摘要；未上传的六轴原始波形和断网期间样本不在这里'
            return result

    def series(self, minutes=10):
        with self.lock:
            # Latest bounded window, then chronological ordering; do not return the oldest 600 samples of a day.
            rows=self.db.execute('SELECT stamp,payload FROM samples WHERE stamp>=? ORDER BY stamp DESC LIMIT 600',
                                  (time.time()-minutes*60,)).fetchall()
            return [dict(json.loads(r['payload']),received_at=r['stamp']) for r in reversed(rows)]

    def prune(self, days):
        with self.lock, self.db:
            cutoff=time.time()-days*86400
            # Preserve samples around every retained event for the configured history duration.
            self.db.execute('DELETE FROM samples WHERE stamp<?',(cutoff,))
            self.db.execute('DELETE FROM diagnostics WHERE stamp<?',(cutoff,))
            self.db.execute('DELETE FROM events WHERE received<?',(cutoff,))
            self.db.execute('DELETE FROM sessions WHERE expires<?',(time.time(),))

    def close(self):
        with self.lock: self.db.close()
