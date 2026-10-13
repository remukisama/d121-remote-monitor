import json,sqlite3,threading,time
import pytest
from backend.protocol import decode
from backend.store import Store
from backend.collector import Collector
from tests.test_history import CFG,state

def context(kind=5,boot=1,col=1,t=10):
    d=([boot,col,t,1,[1,1,33,0,0],[1,1,15,0,32768,0,1234]] if kind==5 else
       [boot,col,t,0,10] if kind==7 else [boot,col,t,2,0,0,50,0,0,400])
    return decode('event',json.dumps(dict(v=kind,d=d)).encode(),CFG)

def event():return decode('event',b'{"v":1,"d":[1,1,1,1,1,7,1,12000]}',CFG)

def test_replay_does_not_update_live_status_or_close_a_heartbeat_incident(tmp_path):
    s=Store(tmp_path/'offline.db');s.connection(True);s.ingest('state',state(100,boot=2),time.time())
    s.receive_clock=s.link_since=time.monotonic()-20;s.check_offline()
    before=s.receive_clock
    for kind in (5,7,8):s.ingest(*context(kind),time.time())
    assert s.receive_clock==before and s.status()['state']=='offline'
    assert s.latest()['boot']==2 and len(s.event_rows())==1 and s.event_rows()[0]['resolved'] is None
    s.close()

def test_replayed_context_survives_restart_and_supplies_zero_snapshot_with_known_time(tmp_path):
    path=tmp_path/'context.db';s=Store(path);stamp=time.time()
    s.ingest('state',state(100),stamp)
    for kind in (5,7,8):s.ingest(*context(kind),stamp+1)
    s.close();s=Store(path);s.ingest(*event(),stamp+2);s.ingest(*event(),stamp+3)
    detail=s.event_detail(s.event_rows()[0]['id'])
    assert len(s.event_rows())==1 and detail['snapshot']['replayed'] and detail['snapshot']['time_estimated']
    assert detail['snapshot']['nodes'][0]['temperature']==0 and detail['snapshot']['nodes'][0]['vibration']==0
    assert detail['diagnostic']['storage']=='TF 卡' and detail['occurred']==pytest.approx(stamp-88)
    assert detail['available_points']==1 and detail['points'][0]['uptime_s']==10
    assert s.latest()['uptime_s']==100 # Old replay must not become today's live reading.
    s.close()

def test_replay_without_a_calendar_anchor_never_fakes_occurrence_or_a_curve(tmp_path):
    s=Store(tmp_path/'unknown.db');stamp=time.time();s.ingest(*context(),stamp);s.ingest(*event(),stamp+1)
    d=s.event_detail(s.event_rows()[0]['id'])
    assert d['occurred'] is None and d['snapshot']['replayed'] and not d['snapshot']['time_estimated']
    assert d['snapshot']['received_at']==stamp and d['available_points']==0 and s.status()['state']=='waiting'
    s.close()

def test_replay_cannot_use_context_from_a_different_boot_or_collection(tmp_path):
    s=Store(tmp_path/'identity.db')
    for args in [dict(boot=2),dict(col=2),dict(t=13)]:s.ingest(*context(**args))
    s.ingest(*event());assert s.event_rows()[0]['snapshot'] is None;s.close()

def test_observed_incident_merge_keeps_original_id_and_newer_replayed_context(tmp_path):
    s=Store(tmp_path/'merged.db');stamp=time.time()
    s.ingest('state',state(5),stamp)
    offline=state(12);offline['nodes'][0]['online']=False;offline['nodes'][0]['fresh']=False
    s.ingest('state',offline,stamp+7)
    original=s.event_rows()[0];assert original['source']=='observed' and original['snapshot']['uptime_s']==5
    for kind in (5,7,8):s.ingest(*context(kind,t=10),stamp+9)
    s.ingest(*event(),stamp+10);s.ingest(*event(),stamp+11)
    d=s.event_detail(original['id'])
    assert len(s.event_rows())==1 and d['source']=='device' and d['uptime']==12
    assert d['received']==original['received'] and d['snapshot']['replayed'] and d['snapshot']['uptime_s']==10
    assert d['snapshot']['nodes'][0]['temperature']==0 and d['snapshot']['nodes'][0]['vibration']==0
    assert d['diagnostic']['storage']=='TF 卡' and d['diagnosis']['confidence']=='原因尚未确认'
    s.close()

@pytest.mark.parametrize('values',[[1,1,1],[1,1,5,1],[True,1,1,7],[1,1,1,-1]])
def test_invalid_ack_is_rejected(values):
    with pytest.raises(ValueError):decode('event',json.dumps(dict(v=6,d=values)).encode(),CFG)

def test_ack_only_after_real_database_commit_and_retry_is_deduplicated(tmp_path):
    path=tmp_path/'committed.db';s=Store(path);cfg=dict(CFG);collector=Collector(s,cfg,acknowledge=True)
    messages=[];ready=threading.Event()
    class Client:
        def publish(self,topic,payload,**kw):
            with sqlite3.connect(path) as independent:assert independent.execute('SELECT COUNT(*) FROM events').fetchone()[0]==1
            messages.append((topic,json.loads(payload),kw));ready.set()
            return type('Result',(),dict(rc=0))()
    collector.client=Client();thread=threading.Thread(target=collector.run);thread.start()
    try:
        raw=b'{"v":1,"d":[1,1,1,1,1,7,1,12000]}'
        for _ in range(2):
            ready.clear();collector.queue.put(('event',raw,time.time(),False));assert ready.wait(2)
        assert len(messages)==2 and messages[0]==('event/set',{'v':6,'d':[1,1,1,7]},{'qos':0,'retain':False})
        assert len(s.event_rows())==1
    finally:collector.stop_event.set();thread.join(2);s.close()

def test_failed_commit_and_retained_event_never_receive_ack(tmp_path):
    s=Store(tmp_path/'failure.db');collector=Collector(s,CFG,acknowledge=True);attempted=threading.Event();acks=[]
    class Client:
        def publish(self,*args,**kwargs):acks.append(args);return type('R',(),dict(rc=0))()
    collector.client=Client();original=s.ingest
    def fail(*args,**kwargs):attempted.set();raise OSError('disk full')
    s.ingest=fail;thread=threading.Thread(target=collector.run);thread.start()
    raw=b'{"v":1,"d":[1,1,1,1,1,7,1,12000]}'
    try:
        collector.queue.put(('event',raw,time.time(),False));assert attempted.wait(2)
        assert not acks
        s.ingest=original;collector.queue.put(('event',raw,time.time(),True))
        deadline=time.monotonic()+2
        while not s.event_rows() and time.monotonic()<deadline:time.sleep(.01)
        assert s.event_rows() and not acks
    finally:collector.stop_event.set();thread.join(2);s.close()

def test_local_collector_cannot_confirm_cloud_storage(tmp_path):
    s=Store(tmp_path/'local.db');collector=Collector(s,CFG)
    assert not collector.acknowledge;s.close()
