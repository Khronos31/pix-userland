"""Experimental, synchronous PIX-DT295 (06b8:103b) protocol primitives.

Only target 0 is implemented. Register byte strings are WIRE ORDER; callers
must explicitly convert words. No keys or broadcast data are logged.
"""
import ctypes as C
import ctypes.util
import json
import os
from pathlib import Path
import struct
import sys
import time


class ProtocolError(RuntimeError):
    pass


class USBError(ProtocolError):
    def __init__(self, code, operation):
        self.code = code
        super().__init__(f'{operation}: libusb error {code}')


class USB:
    def __init__(self):
        name = ctypes.util.find_library('usb-1.0')
        if not name and sys.platform == 'darwin':
            name = next((str(p) for p in (Path('/opt/homebrew/lib/libusb-1.0.dylib'),
                         Path('/usr/local/lib/libusb-1.0.dylib')) if p.exists()), None)
        if not name:
            raise ProtocolError('libusb-1.0 not found')
        self.lib = C.CDLL(name)
        p, i, u, b, s = C.c_void_p, C.c_int, C.c_uint, C.c_ubyte, C.c_ushort
        signatures = {
            'libusb_init': ([C.POINTER(p)], i),
            'libusb_exit': ([p], None),
            'libusb_open_device_with_vid_pid': ([p, s, s], p),
            'libusb_close': ([p], None),
            'libusb_claim_interface': ([p, i], i),
            'libusb_release_interface': ([p, i], i),
            'libusb_control_transfer': ([p, b, b, s, s, p, s, u], i),
            'libusb_bulk_transfer': ([p, b, p, i, C.POINTER(i), u], i),
            'libusb_interrupt_transfer': ([p, b, p, i, C.POINTER(i), u], i),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.lib, name); fn.argtypes = args; fn.restype = result
        self.context = p(); self.handle = None; self.claimed = False
        self.check(self.lib.libusb_init(C.byref(self.context)), 'init')
        self.handle = self.lib.libusb_open_device_with_vid_pid(self.context, 0x06b8, 0x103b)
        if not self.handle:
            self.close()
            raise ProtocolError('PIX-DT295 06b8:103b cannot be opened')

    @staticmethod
    def check(code, op):
        if code < 0:
            raise USBError(code, op)
        return code

    def claim(self):
        if not self.claimed:
            self.check(self.lib.libusb_claim_interface(self.handle, 0), 'claim interface 0')
            self.claimed = True

    def close(self):
        if self.handle:
            if self.claimed:
                self.lib.libusb_release_interface(self.handle, 0)
            self.lib.libusb_close(self.handle); self.handle = None
        if self.context:
            self.lib.libusb_exit(self.context); self.context = None

    def control(self, request_type, request, value, index, data, timeout=1000):
        read = bool(request_type & 0x80)
        n = data if read else len(data)
        buf = (C.c_ubyte * n)()
        if not read: buf[:] = data
        got = self.check(self.lib.libusb_control_transfer(self.handle, request_type,
                         request, value, index, buf, n, timeout), 'control transfer')
        if got != n: raise ProtocolError(f'short control transfer: {got}/{n}')
        return bytes(buf) if read else None

    def transfer(self, ep, data, timeout=1000, interrupt=False):
        self.claim()
        read = bool(ep & 0x80)
        n = data if read else len(data)
        buf = (C.c_ubyte * n)()
        if not read: buf[:] = data
        got = C.c_int()
        fn = self.lib.libusb_interrupt_transfer if interrupt else self.lib.libusb_bulk_transfer
        code = fn(self.handle, ep, buf, n, C.byref(got), timeout)
        # A timed-out bulk IN can still contain valid bytes. OUT must never retry
        # blindly because a partial command/firmware upload changes device state.
        if code == -7 and read and got.value:
            return bytes(buf[:got.value])
        self.check(code, f'endpoint {ep:02x}')
        if not read and got.value != n:
            raise ProtocolError(f'short OUT transfer: {got.value}/{n}')
        return bytes(buf[:got.value]) if read else None

    def __enter__(self): return self
    def __exit__(self, *_): self.close()


