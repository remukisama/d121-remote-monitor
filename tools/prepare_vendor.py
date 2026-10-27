"""Apply checked project files to a fresh pinned SDK checkout, never the original workspace."""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def run(*args,cwd=None):return subprocess.check_output(list(args),cwd=cwd,text=True).strip()
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('component',choices=['d121','esp8266-at'])
    parser.add_argument('--destination',type=Path,required=True)
    parser.add_argument('--source',help='Optional existing vendor Git repository for an offline local clone')
    args=parser.parse_args()
    project=ROOT/'firmware'/args.component
    manifest=json.loads((project/'source-manifest.json').read_text())
    # Keep the ordinary Windows spelling for Git; Path.resolve() can return a
    # \\?\ extended path that Git rejects when the destination does not exist.
    destination=Path(os.path.abspath(args.destination))
    marker=destination/'.d121-overlay.json'
    manifest_hash=hashlib.sha256((project/'source-manifest.json').read_bytes()).hexdigest()
    def target(name):
        root=destination.resolve()
        path=(root/name).resolve()
        if not path.is_relative_to(root):raise ValueError('Path escapes SDK root: '+name)
        return path
    def verify():
        for item in manifest['files']:
            if hashlib.sha256(target(item['path']).read_bytes()).hexdigest()!=item['sha256']:
                raise ValueError('Applied file checksum mismatch: '+item['path'])
        if any(target(name).exists() for name in manifest['delete']):raise ValueError('Removed vendor file remains')
    if marker.exists():
        existing=json.loads(marker.read_text())
        if existing['base_commit']!=manifest['base_commit'] or existing.get('manifest_sha256')!=manifest_hash:
            raise ValueError('Different SDK marker or source manifest; use a fresh checkout')
        verify();print('Existing project overlay verified; configuration may have been edited');return
    if not destination.exists():
        destination.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run(['git','clone','--no-checkout',args.source or manifest['upstream'],str(destination)],check=True)
        subprocess.run(['git','-c','core.autocrlf=false','checkout','--detach',manifest['base_commit']],cwd=destination,check=True)
        subprocess.run(['git','config','core.autocrlf','false'],cwd=destination,check=True)
    if run('git','rev-parse','HEAD',cwd=destination)!=manifest['base_commit']:
        raise ValueError('Destination must be at pinned base '+manifest['base_commit'])
    if run('git','status','--porcelain','--untracked-files=normal',cwd=destination):
        raise ValueError('Refusing to overwrite a non-clean checkout; use a new destination')
    tracked=set(subprocess.check_output(['git','ls-files','-z'],cwd=destination).decode().split('\0'))
    # Preflight all source files and destination paths before changing a clean checkout.
    for item in manifest['files']:
        source=project/'overlay'/item['path']
        if hashlib.sha256(source.read_bytes()).hexdigest()!=item['sha256']:raise ValueError('Source checksum mismatch')
        path=target(item['path'])
        if path.exists() and (not path.is_file() or item['path'] not in tracked):
            raise ValueError('Refusing to overwrite an untracked or non-file path: '+item['path'])
    for item in manifest.get('config_seed',[]):
        source=project/'config-seed'/item['path']
        if hashlib.sha256(source.read_bytes()).hexdigest()!=item['sha256']:raise ValueError('Config checksum mismatch')
        path=target(item['path'])
        if path.exists():raise ValueError('Refusing to overwrite existing configuration: '+item['path'])
    for name in manifest['delete']:
        path=target(name)
        if path.exists() and (not path.is_file() or name not in tracked):
            raise ValueError('Deletion is not a tracked file: '+name)
    for name in manifest['delete']:
        path=target(name)
        if path.is_file():path.unlink()
    for item in manifest['files']:
        source=project/'overlay'/item['path']
        path=target(item['path']);path.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,path)
    for item in manifest.get('config_seed',[]):
        source=project/'config-seed'/item['path']
        shutil.copyfile(source,target(item['path']))
    verify();marker.write_text(json.dumps({'component':args.component,'base_commit':manifest['base_commit'],
                                          'manifest_sha256':manifest_hash})+'\n')
    print(json.dumps({'component':args.component,'destination':str(destination),'copied_files':len(manifest['files']),
                      'removed_files':len(manifest['delete']),'programmed_hardware':False}))

if __name__=='__main__':main()
