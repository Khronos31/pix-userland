#!/usr/bin/env python3
"""Package an explicit source/binary allowlist; never follow private asset links."""
import argparse, hashlib, io, json, re, subprocess, tarfile
from pathlib import Path
from tempfile import TemporaryDirectory
ROOT = Path(__file__).resolve().parent.parent
TARGETS = ('linux-x86_64', 'linux-aarch64', 'darwin-arm64', 'android-aarch64', 'android-armv7a', 'android-x86_64', 'windows-x64')
VERSION = re.search(r'#define VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', (ROOT/'include/version.h').read_text()).group(1)
CORE_LOCK = '35249f85347093ca3b431cbe4a730b2366de7ada'
PX4_LOCK = '90b08dfe79b0b40e393f015975c761863830866a'
LIBUSB_SHA256 = 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf'
CORE_URL = 'https://github.com/Khronos31/dtv-userland-core.git'
PX4_URL = 'https://github.com/Khronos31/px4-userland.git'
TOP_FILES = {'Makefile', 'CMakeLists.txt', 'README.md', 'LICENSE', 'COPYING.libusb', 'THIRD_PARTY_NOTICES.md', 'PROVENANCE.md', 'AGENTS.md', '.gitignore', '.clang-format', 'pixd.cpp', 'pix_runtime.cpp', 'pix_runtime.h', 'pix-capture-consumer.cpp', 'pix-capture-consumer.h', 'pix-dt181-consumer.cpp', 'pix-dt181-consumer.h', 'pix-dt181-receiver-adapter.cpp', 'pix-dt181-receiver-adapter.h', 'pix-dt295-receiver-adapter.cpp', 'pix-dt295-receiver-adapter.h'}
EXTENSIONS = {'.c', '.h', '.hpp', '.cpp', '.sh', '.py', '.md', '.json', '.yml', '.rules', '.conf'}
SOURCE_DIRS = {'include','src','tests','tools','packaging','docs','.github','licenses','userland'}
OLD_BUILD_SOURCES = {
    'old-control-source/pix-capture-consumer/src/pid_filter.c',
    'old-control-source/pix-capture-consumer/src/control.c',
    'old-control-source/pix-capture-consumer/src/channel.c',
    'old-control-source/pix-capture-consumer/src/crypto.c',
    'old-control-source/pix-capture-consumer/src/platform.c',
    'old-control-source/pix-capture-consumer/src/protocol.c',
    'old-control-source/pix-capture-consumer/src/transport.c',
    'old-control-source/pix-capture-consumer/src/usb_selector.c',
    'old-control-source/pix-capture-consumer/src/usb_listing.c',
    'old-control-source/pix-capture-consumer/src/usb_libusb.c',
    'old-control-source/pix-capture-consumer/src/usb_log.c',
    'old-control-source/pix-capture-consumer/src/diagnostic.c',
    'old-control-source/pix-capture-consumer/src/dt295.c',
    'old-control-source/pix-capture-consumer/src/dt295_crypto.c',
    'old-control-source/pix-capture-consumer/src/dt295_routes.c',
    'old-control-source/pix-capture-consumer/src/dt295_stream.c',
    'old-control-source/pix-capture-consumer/src/ts_output.c',
    'old-control-source/pix-capture-consumer/src/dt295_capture.c',
}

def source_files():
    result = []
    for f in ROOT.rglob('*'):
        rel = f.relative_to(ROOT)
        if f.is_symlink() or not f.is_file(): continue
        if len(rel.parts) == 1 and rel.name in TOP_FILES:
            result.append((rel.as_posix(), f))
        elif rel.parts[0] == 'cmake' and rel.name.endswith('.revision'):
            result.append((rel.as_posix(), f))
        elif rel.parts[0] in SOURCE_DIRS and (rel.parts[0] == 'licenses' or f.suffix in EXTENSIONS or rel.as_posix() == 'packaging/pix-usb-permissions'):
            result.append((rel.as_posix(), f))
        elif rel.as_posix() in OLD_BUILD_SOURCES:
            result.append((rel.as_posix(), f))
    return sorted(result)

