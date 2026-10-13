"""Create a dedicated subscriber credential; keep the hardware client ID untouched."""
from pathlib import Path
import json, ssl, urllib.request

root = Path(__file__).resolve().parents[1]
path = root / 'config.private.json'
cfg = json.loads(path.read_text(encoding='utf-8-sig'))
if cfg['bemfa']['username'] and cfg['bemfa']['password']:
    print('Dedicated MQTT credential already configured.')
else:
    body = json.dumps({'openID': cfg['bemfa']['uid']}).encode()
    request = urllib.request.Request('https://pro.bemfa.com/vs/web/v1/userSecretKey', data=body,
                                     headers={'Content-Type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(request, timeout=20, context=ssl.create_default_context()) as response:
            result = json.loads(response.read(65536))
        if result.get('code') != 0:
            raise ValueError('Credential creation rejected')
        data = result['data']
        if not isinstance(data.get('appID'), str) or not isinstance(data.get('secretKey'), str):
            raise ValueError('Unexpected credential response')
        # Re-read so concurrent user edits to WeChat/deployment settings are retained.
        cfg = json.loads(path.read_text(encoding='utf-8-sig'))
        cfg['bemfa']['username'], cfg['bemfa']['password'] = data['appID'], data['secretKey']
        path.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding='utf-8')
        print('Dedicated MQTT subscriber credential saved privately. Hardware credential unchanged.')
    except Exception as exc:
        print('Credential setup failed: ' + type(exc).__name__)
        raise SystemExit(1)
