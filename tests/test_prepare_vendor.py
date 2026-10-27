"""Exercise SDK preparation on disposable repositories, never real source trees."""
import hashlib,json,subprocess,sys
from pathlib import Path
import pytest

SCRIPT=Path(__file__).resolve().parents[1]/'tools/prepare_vendor.py'

def run(*args,cwd=None,check=True):
    return subprocess.run(list(args),cwd=cwd,check=check,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)

@pytest.fixture
def fixture_repo(tmp_path):
    vendor=tmp_path/'vendor';vendor.mkdir()
    run('git','init','-q',str(vendor))
    run('git','config','user.name','Test',cwd=vendor)
    run('git','config','user.email','test@example.invalid',cwd=vendor)
    (vendor/'keep.txt').write_bytes(b'old\n')
    (vendor/'delete.txt').write_bytes(b'vendor\n')
    run('git','add','.',cwd=vendor);run('git','commit','-qm','base',cwd=vendor)
    base=run('git','rev-parse','HEAD',cwd=vendor).stdout.strip()
    root=tmp_path/'project';(root/'tools').mkdir(parents=True)
    (root/'tools/prepare_vendor.py').write_bytes(SCRIPT.read_bytes())
    component=root/'firmware/d121';(component/'overlay').mkdir(parents=True)
    (component/'config-seed').mkdir()
    replacement=b'new project source\n';seed=b'configuration\n'
    (component/'overlay/keep.txt').write_bytes(replacement)
    (component/'config-seed/.config').write_bytes(seed)
    manifest={'upstream':str(vendor),'base_commit':base,
              'files':[{'path':'keep.txt','sha256':hashlib.sha256(replacement).hexdigest()}],
              'delete':['delete.txt'],
              'config_seed':[{'path':'.config','sha256':hashlib.sha256(seed).hexdigest()}]}
    (component/'source-manifest.json').write_text(json.dumps(manifest),encoding='utf-8')
    return root,vendor,manifest

def invoke(root,destination,source=None,check=True):
    args=[sys.executable,str(root/'tools/prepare_vendor.py'),'d121','--destination',str(destination)]
    if source is not None:args+=['--source',str(source)]
    return run(*args,check=check)

def test_fresh_clone_applies_and_rechecks(fixture_repo,tmp_path):
    root,vendor,_=fixture_repo;target=tmp_path/'new-sdk'
    invoke(root,target,vendor)
    assert (target/'keep.txt').read_bytes()==b'new project source\n'
    assert not (target/'delete.txt').exists()
    assert (target/'.config').read_bytes()==b'configuration\n'
    assert 'verified' in invoke(root,target).stdout

def test_refuses_dirty_existing_checkout(fixture_repo,tmp_path):
    root,vendor,_=fixture_repo;target=tmp_path/'existing-sdk'
    run('git','clone','-q',str(vendor),str(target))
    (target/'keep.txt').write_text('user changes\n')
    result=invoke(root,target,check=False)
    assert result.returncode and 'non-clean checkout' in result.stderr
    assert (target/'keep.txt').read_text()=='user changes\n'
    assert (target/'delete.txt').exists()

def test_rejects_escaped_path_before_deleting_anything(fixture_repo,tmp_path):
    root,vendor,manifest=fixture_repo;component=root/'firmware/d121'
    data=b'outside\n';(component/'escape.txt').write_bytes(data)
    manifest['files'].append({'path':'../escape.txt','sha256':hashlib.sha256(data).hexdigest()})
    (component/'source-manifest.json').write_text(json.dumps(manifest),encoding='utf-8')
    target=tmp_path/'new-sdk';result=invoke(root,target,vendor,check=False)
    assert result.returncode and 'escapes SDK root' in result.stderr
    assert (target/'delete.txt').read_bytes()==b'vendor\n'
    assert (target/'keep.txt').read_bytes()==b'old\n'

def test_refuses_existing_ignored_configuration(fixture_repo,tmp_path):
    root,vendor,_=fixture_repo;target=tmp_path/'existing-sdk'
    run('git','clone','-q',str(vendor),str(target))
    (target/'.git/info/exclude').write_text('.config\n')
    (target/'.config').write_text('user local configuration\n')
    result=invoke(root,target,check=False)
    assert result.returncode and 'existing configuration' in result.stderr
    assert (target/'.config').read_text()=='user local configuration\n'
    assert (target/'delete.txt').exists()
