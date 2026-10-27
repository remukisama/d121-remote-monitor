"""Make an OS/rodata-only update from a specified, built D121 SDK."""
import argparse,json,subprocess,sys
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--sdk',type=Path,required=True,help='Prepared and built D121 SDK directory')
args=parser.parse_args()
sdk=args.sdk.resolve()
sys.path.insert(0,str(sdk/'tools/scripts'))
import mk_image
solution=(sdk/'.defconfig').read_text().strip().replace('_defconfig','')
images=sdk/'output'/solution/'images'
cfg=mk_image.parse_image_cfg(str(images/'image_cfg.json'))
cfg['image']['target']={key:cfg['image']['target'][key] for key in ('os','rodata')}
cfg['image']['info']['version']+='-update'
temporary=images/'application_update_cfg.json'
temporary.write_text(json.dumps(cfg,indent=2),encoding='utf-8')
subprocess.run([sys.executable,str(sdk/'tools/scripts/mk_image.py'),'-c',str(temporary),'-d',str(images)],check=True)
result=images/mk_image.img_gen_fw_file_name(cfg)
if not result.is_file():raise RuntimeError('Generated image is missing')
print(result)
