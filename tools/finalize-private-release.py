#!/usr/bin/env python3
"""Add only the three authorized assets to each private binary package."""
import gzip, importlib.util, io, json, sys, tarfile
from pathlib import Path
spec=importlib.util.spec_from_file_location('pack',Path(__file__).with_name('package-release.py'))
pack=importlib.util.module_from_spec(spec);spec.loader.exec_module(pack)
assets=Path(sys.argv[1]);expected={'fpga.bin':699673,'bootstrap.bin':16,'validation.bin':64}
blobs={n:(assets/n).read_bytes() for n in expected}
assert all(len(blobs[n])==size for n,size in expected.items())
for target in pack.TARGETS:
    prefix=f'pix-userland-{pack.VERSION}-{target}/';path=pack.ROOT/'dist'/(prefix[:-1]+'.tar.gz')
    with tarfile.open(path) as t:
        assert all(m.isfile() for m in t.getmembers())
        entries=[(m.name,t.extractfile(m).read(),m.mode) for m in t.getmembers()]
    assert not any('/assets/' in n for n,_,_ in entries), 'Package already finalized'
    updated=[]
    for n,data,mode in entries:
        if n==prefix+'release-info.json':
            meta=json.loads(data);meta['device_assets']=True;data=json.dumps(meta,indent=2).encode()+b'\n'
        updated.append((n,data,mode))
    updated += [(prefix+'assets/'+n,data,0o600) for n,data in blobs.items()]
    temporary=path.with_suffix('.tmp')
    with temporary.open('wb') as raw,gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed,tarfile.open(fileobj=compressed,mode='w') as t:
        for name,data,mode in updated:
            m=tarfile.TarInfo(name);m.size=len(data);m.mode=mode;m.mtime=0;t.addfile(m,io.BytesIO(data))
    temporary.replace(path)
print('Seven binary packages finalized with private assets; source unchanged')
