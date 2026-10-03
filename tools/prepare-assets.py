#!/usr/bin/env python3
"""Extract device assets from the user's StationTV LE 2.3.3.1 installation."""
import argparse
import hashlib
import json
import os
import struct
from pathlib import Path

FRAMEWORKS = 'AliasFile CorePixy LoginItemsManager LogUtility PDLayerPixy Pixy PixyGraph px_mpegvd SerialKeyUtility SkinManager Sparkle TSReceiver TVCore WDLibrary'.split()
PLUGINS = 'AACDec AudioOutputter DataSource DefaultPixy OpenGLViewVideoOutputter TSDemuxer TSFileSource TSRecorder VideoDec'.split()
PD_SHA256 = 'c10c313ede85952d261f787af9b1084cd101292e6c1e11dfd46b446ad7d4e190'


def mapped_bytes(binary, address, size):
    if binary[:4] != b'\xce\xfa\xed\xfe':
        raise ValueError('Expected a 32-bit little-endian Mach-O')
    offset = 28
    for _ in range(struct.unpack_from('<I', binary, 16)[0]):
        command, length = struct.unpack_from('<II', binary, offset)
        if command == 1:
            _, vmaddr, _, fileoff, filesize = struct.unpack_from('<16sIIII', binary, offset+8)
            if vmaddr <= address and address+size <= vmaddr+filesize:
                return binary[fileoff+address-vmaddr:fileoff+address-vmaddr+size]
        offset += length
    raise ValueError('Address is outside file-backed segments')


def prepare(app, fpga, output):
    app = Path(app); output = Path(output)
    framework = app/'Contents/Frameworks/PDLayerPixy.framework/Versions/A'
    binary = (framework/'PDLayerPixy').read_bytes()
    if hashlib.sha256(binary).hexdigest() != PD_SHA256:
        raise ValueError('This extractor supports only the supplied StationTV LE 2.3.3.1 binary')
    paths = [app/f'Contents/Frameworks/{n}.framework/Versions/A/{n}' for n in FRAMEWORKS]
    paths += [app/'Contents/Resources/PxMoveFilter.bundle/Contents/MacOS/PxMoveFilter']
    paths += [framework/f'Resources/PixyPlugins/{n}.pixy/Contents/MacOS/{n}' for n in PLUGINS]
    digest = hashlib.md5()
    for path in paths:
        digest.update(path.read_bytes())
    tag = (app/'Contents/Resources/tag.dat').read_bytes()
    if len(tag) != 48: raise ValueError('Unexpected application validation tag size')
    bitstream = Path(fpga).read_bytes()
    if len(bitstream) != 699673: raise ValueError('Unexpected FPGA bitstream size')
    assets = {'bootstrap.bin': mapped_bytes(binary, 0x14a4bd+0x10ce98f, 16),
              'validation.bin': digest.digest()+tag, 'fpga.bin': bitstream}
    output.mkdir(parents=True, exist_ok=True); output.chmod(0o700)
    for name, data in assets.items():
        with open(output/name, 'wb') as f: f.write(data)
        (output/name).chmod(0o600)
    (output/'manifest.json').write_text(json.dumps({'application_sha256':PD_SHA256,
           'validation_files':len(paths),'fpga_sha256':hashlib.sha256(bitstream).hexdigest()},indent=2)+'\n')
    print(f'Prepared {len(assets)} private assets in {output.resolve()}')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app', required=True, type=Path)
    p.add_argument('--fpga', required=True, type=Path)
    p.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent/'assets')
    a = p.parse_args(); prepare(a.app, a.fpga, a.output)
