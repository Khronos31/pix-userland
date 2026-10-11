"""Target-0 receive setup using a private, offline-resolved configuration.

Profiles contain device/session-specific inputs and stay outside source control.
Bounded Mac video/audio reception is verified; Linux hardware is untested.
"""
import json
import struct
import time
from pathlib import Path
from dt295 import ProtocolError, USBError


class ReceiveProfile:
    def __init__(self, data):
        if data.get('schema') != 1 or data.get('target') != 0:
            raise ValueError('only target-0 profile schema 1 is supported')
        words = data.get('ts_key_request_words')
        if not isinstance(words, list) or len(words) != 3 or any(
                type(w) is not int or not 0 <= w <= 0xffffffff for w in words):
            raise ValueError('TS key request requires three uint32 values')
        # SDK 104c3640 converts host DWORDs to big endian before transport.
        self.key_request = struct.pack('>3I', *words)
        rows = data.get('fields')
        if not isinstance(rows, list) or not rows or len(rows) > 4096:
            raise ValueError('invalid TRC field count')
        fields = []
        for row in rows:
            a, shift, width, value = (row.get(k) for k in ('address','shift','width','value'))
            if any(type(v) is not int for v in (a,shift,width,value)):
                raise ValueError('TRC field must be integral')
            if not 0x1000 <= a <= 0x1afe or a % 2:
                raise ValueError('TRC field outside configuration registers')
            if not 0 <= shift < 16 or not 1 <= width <= 16-shift or not 0 <= value < 1 << width:
                raise ValueError('TRC field bit range')
            fields.append((a,shift,width,value))
        self.fields = tuple(fields)
        # Reconstruct DMA input selection after the configuration block clear.
        reg = 0
        for a, shift, width, value in fields:
            if a == 0x1002:
                mask = ((1 << width) - 1) << shift
                reg = (reg & ~mask) | value << shift
        dma_input = (reg >> 8) & 3
        if dma_input not in (0, 1):
            raise ValueError('unsupported DMA input selection')
        self.pause_state = 3 if dma_input else 4

    @classmethod
    def load(cls, path):
        return cls(json.loads(Path(path).read_text()))


class Receiver:
    def __init__(self, device):
        self.device = device
        self.key = None
        self.prepared = False
        self.started = False
        self.pause_state = None

    def prepare(self, profile, firmware):
        """Get the key before TRC preparation, as observed before KS ACQUIRE.

        Control-session authentication and terrestrial tuning must already have
        completed. Never retry a failed upload or state-changing command here.
        """
        if self.prepared or self.started:
            raise ProtocolError('receiver already prepared')
        if not isinstance(profile, ReceiveProfile):
            raise ValueError('validated ReceiveProfile required')
        d = self.device
        if d.session_key is None or d.word(0x82008) != 0x11:
            raise ProtocolError('authenticated idle state required')
        # Validate firmware size before sending any key/state-changing request.
        size = Path(firmware).stat().st_size
        if not 0 < size <= 16*1024*1024:
            raise ProtocolError('invalid firmware size')
        self.key = None
        key = d.secure_command(0x07010600, profile.key_request)
        if len(key) != 16:
            raise ProtocolError('TS key response must contain exactly four words')
        # Direct USB response is already in wire byte order. Do not apply the
        # SDK's host-DWORD conversion a second time to the AES key.
        d.load_trc(firmware)
        if d.word(0x82008) != 0x11:
            raise ProtocolError('TRC upload did not retain idle state')
        for addr, n in ((0x1000,0x500),(0x1500,0x500),(0x1a00,0x100)):
            d.write(addr, bytes(n))
        for field in profile.fields:
            d.field(*field)
        d.put(0x82028, 0)
        d.command(4, 0x20)
        d.wait_state(1)
        self.key, self.prepared = key, True
        self.pause_state = profile.pause_state

    def start(self):
        if not self.prepared or self.started or self.key is None:
            raise ProtocolError('TS start requires successful keyed preparation')
        if self.device.word(0x82008) != 1:
            raise ProtocolError('TS start requires state 1')
        # Invalidate preparation before submitting: a timeout must not permit
        # another start on this object; recover with a new idle session instead.
        self.prepared = False
        self.device.command(5, 0, 2)
        self.device.wait_state(2)
        self.started = True

    def _drain_to_pause(self, timeout=5000):
        # The observed pause needed pending target-0 output to be drained.
        # Keep consuming EP81 after command 5, without resending the command
        # or polling inactive output endpoints.
        deadline = time.monotonic() + timeout / 1000
        total = 0
        while True:
            state = self.device.drain_states()
            if state == self.pause_state:
                return total
            if state != 2 or time.monotonic() >= deadline:
                raise ProtocolError(f'pause incomplete: state={state:04x}')
            try:
                total += len(self.device.usb.transfer(0x81, 65536, 10))
            except USBError as e:
                if e.code != -7:
                    raise

    def stop(self):
        """Use observed hardware state, even when start timed out."""
        drained = 0
        try:
            state = self.device.drain_states()
            if state == 2:
                # Driver 31fd0/32bec chooses 3 or 4 from DMA input selection.
                if self.pause_state is None:
                    raise ProtocolError('missing receive profile for running device')
                self.device.command(5, 0, self.pause_state)
                drained = self._drain_to_pause()
                state = self.pause_state
            if state in (3,4):
                self.device.command(5, 0, 1)
                self.device.wait_state(1)
                self.device.configure_idle()
            elif state not in (1, 0x11):
                raise ProtocolError(f'cannot stop device state={state:04x}')
            return drained
        finally:
            self.key = None
            self.started = self.prepared = False
