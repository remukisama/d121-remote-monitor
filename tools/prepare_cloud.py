"""Produce private runtime settings only after account/resource setup is complete."""
from pathlib import Path
import copy,json,secrets
root=Path(__file__).resolve().parents[1];path=root/'config.private.json'
cfg=json.loads(path.read_text(encoding='utf-8-sig'));missing=[]
for section,key in [('wechat','appid'),('wechat','appsecret'),('deployment','cloud_env_id')]:
    if not cfg.get(section,{}).get(key):missing.append(section+'.'+key)
if missing:
    print('Required before cloud deployment: '+', '.join(missing));raise SystemExit(2)
cloud=copy.deepcopy(cfg);server=cloud['server']
server.update(local_dev=False,database_driver='mysql',development_token='',pairing_code=server.get('pairing_code') or secrets.token_urlsafe(12))
if server.get('public_url') and not server['public_url'].startswith('https://'):
    print('server.public_url, when provided, must use HTTPS');raise SystemExit(2)
if not cfg['server'].get('pairing_code'):
    cfg['server']['pairing_code']=server['pairing_code']
    path.write_text(json.dumps(cfg,ensure_ascii=False,indent=2),encoding='utf-8')
cloud.setdefault('mysql',{}).setdefault('database','d121_monitor')
dest=root/'output';dest.mkdir(exist_ok=True)
(dest/'cloud-config.private.json').write_text(json.dumps(cloud,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
mysql=cloud.get('mysql',{})
address=(str(mysql.get('host',''))+':'+str(mysql.get('port',3306))) if mysql.get('host') else ''
env={'D121_CONFIG_JSON':json.dumps(cloud,ensure_ascii=False,separators=(',',':')),
     'MYSQL_ADDRESS':address,'MYSQL_USERNAME':mysql.get('username','root'),
     'MYSQL_PASSWORD':mysql.get('password',''),'MYSQL_DATABASE':mysql.get('database','d121_monitor')}
(dest/'cloud-env.private.json').write_text(json.dumps(env,ensure_ascii=False,indent=2),encoding='utf-8')
print('Private runtime config prepared for D121_CONFIG_JSON. No credentials printed.')
print('Cloud console JSON environment form prepared in output/cloud-env.private.json.')
