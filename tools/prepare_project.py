"""Generate IDE/cloud routing settings; copy no account secrets into the client."""
from pathlib import Path
import json
root=Path(__file__).resolve().parents[1]
cfg=json.loads((root/'config.private.json').read_text(encoding='utf-8-sig'))
mini=root/'miniprogram'; mini.mkdir(exist_ok=True)
project=dict(description='D121 双从站远程监控与历史事件',projectname='D121远程监控',appid=cfg['wechat'].get('appid') or 'touristappid',
             compileType='miniprogram',libVersion='latest',miniprogramRoot='./',
             setting=dict(es6=True,enhance=True,minified=True,urlCheck=True,postcss=True),
             packOptions=dict(ignore=[],include=[]))
(mini/'project.config.json').write_text(json.dumps(project,ensure_ascii=False,indent=2),encoding='utf-8')
deployment=cfg.get('deployment',{})
env=deployment.get('cloud_env_id','')
route=dict(mode='cloud' if env else 'local',baseUrl='http://127.0.0.1:8765',envId=env,service=deployment.get('cloud_service') or 'd121-monitor')
(mini/'config.js').write_text('// Public routing information only. No account secrets.\nmodule.exports = '+json.dumps(route,ensure_ascii=False)+'\n',encoding='utf-8')
# Simulator-only local override; deliberately kept out of version control.
(mini/'project.private.config.json').write_text(json.dumps(dict(projectname='D121远程监控',setting=dict(urlCheck=bool(env),hotReload=True)),ensure_ascii=False,indent=2),encoding='utf-8')
print('IDE project prepared:',mini)
print('Routing mode:',route['mode'])
