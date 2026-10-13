import json,time
import pytest
from backend.protocol import decode
from backend.store import Store
from tests.test_history import state,CFG

def motion(t,peak=125,count=10,boot=1,collection=1):
    return decode('event',json.dumps({'v':4,'d':[boot,collection,t,peak,count]}).encode(),CFG)[1]

def test_motion_can_arrive_before_state_and_survives_restart(tmp_path):
    path=tmp_path/'motion.db';store=Store(path)
    store.ingest('motion',motion(10),1000.2)
    assert store.status()['state']=='waiting' and not store.event_rows()
    store.ingest('state',state(10),1000)
    assert store.latest()['nodes'][0]['vibration']==.125
    store.close();store=Store(path)
    assert store.latest()['nodes'][0]['vibration']==.125
    store.close()

@pytest.mark.parametrize('flags,running,expected',[(33,1,0),(49,1,None),(32,1,None),(33,0,None)])
def test_motion_zero_and_invalid_or_stopped_sensor(tmp_path,flags,running,expected):
    store=Store(tmp_path/'quality.db')
    value=state(10,running=running);value['nodes'][0]['flags']=flags
    store.ingest('state',value,1000);store.ingest('motion',motion(10,peak=0),1000.1)
    assert store.latest()['nodes'][0]['vibration']==expected
    store.close()

def test_motion_has_exact_boot_collection_and_time_identity(tmp_path):
    store=Store(tmp_path/'identity.db');store.ingest('state',state(10),1000)
    for value in [motion(10,boot=2),motion(10,collection=2),motion(11)]:store.ingest('motion',value,1000.1)
    assert store.latest()['nodes'][0]['vibration'] is None
    store.ingest('motion',motion(10),1000.2);store.ingest('motion',motion(10),1000.3)
    assert store.latest()['nodes'][0]['vibration']==.125
    assert store.db.execute('SELECT COUNT(*) AS total FROM motions').fetchone()['total']==4
    store.close()

def test_unknown_boot_motion_does_not_attach_to_an_old_reboot(tmp_path):
    store=Store(tmp_path/'unknown.db');store.ingest('state',state(10,boot=0),1000)
    store.ingest('motion',motion(10,boot=0),900)
    assert store.latest()['nodes'][0]['vibration'] is None
    store.ingest('motion',motion(10,boot=0),1000.2)
    store.ingest('motion',motion(10,boot=0),1000.3)
    assert store.latest()['nodes'][0]['vibration']==.125
    assert len(store.series(minutes=1440))==0 # Synthetic old stamps are not recent data.
    store.close()

def test_motion_packets_do_not_hide_master_upload_interruption(tmp_path):
    store=Store(tmp_path/'heartbeat.db');store.connection(True);store.ingest('state',state(10),1000)
    store.receive_clock=store.link_since=time.monotonic()-20
    old_clock=store.receive_clock;store.ingest('motion',motion(10),1010)
    assert store.receive_clock==old_clock and store.status()['state']=='offline'
    store.check_offline(1020);assert len(store.event_rows())==1
    store.close()

def test_vibration_snapshot_and_curve_include_saved_motion_and_remain_immutable(tmp_path):
    store=Store(tmp_path/'events.db');stamp=time.time()
    value=state(10);value['nodes'][0].update(flags=41,temperature=None,humidity=None)
    store.ingest('state',value,stamp);store.ingest('motion',motion(10,peak=250),stamp+.1)
    kind,event=decode('event',b'{"v":1,"d":[1,1,1,1,1,7,4,10500]}',CFG)
    store.ingest(kind,event,stamp+.2)
    detail=store.event_detail(store.event_rows()[0]['id'])
    assert detail['snapshot']['nodes'][0]['vibration']==.25
    assert detail['snapshot']['nodes'][0]['temperature'] is None
    assert detail['points'][0]['nodes'][0]['vibration']==.25
    store.ingest('state',state(11),stamp+1);store.ingest('motion',motion(11,peak=750),stamp+1.1)
    assert store.event_detail(detail['id'])['snapshot']['nodes'][0]['vibration']==.25
    assert [frame['nodes'][0]['vibration'] for frame in store.series()]==[.25,.75]
    store.close()

@pytest.mark.parametrize('values',[[1,1,10,0,0],[1,1,10,None,1],[1,1,10,2501,1],[1,1,10,5,13],[1,1,10,False,1]])
def test_malformed_motion_cannot_create_fake_values(values):
    with pytest.raises(ValueError):decode('event',json.dumps({'v':4,'d':values}).encode(),CFG)
