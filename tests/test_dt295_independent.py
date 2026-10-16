#!/usr/bin/env python3
"""Independent macOS CommonCrypto oracle; synthetic data only, no USB/assets."""
import ctypes as C
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if sys.platform != 'darwin':
        print('DT295 independent CommonCrypto oracle: skipped (macOS only)')
        return
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='pix-dt295-crypto-') as tmp:
        libpath = Path(tmp) / 'crypto.dylib'
        subprocess.run(['cc', '-dynamiclib', '-fPIC', '-std=c11', '-Iinclude', '-Isrc',
                        'src/dt295_crypto.c', 'src/crypto.c', 'src/platform.c',
                        '-pthread', '-o', str(libpath)], cwd=root, check=True)
        lib = C.CDLL(str(libpath))
        cc = C.CDLL('/usr/lib/system/libcommonCrypto.dylib')
        cc.CCCrypt.argtypes = [C.c_uint32, C.c_uint32, C.c_uint32, C.c_void_p,
                              C.c_size_t, C.c_void_p, C.c_void_p, C.c_size_t,
                              C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
        cc.CCCrypt.restype = C.c_int
        for name, args in [('dt295_packet_crypto', [C.c_void_p]*3),
                           ('dt295_cmac', [C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]),
                           ('dt295_sha256', [C.c_void_p, C.c_size_t, C.c_void_p])]:
            getattr(lib, name).argtypes = args
        lib.dt295_packet_crypto.restype = C.c_int
        key = bytes(range(16))
        iv = bytes(range(16, 32))

        def aes(block):
            out = C.create_string_buffer(32)
            used = C.c_size_t()
            assert cc.CCCrypt(0, 0, 2, key, 16, None, block, 16, out, 32, C.byref(used)) == 0
            assert used.value == 16
            return out.raw[:16]

        def xor(a, b):
            return bytes(x ^ y for x, y in zip(a, b))

        def protect(plain):
            chain = iv
            out = bytearray()
            full = len(plain)//16*16
            for at in range(0, full, 16):
                chain = aes(xor(plain[at:at+16], chain))
                out.extend(chain)
            out.extend(xor(plain[full:], aes(chain)))
            return bytes(out)

        packets = 0
        for length in range(1, 185):
            packet = bytearray(188)
            packet[:4] = b'\x47\x01\x00\x10'
            start = 4
            if length != 184:
                packet[3] = 0x30
                start = 188-length
                packet[4] = start-5
                for i in range(5, start):
                    packet[i] = (i*13)&255
            plain = bytes((i*19+length)&255 for i in range(length))
            packet[start:] = plain
            expected = bytes(packet)
            packet[3] |= 0x40
            packet[start:] = protect(plain)
            buf = C.create_string_buffer(bytes(packet), 188)
            assert lib.dt295_packet_crypto(key, iv, buf) == 1
            assert buf.raw == expected, ('payload mismatch', length)
            packets += 1
        for bad in (b'\x00\x00\x00\x10', b'\x47\x80\x00\x10', b'\x47\x00\x00\x90'):
            buf = C.create_string_buffer(bad+bytes(184), 188)
            before = buf.raw
            assert lib.dt295_packet_crypto(key, iv, buf) < 0
            assert buf.raw == before

        def double(block):
            number = int.from_bytes(block, 'big')
            return (((number << 1) & ((1 << 128)-1)) ^ (0x87 if number >> 127 else 0)).to_bytes(16, 'big')

        k1 = double(aes(bytes(16)))
        k2 = double(k1)
        for length in range(81):
            data = bytes((i*7+length)&255 for i in range(length))
            full = length != 0 and length % 16 == 0
            prefix = data[:-16] if full else data[:length//16*16]
            last = xor(data[-16:], k1) if full else xor(data[len(prefix):]+b'\x80'+bytes(15-length%16), k2)
            state = bytes(16)
            for at in range(0, len(prefix), 16):
                state = aes(xor(state, prefix[at:at+16]))
            expected = aes(xor(state, last))
            out = C.create_string_buffer(16)
            lib.dt295_cmac(key, data, length, out)
            assert out.raw == expected, ('CMAC mismatch', length)
        for length in (0, 1, 55, 56, 63, 64, 65, 127, 128, 4097):
            data = bytes((i*23)&255 for i in range(length))
            out = C.create_string_buffer(32)
            lib.dt295_sha256(data, length, out)
            assert out.raw == hashlib.sha256(data).digest()
        print(f'DT295 independent oracle passed: {packets} packet payload lengths, 81 CMAC inputs, 10 SHA inputs')


if __name__ == '__main__':
    main()
