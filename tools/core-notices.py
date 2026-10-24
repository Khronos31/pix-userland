#!/usr/bin/env python3
"""Copy the license and provenance files of a dtv-userland-core source tree into a notices directory."""
import shutil, sys
from pathlib import Path

def copy_notices(core_root, destination):
    core_root, destination = Path(core_root), Path(destination)
    if not (core_root / 'LICENSE').is_file():
        raise SystemExit(f'core-notices: missing LICENSE in {core_root}')
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(core_root / 'LICENSE', destination / 'dtv-userland-core-LICENSE')
    if (core_root / 'PROVENANCE.json').is_file():
        shutil.copyfile(core_root / 'PROVENANCE.json', destination / 'dtv-userland-core-PROVENANCE.json')
    copings = sorted((core_root / 'adapters').glob('*/COPYING'))
    if not copings:
        raise SystemExit(f'core-notices: no adapter COPYING in {core_root}')
    for path in copings:
        shutil.copyfile(path, destination / f'dtv-userland-core-{path.parent.name}-COPYING')

if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: core-notices.py CORE_SOURCE_ROOT DESTINATION')
    copy_notices(*sys.argv[1:])
