"""Allow the SDK's pinned pyparsing 2.3 on the isolated Python 3.10 host."""
import collections
import collections.abc
from pathlib import Path
import sysconfig

target=Path(sysconfig.get_path('purelib'))/'sitecustomize.py'
source='import collections, collections.abc\n'
for name in ('MutableMapping','Mapping','Sequence','MutableSequence','Iterable'):
    source+=f'if not hasattr(collections, {name!r}): collections.{name} = collections.abc.{name}\n'
if target.exists() and target.read_text()!=source:
    raise FileExistsError('Refusing to replace another host compatibility setup')
target.write_text(source)
print('Configured isolated Python compatibility')
