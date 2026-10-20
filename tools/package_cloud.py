"""Package a secret-free deployable container source. Runtime configuration stays private."""
from pathlib import Path
import zipfile, hashlib
root=Path(__file__).resolve().parents[1];dest=root/'output';dest.mkdir(exist_ok=True)
archive=dest/'d121-wechat-cloud-source.zip'
paths=[root/'Dockerfile',root/'.dockerignore',root/'container.config.json',root/'requirements.lock.txt']
paths+=list((root/'backend').glob('*.py'))
paths.append(root/'tools/database_backup.py')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for file in paths:z.write(file,str(file.relative_to(root)))
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    assert not any('private' in name or '.env' in name or name.startswith('data/') for name in z.namelist())
print('Cloud container source:',archive)
print('SHA256:',hashlib.sha256(archive.read_bytes()).hexdigest())