class Device:
    def __init__(self, usb):
        self.usb = usb
        self.session_key = None
        self.session_iv = None
        self.crypto = None
        self.renewal_counter = None
        self.renewal_due = None

    def read(self, addr, n):
        if not 0 <= addr <= 0xffffff or not 0 <= n <= 65536 or addr + n > 0x1000000:
            raise ValueError('register range')
        out = bytearray()
        while n:
            count = min(n, 64)
            out += self.usb.control(0xc0, 0xbc, (addr >> 16) << 8, addr & 65535, count)
            addr += count; n -= count
        return bytes(out)

    def write(self, addr, data):
        if not 0 <= addr <= 0xffffff or len(data) > 65536 or addr + len(data) > 0x1000000:
            raise ValueError('register range')
        for off in range(0, len(data), 64):
            a = addr + off
            self.usb.control(0x40, 0xbc, (a >> 16) << 8, a & 65535, data[off:off+64])

    def word(self, addr): return int.from_bytes(self.read(addr, 2), 'big')
    def put(self, addr, value): self.write(addr, value.to_bytes(2, 'big'))
    def field(self, addr, shift, width, value):
        if not 0 <= shift < 16 or not 1 <= width <= 16-shift or not 0 <= value < 1 << width:
            raise ValueError('register field')
        mask = ((1 << width) - 1) << shift
        self.put(addr, (self.word(addr) & ~mask) | value << shift)

    def status(self):
        return {f'{a:05x}': self.word(a) for a in (0x82008, 0x90074, 0x90078, 0x901b0, 0x901b4)}

    def command(self, cmd, sub=0, value=0, i2c=False):
        mask = 2 if i2c else 8
        if self.word(0x90074) & mask:
            raise ProtocolError('stale command completion; drain before issuing another command')
        self.usb.control(0x40, 0xb9 if i2c else 0xb8, 0, 0, bytes((cmd, sub)) + struct.pack('>H', value))
        self.wait_event(mask, 5000)
        completion = self.word(0x80016 if i2c else 0x80014)
        # Copy command 8 response before acknowledging completion (driver 2ad28).
        response = self.read(0x83000, 512) if cmd == 8 else None
        self.put(0x90074, mask)
        if completion >> 8 != cmd or completion & 255 in (0x80, 0x81):
            raise ProtocolError(f'command {cmd:02x} completion {completion:04x}')
        return response

    def secure_command(self, command_id, payload=b''):
        if len(payload) % 4 or len(payload) > 496:
            raise ValueError('secure command payload length')
        request = struct.pack('>4I', command_id, 0, len(payload)//4, 0xffffffff)
        request = (request + payload).ljust(512, b'\0')
        # Driver 290a0/2b168: bits 10/9 select request/response CBC.
        # The allocation has a four-byte prefix; wire encryption starts at 12.
        if command_id & 0x600 and self.session_key is None:
            raise ProtocolError('authenticated control session required')
        if command_id & 0x400:
            request = request[:12] + self.crypto.cbc(self.session_key,self.session_iv,request[12:508],True) + request[508:]
        self.write(0x83000, request)
        response = self.command(8)
        if command_id & 0x200:
            response = response[:12] + self.crypto.cbc(self.session_key,self.session_iv,response[12:508]) + response[508:]
        rid, status, words = struct.unpack('>3I', response[:12])
        if rid != command_id or status or words > 124:
            raise ProtocolError(f'secure command {command_id:08x}: id={rid:08x}, status={status:08x}, words={words}')
        # Driver 41b94 checks status at +4 and copies payload from +16.
        # The encrypted prefix at +12 is not a result/status DWORD.
        return response[16:16+4*words]

    def i2c(self, address, data=b'', read_length=0):
        if not 0 <= address <= 255 or len(data) > 64 or not 0 <= read_length <= 64:
            raise ValueError('I2C address/length')
        if not data and not read_length: raise ValueError('empty I2C transaction')
        if self.word(0x90074) & 0x100: raise ProtocolError('stale I2C reply')
        if data:
            self.write(0x800c0, data.ljust(64,b'\0'))
            self.command(9, 2 if read_length else 0, address << 8 | len(data), i2c=True)
        if read_length:
            self.command(9, 3 if data else 1, address << 8 | read_length, i2c=True)
        self.wait_event(0x100, 1000)
        reply = self.read(0x823be, 66)
        self.put(0x90074, 0x100)
        if reply[:2] != b'\0\0':
            raise ProtocolError(f'I2C {address:02x} status {reply[0]:02x}/{reply[1]:02x}')
        return reply[2:2+read_length]

    def drain_states(self):
        """Consume state notifications; 82008 can show an older queued state."""
        for _ in range(64):
            pending = self.word(0x90074)
            state = self.word(0x82008)
            if pending & 4:
                self.put(0x90074, 4)
            if state == 0xffff:
                raise ProtocolError('firmware entered error state')
            if not pending & 4:
                return state
        raise ProtocolError('state notification queue did not settle')

    def wait_state(self, expected, timeout=30000):
        # Driver 29378 waits for a separate state notification after command
        # completion; command 5's target state is its value, not the KS state.
        deadline = time.monotonic() + timeout / 1000
        while True:
            state = self.drain_states()
            if state == expected:
                return state
            if time.monotonic() >= deadline:
                raise ProtocolError(f'state {expected:04x} timed out; last={state:04x}')
            time.sleep(.01)

    def configure_idle(self):
        # 2efe0 defaults, USB INF BCASNUM=1, script table cb8.
        self.field(0x80074,8,2,1); self.field(0x80074,0,7,0)
        for shift in (12,8,4): self.field(0x80076,shift,4,1)
        self.field(0x80078,8,8,0); self.field(0x80078,0,8,0)
        self.command(1)
        self.wait_state(0x11, 5000)

    def recover_session(self):
        """Explicitly recover firmware error; never reset a healthy receiver.

        Driver 1d38c/action16 uses command 3 to return to unauthenticated
        state 0x10. A stale error notification may remain briefly after the
        completion; only this bounded reset wait tolerates that old error.
        This command is rejected in normal idle, so it is not a shutdown API.
        """
        state = self.word(0x82008)
        if state != 0xffff:
            raise ProtocolError('session recovery requires firmware error state')
        self.session_key = self.session_iv = self.crypto = None
        self.renewal_counter = self.renewal_due = None
        if self.word(0x90074) & 4:
            self.put(0x90074, 4)
        self.command(3)
        deadline = time.monotonic() + 5
        while True:
            pending, state = self.word(0x90074), self.word(0x82008)
            if pending & 4:
                self.put(0x90074, 4)
            if state == 0x10:
                return
            if state not in (0x11, 0xffff) or time.monotonic() >= deadline:
                raise ProtocolError(f'session reset incomplete: state={state:04x}')
            time.sleep(.01)

    def load_trc(self, firmware):
        # Driver action 6: table e68, 2e4d0 -> 1d318 -> 184d8.
        # Unlike cold boot, command 4 prepares this upload; no 90070 kick follows.
        if self.word(0x82008) != 0x11:
            raise ProtocolError('TRC upload requires idle state 0x11')
        data = Path(firmware).read_bytes()
        if not data or len(data) > 16*1024*1024:
            raise ProtocolError('invalid firmware size')
        self.command(4)
        for off in range(0, len(data), 65536):
            self.usb.transfer(0x02, data[off:off+65536], 300)
        return {'firmware_bytes': len(data), 'state': self.word(0x82008)}

    def reset_tuner(self):
        # Model 7 GPIO masks in 1b06c, reset pulse in 50e20, access in 1da74.
        self.field(0x90014,1,1,1)
        self.field(0x90018,1,1,0); time.sleep(.020)
        self.field(0x90018,1,1,1); time.sleep(.005)

    def notification(self, timeout=1000):
        data = self.usb.transfer(0x83, 6, timeout, interrupt=True)
        # Driver 1944c requests six bytes but accepts any nonempty completion.
        # The authoritative event/status is then read from BC registers.
        if not data: raise ProtocolError('empty notification')
        return data

    def wait_event(self, mask, timeout=1000):
        deadline = time.monotonic() + timeout / 1000
        while True:
            pending = self.word(0x90074)
            if pending & mask: return pending
            remaining = deadline - time.monotonic()
            if remaining <= 0: raise ProtocolError(f'event {mask:04x} timed out')
            try: self.notification(max(1, min(50, int(remaining * 1000))))
            except USBError as e:
                if e.code != -7: raise

    def boot(self, firmware):
        # Driver VA 17578,19674,198ec,19a34. Require cold state; do not replace
        # active firmware or reset another receiver behind its back.
        if self.word(0x82008) != 0:
            raise ProtocolError('boot requires cold state 0; active firmware left untouched')
        data = Path(firmware).read_bytes()
        if not data or len(data) > 16*1024*1024:
            raise ProtocolError('invalid firmware size')
        self.usb.claim()
        self.field(0x90194, 12, 3, 1); self.put(0x90198, 0x40)
        for bit in range(13, -1, -1): self.field(0x90078, bit, 1, 0)
        for bit in range(11, -1, -1): self.field(0x901b4, bit, 1, 0)
        self.wait_event(4, 100)
        if not self.word(0x90074) & 4: raise ProtocolError('firmware upload not ready')
        self.field(0x90074, 2, 1, 1)
        for off in range(0, len(data), 65536):
            self.usb.transfer(0x02, data[off:off+65536], 300)
        for bit in range(13, -1, -1): self.field(0x90078, bit, 1, int(bit != 4))
        for bit in range(11, -1, -1): self.field(0x901b4, bit, 1, 1)
        try:
            self.field(0x90070, 2, 1, 1)
        except USBError as e:
            # The firmware can restart EP0 before acknowledging this OUT.
            # Never resend it: validate the independent boot state/event below.
            if e.code != -7: raise
        # EP0 may also time out on the first IN during firmware restart.
        # Retry only observations; never repeat the firmware upload/start OUT.
        deadline = time.monotonic() + 3
        while True:
            try:
                pending, state = self.word(0x90074), self.word(0x82008)
                if pending & 4 and state in (0x10, 0x11): break
            except USBError as e:
                if e.code != -7: raise
            if time.monotonic() >= deadline:
                raise ProtocolError('firmware boot status unavailable after restart')
            time.sleep(.02)
        self.put(0x90074, 4)
        if not pending & 4 or state not in (0x10, 0x11):
            raise ProtocolError(f'firmware boot failed: state={state:04x}, pending={pending:04x}')
        return {'firmware_bytes': len(data), 'boot_state': state}


class Crypto:
    def __init__(self):
        suffix = '.dylib' if sys.platform == 'darwin' else '.so'
        self.lib = C.CDLL(str(Path(__file__).resolve().parent / ('libdt295' + suffix)))
        p = C.c_void_p
        for name, args in [('dt295_packet', [p,p,p]), ('dt295_payload', [p,p,p,C.c_size_t]),
                           ('dt295_cbc_encrypt', [p,p,p,C.c_size_t]),
                           ('dt295_cmac', [p,p,C.c_size_t,p])]:
            getattr(self.lib, name).argtypes = args
        self.lib.dt295_packet.restype = C.c_int
        self.lib.dt295_cmac.restype = None

    def cbc(self, key, iv, data, encrypt=False):
        if len(key)!=16 or len(iv)!=16 or len(data)%16: raise ValueError('CBC sizes')
        out=C.create_string_buffer(data,len(data))
        fn=self.lib.dt295_cbc_encrypt if encrypt else self.lib.dt295_payload
        if fn(key,iv,out,len(data)): raise ProtocolError('CBC failed')
        return out.raw

    def cmac(self, key, data):
        if len(key) != 16: raise ValueError('AES key size')
        out = C.create_string_buffer(16)
        self.lib.dt295_cmac(key, data, len(data), out)
        return out.raw

    def packet(self, key, iv, packet):
        if len(key) != 16 or len(iv) != 16 or len(packet) != 188:
            raise ValueError('key, IV or packet size')
        out = C.create_string_buffer(packet, 188)
        code = self.lib.dt295_packet(key, iv, out)
        if code < 0: raise ProtocolError(f'TS rejected (code {code})')
        return out.raw


class Terrestrial:
    def __init__(self, device, material): self.d, self.material = device, material
    def demod_write(self, reg, value): self.d.i2c(0x20, bytes((reg,value)))
    def demod_read(self, reg): return self.d.i2c(0x20, bytes((reg,)),1)[0]
    def rf_write(self, reg, value):
        self.d.i2c(0x20, bytes((0xfe,0xc0,reg)) + (b'' if reg == 255 else bytes((value,))))
    def rf_read(self, reg):
        self.d.i2c(0x20,bytes((0xfe,0xc0,0xfb,reg)))
        return self.d.i2c(0x20,b'\xfe\xc1',1)[0]
    def rf_field(self, reg, mask, value): self.rf_write(reg,self.rf_read(reg)&mask|value)
    def table(self, va, count, write):
        pairs=list(struct.iter_unpack('<II',self.material(va,count*8)))
        if any(reg>255 or value>255 for reg,value in pairs): raise ProtocolError('invalid register table')
        for reg,value in pairs: write(reg,value)
    def initialize(self):
        self.d.reset_tuner()
        self.table(0x841d0,22,self.demod_write)
        self.rf_write(255,0); self.table(0x84630,26,self.rf_write)
        self.rf_write(0,1); self.rf_field(0x31,0x2f,0xd0); self.rf_write(0,0)
        # Constructor 59fc0: XTAL_CAP_SYSTEM1 default 9, clock mode 1.
        self.rf_write(1,0x89); self.rf_write(2,0); self.rf_write(0x6d,10)
        self.rf_write(0x0b,1); self.rf_field(4,0xc0,1); self.rf_write(5,0x1b)
        self.rf_field(8,0xf2,5); self.rf_field(9,0x80,0x42)
        self.rf_field(0x5e,0xef,0); self.rf_write(10,0)
        self.table(0x84700,28,self.rf_write)
        self.rf_write(0x6d,10); self.rf_write(3,0); self.rf_write(3,1); time.sleep(.1)
        self.frequency(473142857)
        self.rf_write(0xb,1); self.rf_write(0x12,1); time.sleep(.02)
        self.rf_write(0x12,0); self.rf_write(0xb,0)
        self.demod_write(0x25,0); self.demod_write(0x23,0x4d); self.demod_write(3,0x90)

    def frequency(self, hz):
        if not 90000000 <= hz <= 770000000: raise ValueError('RF frequency out of range')
        n=hz*64//1000000
        self.rf_field(0xd,0xfe,0); self.rf_write(0xf,0x20)
        for reg,value in [(0x10,n&255),(0x11,n>>8),(0,1),
                          (0x31,0x81 if hz<700000000 else 0x91),(0,0),
                          (0x7c,0x1f if hz<700000000 else 0x9f)]: self.rf_write(reg,value)

    def tune(self, channel):
        if not channel.startswith('T') or not channel[1:].isdigit() or not 13<=int(channel[1:])<=62:
            raise ValueError('terrestrial channel must be T13..T62')
        hz=473142857+(int(channel[1:])-13)*6000000
        self.rf_write(0xb,1); self.rf_write(0x12,1); time.sleep(.02)
        self.demod_write(3,0)
        for reg in (0x1d,0x1e,0x1f): self.demod_write(reg,0xaa)
        self.demod_write(0x25,0); self.demod_write(0x23,0x4d)
        self.frequency(hz)
        self.rf_write(0x12,0); self.rf_field(0x6e,0xff,0x80)
        self.rf_field(0x14,0xdf,0); self.rf_write(0x12,1); time.sleep(.02)
        self.rf_field(0x6e,0x7f,0)
        self.demod_write(0x0e,(self.demod_read(0x0e)&0x70)|1)
        self.demod_write(0x0f,0x10)
        self.demod_write(1,0x40); self.demod_write(0x23,0x4c); time.sleep(.1)
        for reg,value in ((0x1d,2),(0x1e,0x2a),(0x1f,0x8a)): self.demod_write(reg,value)
        return hz


def driver_material(path):
    """Extract fixed material from the user's exact driver; never embed in source."""
    import hashlib
    raw = Path(path).read_bytes()
    if hashlib.sha256(raw).hexdigest() != '46889d2f8b046485a0cc25451c1691ca87b13601d0566c807ee6e1c9084fc878':
        raise ProtocolError('unsupported driver SHA-256')
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    sections, optional = struct.unpack_from('<H', raw, pe+6)[0], struct.unpack_from('<H', raw, pe+20)[0]
    base = struct.unpack_from('<Q', raw, pe+24+24)[0]
    def read(va, n):
        for i in range(sections):
            off = pe + 24 + optional + 40*i
            _, rva, size, pos = struct.unpack_from('<4I', raw, off+8)
            delta = va-base-rva
            if 0 <= delta and delta+n <= size:
                return raw[pos+delta:pos+delta+n]
        raise ProtocolError('material outside driver section')
    return read


def authenticate(device, crypto, material):
    # Driver 1187c supplies table 85230; 2ddc0/1b814 copy it to auth+330.
    initial = struct.pack('<8I', 0x7e0, 6, 14, 0x578, 1, 3, 2, 12)
    response = device.secure_command(0x01010000, initial)
    if len(response) < 36: raise ProtocolError('short authentication challenge')
    def key(va, mask): return bytes(a ^ b for a,b in zip(material(va,16), material(mask,16)))
    selector = crypto.cmac(key(0x85198,0x851d8), material(0x85148,30) + response[32:33] + initial[6:7])[-1] & 15
    row = material(0x85230+32*selector, 32)
    reply = crypto.cmac(key(0x851a8,0x851e8), response[:32] + row) + os.urandom(4)
    device.secure_command(0x01020000, reply)
    session = crypto.cmac(key(0x851b8,0x851f8), reply + row + material(0x85168,12))
    session = session[12:16] + session[8:12] + session[0:8]
    device.session_key,device.session_iv,device.crypto=session,material(0x85218,16),crypto
    device.renewal_counter = 1
    device.renewal_due = time.monotonic() + 30
    return session


def renew_if_due(device, material):
    """Call from the receive/control loop; never request premature renewal.

    Match the official 30-second timer. A successful initial reply is not a
    substitute for maintaining this session throughout capture.
    """
    if device.renewal_counter is None or device.renewal_due is None:
        raise ProtocolError("no managed authentication session")
    if time.monotonic() < device.renewal_due: return False
    device.renewal_counter = keepalive(device, material, device.renewal_counter)
    device.renewal_due = time.monotonic() + 30
    return True


def keepalive(device, material, counter):
    if not 1 <= counter <= 0xffffffff: raise ValueError('authentication counter')
    challenge=device.secure_command(0x02010200)
    if len(challenge)<32: raise ProtocolError('short renewal challenge')
    key=bytes(a^b for a,b in zip(material(0x851c8,16),material(0x85208,16)))
    reply=device.crypto.cmac(key,challenge[:32]+material(0x85178,28)+counter.to_bytes(4,'big'))+os.urandom(4)
    device.secure_command(0x02020400,reply)
    return 1 if counter==0xffffffff else counter+1

class Framer:
    """Strict aligned stream, bounded remainder. Fail instead of hiding TS loss."""
    def __init__(self): self.pending = bytearray(); self.packets = 0
    def feed(self, chunk):
        data = self.pending + chunk
        end = len(data) // 188 * 188
        for off in range(0, end, 188):
            if data[off] != 0x47: raise ProtocolError(f'TS sync lost at packet {self.packets}')
            self.packets += 1
            yield bytes(data[off:off+188])
        self.pending = data[end:]
    def finish(self):
        if self.pending: raise ProtocolError(f'truncated TS: {len(self.pending)} trailing bytes')


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest='action', required=True)
    sub.add_parser('status')
    p = sub.add_parser('boot', help='load idle firmware only; not a complete tuner initialization')
    p.add_argument('firmware')
    p = sub.add_parser('decode', help='offline TS with explicitly supplied session key/IV files')
    p.add_argument('key_file'); p.add_argument('iv_file'); p.add_argument('input')
    a = ap.parse_args()
    try:
        if a.action == 'decode':
            key, iv = Path(a.key_file).read_bytes(), Path(a.iv_file).read_bytes()
            if len(key) != 16 or len(iv) != 16: raise ProtocolError('key/IV must be exactly 16 bytes')
            if sys.stdout.isatty(): raise ProtocolError('redirect binary TS stdout to a file or pipe')
            crypto, framer = Crypto(), Framer()
            with open(a.input, 'rb') as src:
                while chunk := src.read(65536):
                    for packet in framer.feed(chunk):
                        sys.stdout.buffer.write(crypto.packet(key, iv, packet))
            framer.finish(); sys.stdout.buffer.flush()
            print(json.dumps({'packets': framer.packets}), file=sys.stderr)
        else:
            with USB() as usb:
                d = Device(usb)
                result = d.status() if a.action == 'status' else d.boot(a.firmware)
                print(json.dumps(result))
    except (ProtocolError, OSError, ValueError) as e:
        print(f'dt295: {e}', file=sys.stderr); return 1
    return 0


if __name__ == '__main__': sys.exit(main())
