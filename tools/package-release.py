#!/usr/bin/env python3
"""Package an explicit source/binary allowlist; never follow private asset links."""
import argparse, hashlib, io, json, re, subprocess, tarfile, zipfile
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
TARGETS = ('linux-x86_64', 'linux-aarch64', 'darwin-arm64', 'android-aarch64', 'android-armv7a', 'android-x86_64', 'windows-x64')
VERSION = re.search(r'#define VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', (ROOT/'include/version.h').read_text()).group(1)
TOP_FILES = {'Makefile', 'README.md', 'LICENSE', 'COPYING.libusb', 'AGENTS.md', '.gitignore', '.clang-format'}
EXTENSIONS = {'.c', '.h', '.sh', '.py', '.md', '.json', '.yml', '.rules', '.conf'}
SOURCE_DIRS = {'include','src','tests','tools','packaging','docs','.github'}

def source_files():
    result = []
    for f in ROOT.rglob('*'):
        rel = f.relative_to(ROOT)
        if f.is_symlink() or not f.is_file(): continue
        if len(rel.parts) == 1 and rel.name in TOP_FILES:
            result.append((str(rel), f))
        elif rel.parts[0] in SOURCE_DIRS and (f.suffix in EXTENSIONS or str(rel) == 'packaging/pix-usb-permissions'):
            result.append((str(rel), f))
    return sorted(result)

def package(target, binary_dir=None):
    out = ROOT/'dist'; out.mkdir(exist_ok=True)
    prefix = f'pix-userland-{VERSION}-{target}'
    if target == 'source':
        files = source_files()
    else:
        if target not in TARGETS: raise ValueError('Unknown target')
        files = [(n, ROOT/n) for n in ('README.md','LICENSE','COPYING.libusb')]
        files += [(n,f) for n,f in source_files() if n.startswith('packaging/') or n == 'docs/releases.md']
        b = Path(binary_dir)
        files += [('pix-ts.exe' if target == 'windows-x64' else 'pix-ts', b/('pix-ts.exe' if target == 'windows-x64' else 'pix-ts'))]
        if target == 'windows-x64': files += [('libusb-1.0.dll',b/'libusb-1.0.dll')]
        if (b/'build-info.txt').exists(): files += [('build-info.txt',b/'build-info.txt')]
    commit = subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    metadata = json.dumps({'project':'pix-userland','version':VERSION,'target':target,'commit':commit,'libusb':'1.0.30','private':True},indent=2).encode()+b'\n'
    members = [(name, path.read_bytes(), 0o755 if path.stat().st_mode & 0o111 else 0o644) for name,path in files]
    members.append(('release-info.json', metadata,0o644))
    if target == 'source':
        usb = ROOT/'build/deps/libusb-1.0.30.tar.bz2'
        if hashlib.sha256(usb.read_bytes()).hexdigest() != 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf': raise ValueError('libusb source checksum mismatch')
        members.append(('third_party/libusb-1.0.30.tar.bz2',usb.read_bytes(),0o644))
    if target == 'windows-x64':
        path = out/(prefix+'.zip')
        with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as archive:
            for name,data,mode in members:
                info=zipfile.ZipInfo(prefix+'/'+name,(2020,1,1,0,0,0));info.external_attr=(0o100000|mode)<<16;info.compress_type=zipfile.ZIP_DEFLATED
                archive.writestr(info,data)
    else:
        path = out/(prefix+'.tar.gz')
        import gzip
        with path.open('wb') as raw, gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed, tarfile.open(fileobj=compressed,mode='w') as archive:
            for name,data,mode in members:
                info=tarfile.TarInfo(prefix+'/'+name);info.size=len(data);info.mode=mode;info.mtime=0
                archive.addfile(info,io.BytesIO(data))
    print(path)

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target',choices=(*TARGETS,'source'))
    parser.add_argument('binary_dir',nargs='?')
    args=parser.parse_args();package(args.target,args.binary_dir)
