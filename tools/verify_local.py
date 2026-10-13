from pathlib import Path
import argparse, json, urllib.request
parser=argparse.ArgumentParser()
parser.add_argument('--expected-storage',choices=['TF 卡','板载 Flash'],default='TF 卡')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
cfg=json.loads((root/'config.private.json').read_text(encoding='utf-8-sig'))
def get(path,token):
    req=urllib.request.Request('http://127.0.0.1:8765'+path,headers={'Authorization':'Bearer '+token})
    return json.load(urllib.request.urlopen(req,timeout=5))
req=urllib.request.Request('http://127.0.0.1:8765/api/auth/local',json.dumps({'code':cfg['server']['development_token']}).encode(),{'Content-Type':'application/json'})
token=json.load(urllib.request.urlopen(req,timeout=5))['token']
s=get('/api/status',token);events=get('/api/events?limit=50',token);points=get('/api/series?minutes=10',token)
print(json.dumps({'state':s['state'],'collector':s['collector'],'boot':s['latest']['boot'] if s['latest'] else None,
                  'nodes':s['latest']['nodes'] if s['latest'] else [],'diagnostic':s['diagnostic'],
                  'event_count':len(events['items']),'latest_points':len(points['items'])},ensure_ascii=False,indent=2))
assert s['state']=='online' and s['collector']['connected'] and s['latest']['nodes'][0]['online'] and s['latest']['nodes'][1]['online']
assert s['diagnostic'] and s['diagnostic']['storage']==args.expected_storage and s['diagnostic']['write_errors']==0
assert points['items'] and s['collector']['dropped']==0
for e in events['items']:
    if e['title']=='通信中断' and e['node']==1:
        d=get('/api/events/'+str(e['id']),token)
        print('Star outage archived: event='+str(e['id'])+', points='+str(d['available_points'])+', has_before='+str(d['snapshot'] is not None))
        assert d['snapshot'] and d['snapshot']['nodes'][0]['temperature'] is not None
        break
print('PASS: actual cloud subscription, durable history API and healthy two-node telemetry')
