"""Check the public project snapshot and the shared three-node protocol."""
import hashlib,json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    count=0
    for component in ['d121','esp8266-at']:
        base=ROOT/'firmware'/component
        manifest=json.loads((base/'source-manifest.json').read_text(encoding='utf-8'))
        for key,folder in [('files','overlay'),('config_seed','config-seed')]:
            for item in manifest.get(key,[]):
                path=(base/folder/item['path']).resolve()
                if not path.is_relative_to((base/folder).resolve()) or sha(path)!=item['sha256']:
                    raise ValueError('Incorrect overlay or seed: '+item['path'])
                count+=1
    manifest=json.loads((ROOT/'firmware/star-stm32f407/build-manifest.json').read_text(encoding='utf-8'))
    for item in manifest['source_files']:
        path=(ROOT/item['path']).resolve()
        if not path.is_relative_to(ROOT.resolve()) or sha(path)!=item['sha256']:
            raise ValueError('Incorrect Star source: '+item['path'])
        count+=1
    project=ROOT/'firmware/star-stm32f407'
    if len(manifest['sources'])!=166 or not all((project/p).is_file() for p in manifest['sources']):
        raise ValueError('Star build inventory incomplete')
    paths=[ROOT/'firmware/common/modbus_rtu.h',
           ROOT/'firmware/d121/overlay/application/rt-thread/helloworld/rs485/modbus_rtu.h',
           ROOT/'firmware/star-stm32f407/applications/modbus_rtu.h',
           ROOT/'firmware/loongson-slave2/modbus_rtu.h']
    if len({sha(path) for path in paths})!=1:raise ValueError('Modbus shared headers differ')
    print(json.dumps({'source_files_checked':count,'star_build_units':len(manifest['sources']),
                      'shared_modbus_headers_equal':True,'hardware_tested':False}))

if __name__=='__main__':main()
