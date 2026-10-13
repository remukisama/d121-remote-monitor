"""Check staged source against this machine's actual private credentials."""
import json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
cfg=json.loads((root/'config.private.json').read_text(encoding='utf-8-sig'))
keys={'bemfa':('uid','username','password'),'wechat':('appsecret',),
      'server':('development_token','pairing_code'),'mysql':('password',)}
secrets=[(section+'.'+key,str(cfg.get(section,{}).get(key,''))) for section,fields in keys.items() for key in fields]
secrets=[(key,value.encode()) for key,value in secrets if len(value)>=8]
files=subprocess.check_output(['git','diff','--cached','--name-only','--diff-filter=ACM','-z'],cwd=root).decode().split('\0')
for name in filter(None,files):
    blob=subprocess.check_output(['git','show',':'+name],cwd=root)
    for key,value in secrets:
        if value in blob:raise SystemExit('Private credential present in staged file: '+name+' ('+key+')')
print('PASS: staged source excludes actual local account credentials')
