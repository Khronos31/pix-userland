"""Bounded terrestrial capture; decoded TS on stdout, logs on stderr."""
import argparse
import os
from pathlib import Path
import sys
import time

from card import Card
from dt295 import (USB, USBError, Device, Crypto, Framer, ProtocolError,
                   Terrestrial, authenticate, driver_material, renew_if_due)
from receiver import ReceiveProfile, Receiver


def capture(device, assets, profile, iv, channel, seconds, output, recover=False):
    if not 0 < seconds <= 60 or len(iv) != 16:
        raise ValueError('requires 0 < seconds <= 60 and a 16-byte IV')
    initial_state = device.word(0x82008)
    if initial_state == 0xffff and recover:
        device.recover_session()
        initial_state = 0x10
    if initial_state not in (0, 0x10):
        raise ProtocolError('cold or unauthenticated ready device required')
    material = driver_material(assets/'pxmb86m0x_usb.sys')
    crypto = Crypto()
    receiver = Receiver(device)
    received = packets = 0
    framer = Framer()
    try:
        if initial_state == 0:
            device.boot(assets/'mb86m01_assp_osec_idle.bin')
        authenticate(device, crypto, material)
        device.configure_idle()
        card = Card(device)
        card.initialize()
        card.activate()
        tuner = Terrestrial(device, material)
        tuner.initialize()
        tuner.tune(channel)
        renew_if_due(device, material)
        receiver.prepare(profile, assets/'mb86m01_assp_osec_trc_m_h.bin')
        receiver.start()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            renew_if_due(device, material)
            try:
                data = device.usb.transfer(0x81, 65536, 100)
            except USBError as e:
                if e.code == -7:
                    continue
                raise
            received += len(data)
            decoded = bytearray()
            for packet in framer.feed(data):
                decoded += crypto.packet(receiver.key, iv, packet)
                packets += 1
            output.write(decoded)
            if device.word(0x82008) == 0xffff:
                raise ProtocolError('firmware entered error state')
        output.flush()
        if not packets:
            raise ProtocolError('no TS packets received')
        return {'received_bytes': received, 'packets': packets,
                'trailing_bytes': len(framer.pending)}
    finally:
        # A failed start may nevertheless have reached running state.
        state = device.drain_states()
        if state in (1, 2, 3, 4):
            receiver.stop()
            state = device.drain_states()
            if state == 1:
                device.configure_idle()
                state = device.drain_states()
        print(f'capture cleanup: state=0x{state:04x}', file=sys.stderr)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets', required=True, type=Path)
    p.add_argument('--profile', required=True, type=Path)
    p.add_argument('--iv', required=True, type=Path)
    p.add_argument('--seconds', type=float, default=40)
    p.add_argument('--recover', action='store_true',
                   help='explicitly reset firmware error before authentication')
    p.add_argument('channel')
    a = p.parse_args()
    try:
        profile = ReceiveProfile.load(a.profile)
        iv = a.iv.read_bytes()
        with USB() as usb:
            result = capture(Device(usb), a.assets, profile, iv, a.channel,
                             a.seconds, sys.stdout.buffer, recover=a.recover)
        print('capture complete: '+str(result), file=sys.stderr)
        return 0
    except BrokenPipeError:
        # The consumer closed its input; cleanup already ran in capture().
        with open(os.devnull, 'wb') as sink:
            os.dup2(sink.fileno(), sys.stdout.fileno())
        return 0
    except KeyboardInterrupt:
        return 130
    except Exception as e:
        print('capture failed: '+str(e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
