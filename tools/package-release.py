#!/usr/bin/env python3
"""Package an explicit source/binary allowlist; never follow private asset links."""
import argparse, gzip, hashlib, io, json, re, subprocess, tarfile
from pathlib import Path
from tempfile import TemporaryDirectory
ROOT = Path(__file__).resolve().parent.parent
TARGETS = ('linux-x86_64', 'linux-aarch64', 'darwin-arm64', 'android-aarch64', 'android-armv7a', 'android-x86_64', 'windows-x64')
VERSION = re.search(r'#define VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', (ROOT/'include/version.h').read_text()).group(1)
CORE_LOCK = (ROOT/'core-integration/dtv-userland-core.revision').read_text(encoding='ascii').strip()
CORE_URL = 'https://github.com/Khronos31/dtv-userland-core.git'
LIBUSB_SHA256 = 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf'
PROGRAMS = ('pixd', 'pixctl', 'pix-ts')
TOP_FILES = {'Makefile', 'README.md', 'LICENSE', 'COPYING.libusb', 'THIRD_PARTY_NOTICES.md', 'AGENTS.md', '.gitignore', '.clang-format',
             'pix-dt181-consumer.cpp', 'pix-dt181-consumer.h', 'pix-dt181-receiver-adapter.cpp', 'pix-dt181-receiver-adapter.h',
             'pix-dt295-receiver-adapter.cpp', 'pix-dt295-receiver-adapter.h'}
EXTENSIONS = {'.c', '.h', '.cpp', '.sh', '.py', '.md', '.json', '.yml', '.rules', '.conf'}
SOURCE_DIRS = {'include','src','tests','tools','packaging','docs','.github','licenses','core-integration'}

SOURCE_EXTRA = {'packaging/pix-usb-permissions', 'core-integration/CMakeLists.txt', 'core-integration/dtv-userland-core.revision'}

