#!/usr/bin/env python3
"""Validate the complete release and each embedded build identity before upload."""
import argparse, hashlib, importlib.util, json, subprocess, tarfile
from pathlib import Path
spec=importlib.util.spec_from_file_location('pack',Path(__file__).with_name('package-release.py'))
pack=importlib.util.module_from_spec(spec);spec.loader.exec_module(pack)
parser=argparse.ArgumentParser();parser.add_argument('--with-assets',action='store_true');args=parser.parse_args()
root=pack.ROOT;out=root/'dist'
commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
expected={f'pix-userland-{pack.VERSION}-{t}'+'.tar.gz':t for t in (*pack.TARGETS,'source')}
assert {p.name for p in out.iterdir() if p.name!='SHA256SUMS'} == set(expected), 'Unexpected or missing release files'
for name,target in expected.items():
    path=out/name;prefix=f'pix-userland-{pack.VERSION}-{target}/'
    with tarfile.open(path) as t:
        assert all(m.isfile() for m in t.getmembers()), 'Only regular archive members permitted'
        assert len(t.getnames())==len(set(t.getnames())), 'Duplicate archive member'
        contents={m.name:t.extractfile(m).read() for m in t.getmembers()}
    for n in contents:
        assert n.startswith(prefix) and '..' not in Path(n).parts, 'Invalid archive member'
        assert not any(part in {'captures','.git','.private'} for part in Path(n).parts), 'Private data directory'
        assert not n.endswith(('.ts','.context','.mp4','.usb.bin')), 'Private capture data'
    meta=json.loads(contents[prefix+'release-info.json'])
    assert meta == {'project':'pix-userland','version':pack.VERSION,'target':target,'commit':commit,'libusb':'1.0.30','private':True,'libusb_linkage':'static','device_assets':bool(args.with_assets and target!='source')}, meta
    asset_names={n for n in contents if n.startswith(prefix+'assets/')}
    expected_assets={prefix+'assets/'+n:size for n,size in {'fpga.bin':699673,'bootstrap.bin':16,'validation.bin':64}.items()} if args.with_assets and target!='source' else {}
    assert asset_names==set(expected_assets), 'Incorrect device asset set'
    for n,size in expected_assets.items(): assert len(contents[n])==size
    assert not any(n.lower().endswith(('.dll','.dylib','.so')) for n in contents), 'Unexpected shared library'
    for n in ('LICENSE','COPYING.libusb','THIRD_PARTY_NOTICES.md','licenses/musl-COPYRIGHT.txt'):
        assert prefix+n in contents, f'Missing license notice: {target}/{n}'
    if target != 'source':
        assert prefix+('pix-ts.exe' if target=='windows-x64' else 'pix-ts') in contents
    else:
        assert hashlib.sha256(contents[prefix+'third_party/libusb-1.0.30.tar.bz2']).hexdigest() == 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf'
(out/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256((out/name).read_bytes()).hexdigest()}  {name}\n' for name in sorted(expected)))
print('Verified seven binary packages and matching source; SHA256SUMS created')
