#!/usr/bin/env python3
"""Package built CLI/library artifacts with their header, license and hashes."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import zipfile

ROOT=Path(__file__).resolve().parents[1]
TARGETS=('windows-x64','linux-x64','android-arm64','android-x64')


def package(target):
    directory=ROOT/'bin'/target
    manifest=json.loads((directory/'build.json').read_text())
    if manifest.get('schema') != 2 or manifest.get('target') != target:
        raise ValueError('unsupported build manifest; rebuild before packaging')
    required={'include/rasterfold.h','lib/librasterfold.a'}
    if target=='windows-x64':
        required.update(('bin/rasterfold.exe','bin/rasterfold.dll',
                         'lib/librasterfold.dll.a','lib/rasterfold.lib',
                         'symbols/rasterfold.pdb','symbols/rasterfold-dll.pdb'))
    else:
        required.update(('bin/rasterfold','lib/librasterfold.so'))
        if target=='linux-x64':required.add('lib/librasterfold.so.1')
    artifacts=manifest.get('artifacts')
    if not isinstance(artifacts,dict) or required-set(artifacts):
        raise ValueError('incomplete build manifest; rebuild before packaging')
    files=[]
    for name,digest in artifacts.items():
        relative=PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name or relative.as_posix()!=name:
            raise ValueError(f'invalid artifact path: {name}')
        file=directory/name
        if not file.resolve().is_relative_to(directory.resolve()):
            raise ValueError(f'artifact escapes target: {name}')
        if hashlib.sha256(file.read_bytes()).hexdigest()!=digest:
            raise ValueError(f'{file}: build digest mismatch; rebuild before packaging')
        files.append((file,name))
    files += [(directory/'build.json','build.json'),(ROOT/'LICENSE','LICENSE'),(ROOT/'docs/building.md','docs/building.md'),(ROOT/'docs/library.md','docs/library.md')]
    dest=ROOT/'build/packages'/f'rasterfold-{target}-dev.zip';dest.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(dest,'w',compression=zipfile.ZIP_DEFLATED) as z:
        for file,name in files:
            info=zipfile.ZipInfo(f'rasterfold-{target}/'+name,date_time=(2026,1,1,0,0,0))
            info.external_attr=((0o100755 if file.name=='rasterfold' else 0o100644)<<16)
            info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,file.read_bytes())
    digest=hashlib.sha256(dest.read_bytes()).hexdigest()
    dest.with_suffix('.zip.sha256').write_text(f'{digest}  {dest.name}\n')
    print(f'{dest.relative_to(ROOT)}  {digest}')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=(*TARGETS,'all'),default='all');a=p.parse_args()
    for target in TARGETS if a.target=='all' else [a.target]:package(target)
