import ctypes as C
import unittest
import tempfile
from pathlib import Path
from unittest.mock import Mock, patch
from dt295 import Crypto, Device, Framer, ProtocolError, USBError, renew_if_due

H = bytes.fromhex
KEY = H('2b7e151628aed2a6abf7158809cf4f3c')
IV = H('000102030405060708090a0b0c0d0e0f')
PLAIN = H('6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51'
          '30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710')
CIPHER = H('7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2'
           '73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7')


class CryptoTests(unittest.TestCase):
    def setUp(self): self.c = Crypto()
    def test_cmac_known_answers(self):
        for n, expected in [(0,'bb1d6929e95937287fa37d129b756746'),
                             (16,'070a16b46b4d4144f79bdd9dd04a287c'),
                             (40,'dfa66747de9ae63030ca32611497c827'),
                             (64,'51f0bebf7e3b9d92fc49741779363cfe')]:
            self.assertEqual(self.c.cmac(KEY,PLAIN[:n]),H(expected))
    def test_cbc_known_answer(self):
        buf = C.create_string_buffer(CIPHER,64)
        self.c.lib.dt295_payload(KEY,IV,buf,64)
        self.assertEqual(buf.raw,PLAIN)
    def test_cbc_encrypt_known_answer(self):
        self.assertEqual(self.c.cbc(KEY, IV, PLAIN, True), CIPHER)
        self.assertEqual(self.c.cbc(KEY, IV, CIPHER), PLAIN)
        for n in (1, 15, 17):
            with self.assertRaises(ValueError): self.c.cbc(KEY, IV, PLAIN[:n], True)
    def test_short_residual_known_answer(self):
        # AES(KEY, IV) known CFB first-block mask from SP800-38A.
        mask = H('50fe67cc996d32b6da0937e99bafec60')
        for n in range(1,16):
            cipher = bytes(a^b for a,b in zip(PLAIN[:n],mask))
            packet = b'\x47\x00\x20\x70' + bytes([183-n]) + bytes(183-n) + cipher
            out = self.c.packet(KEY,IV,packet)
            self.assertEqual(out[-n:],PLAIN[:n])
            self.assertEqual(out[:3],packet[:3]); self.assertEqual(out[3],0x30)
            self.assertEqual(out[4:-n],packet[4:-n])
    def test_header_validation(self):
        clear = b'\x47\x00\x20\x10' + bytes(184)
        self.assertEqual(self.c.packet(KEY,IV,clear),clear)
        for header in (b'\x46\x00\x20\x50', b'\x47\x80\x20\x50',
                       b'\x47\x00\x20\x40', b'\x47\x00\x20\x90',
                       b'\x47\x00\x20\xd0'):
            with self.assertRaises(ProtocolError): self.c.packet(KEY,IV,header+bytes(184))
        for length in (183,184,255):
            with self.assertRaises(ProtocolError):
                self.c.packet(KEY,IV,b'\x47\x00\x20\x70'+bytes([length])+bytes(183))
    def test_usb_chunking_and_truncation(self):
        packets = [b'\x47\x00\x20\x10' + bytes([i])*184 for i in range(64)]
        data = b''.join(packets)
        for n in (1,187,188,189,4096,65536):
            f=Framer(); actual=[]
            for off in range(0,len(data),n): actual.extend(f.feed(data[off:off+n]))
            f.finish(); self.assertEqual(actual,packets)
        f=Framer(); list(f.feed(data[:-1]))
        with self.assertRaises(ProtocolError): f.finish()
        with self.assertRaises(ProtocolError): list(Framer().feed(b'\x00'+data))


class FakeUSB:
    def __init__(self): self.calls=[]
    def control(self,*args):
        self.calls.append(args)
        return bytes(args[4]) if args[0]&0x80 else None


class RegisterTests(unittest.TestCase):
    def test_address_chunking_wire_order(self):
        u=FakeUSB(); d=Device(u)
        d.read(0x823be,66)
        self.assertEqual(u.calls,[(0xc0,0xbc,0x800,0x23be,64),(0xc0,0xbc,0x800,0x23fe,2)])
        u.calls=[]; d.put(0x90074,0x100)
        self.assertEqual(u.calls,[(0x40,0xbc,0x900,0x74,b'\x01\x00')])
        u.calls=[]; d.write(0x8fff0,bytes(80))
        self.assertEqual(u.calls[1][:4],(0x40,0xbc,0x900,0x30))
    def test_ranges_before_io(self):
        u=FakeUSB(); d=Device(u)
        for addr,n in [(-1,2),(0xffffff,2),(0,65537)]:
            with self.assertRaises(ValueError): d.read(addr,n)
        with self.assertRaises(ValueError): d.field(0,15,2,0)
        self.assertEqual(u.calls,[])


class RenewalTests(unittest.TestCase):
    def test_no_premature_request_or_counter_advance_on_failure(self):
        d=Device(FakeUSB()); d.renewal_counter=1; d.renewal_due=130
        with patch('dt295.time.monotonic', return_value=129.99), patch('dt295.keepalive') as fn:
            self.assertFalse(renew_if_due(d, None)); fn.assert_not_called()
        with patch('dt295.time.monotonic', return_value=130), patch('dt295.keepalive', return_value=2) as fn:
            self.assertTrue(renew_if_due(d, None)); fn.assert_called_once_with(d,None,1)
        self.assertEqual((d.renewal_counter,d.renewal_due),(2,160))
        with patch('dt295.time.monotonic', return_value=160), patch('dt295.keepalive',side_effect=ProtocolError('rejected')):
            with self.assertRaises(ProtocolError): renew_if_due(d,None)
        self.assertEqual((d.renewal_counter,d.renewal_due),(2,160))


class BootTests(unittest.TestCase):
    def test_restart_timeouts_retry_reads_without_repeating_out(self):
        usb = Mock()
        device = Device(usb)
        device.word = Mock(side_effect=[0, 4, USBError(-7, 'restart IN'), 4, 0x10])
        device.wait_event = Mock()
        def field(addr, *args):
            if addr == 0x90070: raise USBError(-7, 'restart OUT')
        device.field = Mock(side_effect=field)
        device.put = Mock()
        with tempfile.TemporaryDirectory() as root:
            firmware = Path(root)/'synthetic.bin'; firmware.write_bytes(bytes(17))
            result = device.boot(firmware)
        self.assertEqual(result['boot_state'], 0x10)
        self.assertEqual(sum(c.args[0] == 0x90070 for c in device.field.call_args_list), 1)
        usb.transfer.assert_called_once_with(2, bytes(17), 300)
        self.assertIn(((0x90074, 4),), device.put.call_args_list)

if __name__ == '__main__': unittest.main()