def git_value(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

def core_snapshot(root):
    """Return (commit, tree, {path: (data, mode)}) of the pinned Core commit from a Git root."""
    commit = git_value(root, 'rev-parse', f'{CORE_LOCK}^{{commit}}')
    tree = git_value(root, 'rev-parse', f'{CORE_LOCK}^{{tree}}')
    data = subprocess.check_output(['git', '-C', str(root), 'archive', '--format=tar', CORE_LOCK])
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as archive:
        for item in archive.getmembers():
            if item.isdir(): continue
            if not item.isfile() or item.name.startswith('/') or '..' in Path(item.name).parts:
                raise ValueError(f'Unsupported Core archive member: {item.name}')
            files[item.name] = (archive.extractfile(item).read(), 0o755 if item.mode & 0o111 else 0o644)
    return commit, tree, files

def source_files():
    result = []
    for f in ROOT.rglob('*'):
        rel = f.relative_to(ROOT)
        if f.is_symlink() or not f.is_file(): continue
        if len(rel.parts) == 1 and rel.name in TOP_FILES:
            result.append((rel.as_posix(), f))
        elif rel.parts[0] in SOURCE_DIRS and (rel.parts[0] == 'licenses' or f.suffix in EXTENSIONS or rel.as_posix() in SOURCE_EXTRA):
            result.append((rel.as_posix(), f))
    return sorted(result)

def package(target, binary_dir=None, core_source_root=None, output_dir=None):
    out = Path(output_dir) if output_dir else ROOT/'dist'; out.mkdir(parents=True, exist_ok=True)
    prefix = f'pix-userland-{VERSION}-{target}'
    if target == 'source':
        files = source_files()
    else:
        if target not in TARGETS: raise ValueError('Unknown target')
        files = [(n, ROOT/n) for n in ('README.md','LICENSE','COPYING.libusb','THIRD_PARTY_NOTICES.md')]
        files += [(n,f) for n,f in source_files() if n.startswith(('packaging/','licenses/','docs/'))]
        b = Path(binary_dir)
        programs = [name + ('.exe' if target == 'windows-x64' else '') for name in PROGRAMS]
        missing = [name for name in programs if not (b/name).is_file()]
        if missing: raise ValueError(f'Missing release program(s): {", ".join(missing)}')
        files += [(name, b/name) for name in programs]
        files += [('licenses/'+f.name,f) for f in sorted((b/'licenses').glob('*')) if f.is_file()]
        if (b/'build-info.txt').exists(): files += [('build-info.txt',b/'build-info.txt')]
    commit = git_value(ROOT, 'rev-parse', 'HEAD')
    metadata = {'project':'pix-userland','version':VERSION,'target':target,'commit':commit,'libusb':'1.0.30','private':True,'libusb_linkage':'static','device_assets':False,'dtv_userland_core_commit':CORE_LOCK}
    members = [(name, path.read_bytes(), 0o755 if path.stat().st_mode & 0o111 else 0o644) for name,path in files]
    required = {'LICENSE','COPYING.libusb','THIRD_PARTY_NOTICES.md','licenses/musl-COPYRIGHT.txt','docs/releases.md','packaging/pix-usb-permissions'}
    if target == 'source':
        required |= {'core-integration/CMakeLists.txt','core-integration/dtv-userland-core.revision','core-integration/pixd.cpp','core-integration/tools/pixctl.cpp','core-integration/tools/pix_ts.cpp'}
    assert required <= {name for name, _, _ in members}, 'Missing package documentation or licenses'
    if target == 'source':
        usb = ROOT/'build/deps/libusb-1.0.30.tar.bz2'
        usb_bytes = usb.read_bytes()
        if hashlib.sha256(usb_bytes).hexdigest() != LIBUSB_SHA256: raise ValueError('libusb source checksum mismatch')
        with TemporaryDirectory(prefix='pix-core-') as temporary:
            core_root = Path(core_source_root).resolve() if core_source_root else Path(temporary)/'core-git'
            if not core_source_root:
                core_root.mkdir()
                subprocess.run(['git','-C',str(core_root),'init','-q'],check=True)
                subprocess.run(['git','-C',str(core_root),'remote','add','origin',CORE_URL],check=True)
                subprocess.run(['git','-C',str(core_root),'fetch','-q','--depth=1','origin',CORE_LOCK],check=True)
            core_commit, core_tree, core_files = core_snapshot(core_root)
        if core_commit != CORE_LOCK: raise ValueError('Core snapshot does not match the fixed pin')
        for required_core in ('LICENSE', 'PROVENANCE.json', 'CMakeLists.txt'):
            if required_core not in core_files: raise ValueError(f'Core snapshot lacks {required_core}')
        members += [('third_party/dtv-userland-core/'+name, data, mode) for name,(data,mode) in sorted(core_files.items())]
        members.append(('third_party/libusb-1.0.30.tar.bz2', usb_bytes, 0o644))
        source_id = {'repository_commit': commit, 'repository_tree': git_value(ROOT, 'rev-parse', 'HEAD^{tree}'),
                     'dtv_userland_core_commit': core_commit, 'dtv_userland_core_tree': core_tree,
                     'libusb_sha256': LIBUSB_SHA256}
        metadata.update(source_id)
    members.append(('release-info.json', json.dumps(metadata,indent=2).encode()+b'\n',0o644))
    if target == 'source':
        manifest = {'schema': 1, 'kind': 'corresponding-source', 'project': 'pix-userland', 'version': VERSION,
                    'private': True, **source_id, 'libusb_version': '1.0.30',
                    'files': {name: {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)} for name,data,_ in sorted(members)}}
        members.append(('source-manifest.json', json.dumps(manifest,sort_keys=True,indent=2).encode()+b'\n', 0o644))
        sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name,data,_ in sorted(members))
        members.append(('SHA256SUMS', sums.encode('ascii'), 0o644))
    path = out/(prefix+'.tar.gz')
    with path.open('wb') as raw, gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed, tarfile.open(fileobj=compressed,mode='w') as archive:
        for name,data,mode in members:
            info=tarfile.TarInfo(prefix+'/'+name);info.size=len(data);info.mode=mode;info.mtime=0
            archive.addfile(info,io.BytesIO(data))
    print(path)

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target',choices=(*TARGETS,'source'))
    parser.add_argument('binary_dir',nargs='?')
    parser.add_argument('--core-source-root', type=Path, help='Git root containing the pinned Core commit (source target; default: fetch from GitHub)')
    parser.add_argument('--output-dir', type=Path)
    args=parser.parse_args();package(args.target,args.binary_dir,args.core_source_root,args.output_dir)
