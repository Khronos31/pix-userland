"""Experimental card interface initialization/activation; no host APDU handling."""
import time

from dt295 import ProtocolError


class Card:
    def __init__(self, device):
        self.device = device
        self.notified_state = None

    def initialize(self):
        d = self.device
        if d.session_key is None or d.drain_states() != 0x11:
            raise ProtocolError('card initialization requires authenticated idle state')
        # 1cb80 action26 -> 408f0 -> 40d58 table320 -> 2e3bc.
        # Model7 GPIO interface slot e8 (50e90) supplies value 1.
        d.command(0x0a, 0, 1, i2c=True)
        return self.state()

    def state(self):
        # Driver 2281c reads the complete 274-byte notification before ACK.
        # 4e258 receives its high nibble. Never ACK a pending APDU response.
        d = self.device
        for _ in range(32):
            pending = d.word(0x90074)
            if pending & 0x400:
                raise ProtocolError('unconsumed card response')
            data = d.read(0x822ac, 0x112 if pending & 0x200 else 2)
            if len(data) != (0x112 if pending & 0x200 else 2):
                raise ProtocolError('short card status')
            state = int.from_bytes(data[:2], 'big') >> 12
            if pending & 0x200:
                self.notified_state = state
                d.put(0x90074, 0x200)
            else:
                return state
        raise ProtocolError('card notification queue did not settle')

    def activate(self, timeout_ms=2000):
        if not 1 <= timeout_ms <= 5000:
            raise ValueError('card activation timeout')
        d = self.device
        if d.session_key is None or d.drain_states() != 0x11:
            raise ProtocolError('card activation requires authenticated idle state')
        state = self.state()
        if state == 3:
            return state
        # The vendor host starts with unknown state 0xff (4d914), not the
        # register's initial zero. Only a notification establishes card absence.
        unknown_zero = state == 0 and self.notified_state is None
        if state != 1 and not unknown_zero:
            raise ProtocolError(f'card not ready for activation: {state}')
        # 4db5c -> 4e2d0 -> 28e40: command 0a, subcommand 01, value 1.
        # Do not reset/retry on an error or a timeout.
        # 4da68 selects context +2030, constructed as lane 1 by 1b814;
        # 19cf8 maps it to USB B9, completion 80016 / event bit 2.
        d.command(0x0a, 1, 1, i2c=True)
        deadline = time.monotonic() + timeout_ms / 1000
        while True:
            if d.drain_states() != 0x11:
                raise ProtocolError('device left idle during card activation')
            state = self.state()
            if state == 3:
                return state
            if state not in (1, 2) and not (state == 0 and self.notified_state is None):
                raise ProtocolError(f'card activation failed: {state}')
            if time.monotonic() >= deadline:
                raise ProtocolError('card activation timed out')
            time.sleep(.01)
