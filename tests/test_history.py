import json, time
import pytest
from backend.protocol import decode
from backend.store import Store

CFG={'state_topic':'state','event_topic':'event'}
def state(t,online=True,boot=1,collection=1,running=1):
    return decode('state',json.dumps({'v':2,'d':[boot,collection,t,running,[int(online),int(online),33,0 if online else None,0 if online else None],[1,1,15,0,32768,0,1234]]}).encode(),CFG)[1]

@pytest.fixture
def store(tmp_path):
    s=Store(tmp_path/'history.db'); yield s; s.close()

def test_zero_values_and_validation():
    s=state(1); assert s['nodes'][0]['temperature']==0 and s['nodes'][1]['cpu']==0
    for raw in [b'{"v":2,"d":[1]}',b'{"source":"PC","v":2,"d":[]}',b'{"v":2,"d":[true,1,1,1,[1,1,33,0,0],[1,1,15,0,0,0,0]]}']:
        with pytest.raises(ValueError): decode('state',raw,CFG)

def test_pre_outage_snapshot_skips_online_but_invalid_sensor_reading(store):
    store.ingest('state',state(10),1000)
    invalid=state(11)
    invalid['nodes'][0].update(flags=0,temperature=None,humidity=None)
    store.ingest('state',invalid,1001)
    kind,event=decode('event',b'{"v":1,"d":[1,1,1,1,1,7,1,12000]}',CFG)
    store.ingest(kind,event,1002)
    saved=store.event_rows()[0]['snapshot']
    assert saved['uptime_s']==10 and saved['nodes'][0]['temperature']==0

def test_master_outage_snapshot_keeps_each_nodes_own_valid_reading_time(store):
    store.ingest('state',state(10),1000)
    partial=state(11)
    partial['nodes'][0].update(flags=0,temperature=None,humidity=None)
    partial['nodes'][1]['cpu']=50
    store.ingest('state',partial,1001)
    store.add_event(dict(boot=1,collection=1,node=0,title='主站上传中断',uptime_s=11,
        source='heartbeat',identity='test-master-outage'),1016)
    saved=store.event_rows()[0]['snapshot']
    assert saved['nodes'][0]['temperature']==0 and saved['nodes'][1]['cpu']==50
    assert saved['node_received_at']==[1000,1001]

def test_event_preceding_data_not_overwritten_by_later_sample(store):
    store.ingest('state',state(10),1000);store.ingest('state',state(11,False),1001)
    kind,event=decode('event',b'{"v":1,"d":[1,1,1,1,1,7,1,11000]}',CFG)
    store.ingest(kind,event,1010)
    store.ingest('state',state(12),1011)
    rows=store.event_rows();assert len(rows)==1
    detail=store.event_detail(rows[0]['id'])
    assert detail['snapshot']['uptime_s']==10 and detail['snapshot']['nodes'][0]['temperature']==0
    assert detail['source']=='device' and detail['occurred']==1001
    assert detail['diagnosis']['confidence']=='原因尚未确认'

def test_duplicate_and_replay_after_restart(tmp_path):
    path=tmp_path/'persist.db';s=Store(path)
    s.ingest('state',state(10),1000)
    kind,event=decode('event',b'{"v":1,"d":[1,1,2,1,1,1,2,10000]}',CFG)
    s.ingest(kind,event,1001);s.ingest(kind,event,1002);s.close()
    s=Store(path);assert len(s.event_rows())==1 and s.event_rows()[0]['snapshot']['uptime_s']==10;s.close()

def test_no_cross_boot_or_collection_snapshot(store):
    store.ingest('state',state(10,boot=2),1000)
    kind,event=decode('event',b'{"v":1,"d":[1,1,2,1,1,1,2,10000]}',CFG)
    store.ingest(kind,event,1001)
    row=store.event_rows()[0];assert row['snapshot'] is None and row['occurred'] is None

def test_backend_disconnected_is_not_device_offline(store):
    store.ingest('state',state(10),1000);store.receive_clock=time.monotonic()-20
    store.connection(False,'collector interrupted');store.check_offline()
    assert store.status()['state']=='unknown' and not store.event_rows()
    store.connection(True);store.link_since=time.monotonic()-20;store.check_offline()
    assert store.status()['state']=='offline' and len(store.event_rows())==1
    store.check_offline();assert len(store.event_rows())==1
    store.ingest('state',state(40),1030);assert store.event_rows()[0]['resolved']==1030

def test_retained_message_does_not_prove_online(store):
    store.connection(True);store.ingest('state',state(10),1000,retained=True)
    assert store.status()['state']=='waiting';store.check_offline();assert not store.event_rows()

def test_diagnostic_is_not_an_event(store):
    kind,value=decode('event',b'{"v":3,"d":[1,1,10,2,0,0,100,3,1,420]}',CFG)
    store.ingest(kind,value,1000)
    assert store.status()['diagnostic']['storage']=='TF 卡' and not store.event_rows()

def test_capture_stop_is_not_node_failure(store):
    store.ingest('state',state(10),1000);store.ingest('state',state(11,False,running=0),1001)
    assert not store.event_rows()

def test_event_pagination_is_stable(store):
    for i in range(1,25):
        kind,event=decode('event',json.dumps({'v':1,'d':[1,1,2,1,1,i,1,i*1000]}).encode(),CFG)
        store.ingest(kind,event,1000+i)
    first=store.event_rows(limit=20);second=store.event_rows(before=first[-1]['id'],limit=20)
    assert len(first)==20 and len(second)==4 and not {r['id'] for r in first}&{r['id'] for r in second}

def test_quality_bits_hide_untrustworthy_numbers():
    raw={'v':2,'d':[1,1,1,1,[1,1,40,100,200],[1,1,1,0,32768,12,30]]}
    value=decode('state',json.dumps(raw).encode(),CFG)[1]
    assert value['nodes'][0]['temperature'] is None and value['nodes'][1]['cpu']==0
    assert value['nodes'][1]['memory_mb'] is None and value['nodes'][1]['uptime_s'] is None

def test_recovery_boot_observation_does_not_invent_power_loss(store):
    store.connection(True);store.ingest('state',state(10),1000)
    store.receive_clock=store.link_since=time.monotonic()-20;store.check_offline(1020)
    store.ingest('state',state(1,boot=2),1030)
    row=store.event_rows()[0]
    assert '重新启动' in row['diagnosis']['confidence']
    assert any('不能证明是断电' in text for text in row['diagnosis']['checks'])
