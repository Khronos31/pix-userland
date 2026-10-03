#!/usr/bin/env python3
"""Check architecture and permitted runtime dependencies of release binaries."""
import re, struct, subprocess, sys
from pathlib import Path
target, filename = sys.argv[1:]
p=Path(filename); data=p.read_bytes()
def run(*args):
    return subprocess.check_output(args,text=True)
if target.startswith(('linux-', 'android-')):
    assert data[:4] == b'\x7fELF' and data[5] == 1, 'Expected little-endian ELF'
    machine=struct.unpack_from('<H',data,18)[0]
    assert machine == {'x86_64':62,'aarch64':183,'armv7a':40}[target.split('-',1)[1]], 'Wrong ELF architecture'
    headers=run('readelf','-lW',str(p)); dynamic=run('readelf','-dW',str(p))
    deps=re.findall(r'Shared library: \[(.*?)\]',dynamic)
    if target.startswith('linux-'):
        assert 'INTERP' not in headers and not deps, 'Linux must be fully static'
    else:
        interp='/system/bin/linker' + ('' if target.endswith('armv7a') else '64')
        assert f'Requesting program interpreter: {interp}]' in headers, 'Wrong Android interpreter'
        assert set(deps) <= {'libc.so','libdl.so','libm.so','liblog.so'}, deps
    print(headers); print(dynamic)
elif target == 'darwin-arm64':
    assert run('lipo','-archs',str(p)).strip() == 'arm64', 'Wrong Mach-O architecture'
    deps=run('otool','-L',str(p))
    for line in deps.splitlines()[1:]:
        assert line.strip().startswith(('/usr/lib/','/System/Library/')), line
    print(deps)
elif target == 'windows-x64':
    allowed={'kernel32.dll','msvcrt.dll','bcrypt.dll','user32.dll','advapi32.dll','ole32.dll','setupapi.dll','cfgmgr32.dll','shlwapi.dll','shell32.dll','winusb.dll','libusb-1.0.dll'}
    for item in (p,p.parent/'libusb-1.0.dll'):
        content=item.read_bytes();pe=struct.unpack_from('<I',content,0x3c)[0]
        assert content[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H',content,pe+4)[0] == 0x8664, 'Wrong PE architecture'
        imports=run('objdump','-p',str(item))
        deps=re.findall(r'DLL Name: (\S+)',imports)
        assert deps and all(d.lower() in allowed or re.fullmatch(r'api-ms-win-crt-[a-z0-9-]+\.dll',d.lower()) for d in deps), deps
        print(item.name, deps)
else:
    raise ValueError(target)
print('Binary audit passed:', target)
