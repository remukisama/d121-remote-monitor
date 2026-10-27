"""Build the exported F407 project with the actual Studio source inventory."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib,json,os
from pathlib import Path
import shutil,subprocess

ROOT=Path(__file__).resolve().parents[1]
PROJECT=ROOT/'firmware/star-stm32f407'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--toolchain',type=Path,help='Directory containing arm-none-eabi-gcc and objcopy')
    parser.add_argument('--jobs',type=int,default=8,choices=range(1,33))
    args=parser.parse_args()
    def tool(name):
        executable=name+('.exe' if os.name=='nt' else '')
        result=str(args.toolchain/executable) if args.toolchain else shutil.which(name)
        if not result or not Path(result).is_file():raise FileNotFoundError(name+' not found; set --toolchain')
        return str(Path(result).resolve())
    cc,objcopy=tool('arm-none-eabi-gcc'),tool('arm-none-eabi-objcopy')
    manifest=json.loads((PROJECT/'build-manifest.json').read_text())
    build=PROJECT/'build';build.mkdir(exist_ok=True)
    flags=['-mcpu=cortex-m4','-mthumb','-mfloat-abi=hard','-mfpu=fpv4-sp-d16',
           '-O0','-ffunction-sections','-fdata-sections','-Wall','-g','-gdwarf-2']
    cflags=['-D'+item for item in manifest['defines']]
    cflags+=['-I'+path for path in manifest['includes']]
    cflags+=['-include','rtconfig_preinc.h','-std=gnu11']
    def compile_one(name):
        source=Path(name)
        target=Path('build/obj')/source.with_suffix(source.suffix+'.o')
        (PROJECT/target).parent.mkdir(parents=True,exist_ok=True)
        extra=['-x','assembler-with-cpp','-I.','-Xassembler','-mimplicit-it=thumb'] if source.suffix in ['.S','.s'] else cflags
        process=subprocess.run([cc,*flags,*extra,'-c',source.as_posix(),'-o',target.as_posix()],cwd=PROJECT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace')
        if process.returncode:raise RuntimeError(name+'\n'+process.stdout)
        return target.as_posix()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        objects=list(pool.map(compile_one,manifest['sources']))
    command=[cc,*flags,'-T',manifest['linker_script'],'-Wl,--gc-sections,-Map,build/rtthread.map,--cref',
             '-o','build/rtthread.elf',*objects,'-lc','-lm']
    subprocess.run(command,cwd=PROJECT,check=True)
    subprocess.run([objcopy,'-O','binary','build/rtthread.elf','build/rtthread.bin'],cwd=PROJECT,check=True)
    image=build/'rtthread.bin'
    print(json.dumps({'compiled_sources':len(objects),'binary_bytes':image.stat().st_size,
                      'sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
                      'scope':'Build only; no board programming performed'}))

if __name__=='__main__':main()