def git_value(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

def git_snapshot(root, revision, destination):
    commit = git_value(root, 'rev-parse', f'{revision}^{{commit}}')
    tree = git_value(root, 'rev-parse', f'{revision}^{{tree}}')
    data = subprocess.check_output(['git', '-C', str(root), 'archive', '--format=tar', revision])
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as archive:
        for item in archive.getmembers():
            if not item.isfile():
                if item.isdir(): continue
                raise ValueError(f'Unsupported dependency archive member: {item.name}')
            if item.name.startswith('/') or '..' in Path(item.name).parts or '\\' in item.name:
                raise ValueError(f'Unsafe dependency archive member: {item.name}')
            content = archive.extractfile(item).read()
            path = destination/item.name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            path.chmod(item.mode & 0o777)
    return commit, tree

def prepare_git_root(url, revision, destination):
    destination.mkdir(parents=True)
    subprocess.run(['git', '-C', str(destination), 'init', '-q'], check=True)
    subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin', url], check=True)
    subprocess.run(['git', '-C', str(destination), 'fetch', '--depth=1', 'origin', revision], check=True)

def inventory(files):
    return {name: {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}
            for name, data, _mode in files}

def package(target, binary_dir=None, core_source_root=None, px4_source_root=None, output_dir=None):
    out = Path(output_dir) if output_dir else ROOT/'dist'
    out.mkdir(parents=True, exist_ok=True)
    prefix = f'pix-userland-{VERSION}-{target}'
    if target == 'source':
        files = source_files()
    else:
        if target not in TARGETS: raise ValueError('Unknown target')
        files = [(n, ROOT/n) for n in ('README.md','LICENSE','COPYING.libusb','THIRD_PARTY_NOTICES.md')]
        files += [(n,f) for n,f in source_files() if n.startswith(('packaging/','licenses/','docs/'))]
        b = Path(binary_dir)
        suffix = '.exe' if target == 'windows-x64' else ''
        programs = [f'{name}{suffix}' for name in ('pixd', 'pixctl', 'pix-ts')]
        missing = [name for name in programs if not (b/name).is_file()]
        if missing: raise ValueError(f'Missing release program(s): {", ".join(missing)}')
        files += [(name,b/name) for name in programs]
        files += [('licenses/'+f.name,f) for f in sorted((b/'licenses').glob('*')) if f.is_file()]
        if (b/'build-info.txt').exists(): files += [('build-info.txt',b/'build-info.txt')]
    commit = git_value(ROOT, 'rev-parse', 'HEAD')
    source_id = {}
    if target == 'source':
        if (core_source_root is None) != (px4_source_root is None):
            raise ValueError('Core and PX4 source roots must be supplied together')
        for path, expected in (('cmake/dtv-userland-core.revision', CORE_LOCK),
                               ('cmake/px4-userland.revision', PX4_LOCK)):
            if (ROOT/path).read_text(encoding='ascii').strip() != expected:
                raise ValueError(f'{path} does not match the fixed source closure pin')
        with TemporaryDirectory(prefix='pix-source-') as temporary:
            stage = Path(temporary)
            if core_source_root is None:
                core_dir = stage/'core-git'
                px4_dir = stage/'px4-git'
                prepare_git_root(CORE_URL, CORE_LOCK, core_dir)
                prepare_git_root(PX4_URL, PX4_LOCK, px4_dir)
            else:
                core_dir = Path(core_source_root).resolve()
                px4_dir = Path(px4_source_root).resolve()
            core_commit, core_tree = git_snapshot(core_dir, CORE_LOCK, stage/'third_party/dtv-userland-core')
            px4_commit, px4_tree = git_snapshot(px4_dir, PX4_LOCK, stage/'third_party/px4-userland')
            if core_commit != CORE_LOCK or px4_commit != PX4_LOCK:
                raise ValueError('dependency archive does not match the fixed source closure pins')
            for name, path in files:
                if not path.is_file(): raise ValueError(f'Missing source file: {name}')
            members = [(name, path.read_bytes(), 0o755 if path.stat().st_mode & 0o111 else 0o644)
                       for name, path in files]
            for base, directory in (('third_party/dtv-userland-core', stage/'third_party/dtv-userland-core'),
                                    ('third_party/px4-userland', stage/'third_party/px4-userland')):
                members.extend((f'{base}/{path.relative_to(directory).as_posix()}', path.read_bytes(),
                                0o755 if path.stat().st_mode & 0o111 else 0o644)
                               for path in sorted(directory.rglob('*')) if path.is_file())
            source_id = {'repository_commit': commit,
                         'repository_tree': git_value(ROOT, 'rev-parse', 'HEAD^{tree}'),
                         'dtv_userland_core_commit': core_commit,
                         'dtv_userland_core_tree': core_tree,
                         'px4_userland_commit': px4_commit,
                         'px4_userland_tree': px4_tree}
    else:
        members = [(name, path.read_bytes(), 0o755 if path.stat().st_mode & 0o111 else 0o644) for name,path in files]
    metadata = {'project':'pix-userland','version':VERSION,'target':target,'commit':commit,
                'libusb':'1.0.30','private':True,'libusb_linkage':'static','device_assets':False}
    if target == 'source':
        metadata.update(source_id)
        metadata['libusb_sha256'] = LIBUSB_SHA256
    metadata_bytes = json.dumps(metadata, indent=2).encode()+b'\n'
    members.append(('release-info.json', metadata_bytes, 0o644))
    required = {'LICENSE','COPYING.libusb','THIRD_PARTY_NOTICES.md','licenses/musl-COPYRIGHT.txt','docs/releases.md','packaging/pix-usb-permissions'}
    if target == 'source':
        required |= {'CMakeLists.txt','cmake/dtv-userland-core.revision','cmake/px4-userland.revision',
                     'pixd.cpp','tools/pixctl.cpp','userland/tools/pix_ts.cpp','PROVENANCE.md',
                     'third_party/dtv-userland-core/LICENSE','third_party/dtv-userland-core/PROVENANCE.json',
                     'third_party/dtv-userland-core/adapters/host_runtime/COPYING',
                     'third_party/dtv-userland-core/adapters/host_daemon/COPYING',
                     'third_party/dtv-userland-core/adapters/host_facade/COPYING',
                     'third_party/dtv-userland-core/adapters/host_ts/COPYING',
                     'third_party/dtv-userland-core/adapters/windows_winscard/COPYING',
                     'third_party/px4-userland/LICENSE','third_party/px4-userland/PROVENANCE.md',
                     'third_party/px4-userland/THIRD_PARTY_NOTICES.md'}
    assert required <= {name for name, _, _ in members}, 'Missing package documentation or licenses'
    if target == 'source':
        usb = ROOT/'build/deps/libusb-1.0.30.tar.bz2'
        usb_bytes = usb.read_bytes()
        if hashlib.sha256(usb_bytes).hexdigest() != LIBUSB_SHA256: raise ValueError('libusb source checksum mismatch')
        members.append(('third_party/libusb-1.0.30.tar.bz2',usb_bytes,0o644))
        files_inventory = inventory(members)
        manifest = {'schema': 1, 'kind': 'corresponding-source', 'project': 'pix-userland',
                    'version': VERSION, 'private': True, **source_id,
                    'libusb_version': '1.0.30', 'libusb_archive_sha256': LIBUSB_SHA256,
                    'licenses': {name: hashlib.sha256(data).hexdigest() for name, data, _ in members
                                 if name in {'LICENSE','COPYING.libusb',
                                             'third_party/dtv-userland-core/LICENSE',
                                             'third_party/px4-userland/LICENSE'} or name.endswith('/COPYING')},
                    'files': files_inventory}
        manifest_bytes = json.dumps(manifest, sort_keys=True, indent=2).encode()+b'\n'
        members.append(('source-manifest.json', manifest_bytes, 0o644))
        sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data, _ in sorted(members))
        members.append(('SHA256SUMS', sums.encode('ascii'), 0o644))
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
    parser.add_argument('--core-source-root', type=Path)
    parser.add_argument('--px4-source-root', type=Path)
    parser.add_argument('--output-dir', type=Path)
    args=parser.parse_args();package(args.target,args.binary_dir,args.core_source_root,args.px4_source_root,args.output_dir)
