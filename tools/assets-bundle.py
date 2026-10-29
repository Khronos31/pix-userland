#!/usr/bin/env python3
"""Seal/open the authorized private assets; AES-256-GCM key is supplied via environment."""
import argparse, base64, io, os, tarfile
from pathlib import Path
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from firmware_bundle import read_manifest, verify_bundle
FIRMWARE = 'dt181.bin'
AAD = b'pix-userland-private-assets-v1'
MAGIC = b'PIXASST1'
def key():
    value=base64.b64decode(os.environ['PIX_ASSETS_KEY'],validate=True)
    if len(value)!=32: raise ValueError('Expected 256-bit asset key')
    return value
def seal(source,destination):
    buf=io.BytesIO()
    with tarfile.open(fileobj=buf,mode='w:gz') as t:
        data=Path(source).read_bytes()
        verify_bundle('dt181',data,read_manifest())
        info=tarfile.TarInfo(FIRMWARE);info.size=len(data);info.mode=0o600;info.mtime=0
        t.addfile(info,io.BytesIO(data))
    nonce=os.urandom(12);output=Path(destination);output.parent.mkdir(parents=True,exist_ok=True)
    output.write_bytes(MAGIC+nonce+AESGCM(key()).encrypt(nonce,buf.getvalue(),AAD))
def unseal(source,destination):
    data=Path(source).read_bytes()
    if data[:8]!=MAGIC: raise ValueError('Unknown asset bundle format')
    plain=AESGCM(key()).decrypt(data[8:20],data[20:],AAD)
    output=Path(destination);output.mkdir(mode=0o700,parents=True,exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(plain),mode='r:gz') as t:
        members=t.getmembers()
        if len(members)!=1 or {m.name for m in members}!={FIRMWARE}: raise ValueError('Unexpected firmware set')
        for m in members:
            if not m.isfile(): raise ValueError('Unexpected firmware entry')
            firmware=t.extractfile(m).read()
            verify_bundle('dt181',firmware,read_manifest())
            with os.fdopen(os.open(output/m.name,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600),'wb') as f:
                f.write(firmware)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=['seal','open']);p.add_argument('source');p.add_argument('destination');a=p.parse_args()
    (seal if a.mode=='seal' else unseal)(a.source,a.destination)
    print('Private asset bundle processed (contents not logged)')
