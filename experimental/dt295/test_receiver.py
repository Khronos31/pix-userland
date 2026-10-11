import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock
from dt295 import Device, Crypto, ProtocolError, USBError
from receiver import ReceiveProfile, Receiver


def profile():
    return ReceiveProfile({'schema':1,'target':0,'ts_key_request_words':[0x12345678,0x90abcdef,0],
        'fields':[{'address':0x1000,'shift':8,'width':2,'value':2}]})


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.firmware=Path(self.temp.name)/'synthetic.bin';self.firmware.write_bytes(bytes(17))
        self.d=Mock();self.d.session_key=bytes(16)
        self.d.secure_command.return_value=bytes(range(16));self.r=Receiver(self.d)

    def test_profile_validation_precedes_device_access(self):
        valid={'schema':1,'target':0,'ts_key_request_words':[1,2,3],
               'fields':[{'address':0x1000,'shift':8,'width':8,'value':255}]}
        for bad in ({'target':1},{'ts_key_request_words':[0,1,-1]},
                    {'fields':[{'address':0x90070,'shift':0,'width':1,'value':1}]},
                    {'fields':[{'address':0x1001,'shift':0,'width':1,'value':1}]},
                    {'fields':[{'address':0x1000,'shift':15,'width':2,'value':1}]}):
            with self.assertRaises(ValueError):ReceiveProfile(dict(valid,**bad))
        self.assertEqual(profile().key_request,bytes.fromhex('1234567890abcdef00000000'))

    def test_key_precedes_prepare_and_start(self):
        self.d.word.side_effect=[0x11,0x11,1];self.d.drain_states.side_effect=[2,2,4];self.d.usb.transfer.return_value=bytes(188)
        self.r.prepare(profile(),self.firmware);self.r.start()
        calls=self.d.mock_calls
        key=next(i for i,c in enumerate(calls) if c[0]=='secure_command')
        load=next(i for i,c in enumerate(calls) if c[0]=='load_trc')
        commands=[(i,c.args) for i,c in enumerate(calls)if c[0]=='command']
        self.assertLess(key,load);self.assertLess(load,commands[0][0])
        self.assertEqual([a for _,a in commands],[(4,0x20),(5,0,2)])
        self.assertEqual(self.r.key,bytes(range(16)))
        self.d.wait_state.assert_any_call(2)
        self.r.stop();self.d.configure_idle.assert_called_once()
        self.assertEqual([c.args for c in self.d.command.call_args_list],
                         [(4,0x20),(5,0,2),(5,0,4),(5,0,1)])
        self.assertIsNone(self.r.key)

    def test_reusable_stop_reloads_configuration_without_new_key_request(self):
        self.r.key=bytes(16);self.r.session_key=self.d.session_key
        self.r.pause_state=4;self.r.started=True
        self.r.profile=profile();self.r.firmware=self.firmware
        self.d.word.return_value=17
        self.d.drain_states.side_effect=[2,4,17]
        self.r.stop(reusable=True)
        self.r.rearm()
        self.d.word.return_value=1
        self.r.start()
        self.assertEqual([c.args for c in self.d.command.call_args_list],
                         [(5,0,4),(5,0,1),(4,0x20),(5,0,2)])
        self.d.secure_command.assert_not_called();self.d.load_trc.assert_called_once_with(self.firmware)
        self.assertEqual(self.r.key,bytes(16))

    def test_rearm_requires_same_session_and_failure_cannot_be_retried(self):
        self.r.key=bytes(16);self.r.session_key=self.d.session_key
        self.r.reusable=True;self.d.drain_states.return_value=17
        self.r.profile=profile();self.r.firmware=self.firmware
        self.d.word.return_value=17
        self.d.command.side_effect=USBError(-7,'timeout')
        with self.assertRaises(USBError):self.r.rearm()
        with self.assertRaises(ProtocolError):self.r.rearm()
        self.d.command.assert_called_once_with(4,0x20)
        self.d.command.reset_mock();self.r.reusable=True
        self.d.session_key=b'different session'
        with self.assertRaises(ProtocolError):self.r.rearm()
        self.d.command.assert_not_called()

    def test_key_failure_never_uploads_or_starts(self):
        self.d.word.return_value=0x11
        for reply in (b'',bytes(12),bytes(20)):
            self.d.secure_command.return_value=reply
            with self.assertRaises(ProtocolError):self.r.prepare(profile(),self.firmware)
        self.d.secure_command.side_effect=ProtocolError('rejected')
        with self.assertRaises(ProtocolError):self.r.prepare(profile(),self.firmware)
        self.d.load_trc.assert_not_called();self.d.command.assert_not_called()
        with self.assertRaises(ProtocolError):self.r.start()

    def test_error_recovery_tolerates_old_error_notification_without_resending(self):
        d=Device(Mock());d.session_key=bytes(16);d.renewal_counter=5
        d.word=Mock(side_effect=[0xffff,4,4,0xffff,4,16])
        d.put=Mock();d.command=Mock()
        from unittest.mock import patch
        with patch('dt295.time.sleep'):d.recover_session()
        d.command.assert_called_once_with(3)
        self.assertIsNone(d.session_key);self.assertIsNone(d.renewal_counter)
        self.assertEqual(d.put.call_count,3)

    def test_error_recovery_refuses_healthy_device_and_bounds_failed_transition(self):
        from unittest.mock import patch
        for state in (0,1,2,16,17):
            d=Device(Mock());d.word=Mock(return_value=state);d.command=Mock()
            with self.assertRaises(ProtocolError):d.recover_session()
            d.command.assert_not_called()
        d=Device(Mock());d.word=Mock(side_effect=[0xffff,0,0,0xffff]);d.command=Mock()
        with patch('dt295.time.monotonic',side_effect=[0,6]):
            with self.assertRaises(ProtocolError):d.recover_session()
        d.command.assert_called_once_with(3)

    def test_timeout_does_not_repeat_start_and_stop_checks_hardware(self):
        self.d.word.side_effect=[0x11,0x11,1];self.d.drain_states.side_effect=[2,2,4];self.d.usb.transfer.return_value=bytes(188)
        self.r.prepare(profile(),self.firmware)
        self.d.command.side_effect=USBError(-7,'start timeout')
        with self.assertRaises(USBError):self.r.start()
        with self.assertRaises(ProtocolError):self.r.start()
        self.assertEqual(sum(c.args==(5,0,2)for c in self.d.command.call_args_list),1)
        self.d.command.side_effect=None;self.r.stop()
        self.d.command.assert_called_with(5,0,1)

    def test_latched_state_is_acknowledged_and_error_aborts(self):
        d=Device(Mock());queue=[1,2]
        d.word=Mock(side_effect=lambda addr: (4 if queue else 0) if addr==0x90074 else (queue[0] if queue else 2))
        d.put=Mock(side_effect=lambda addr,value:queue.pop(0))
        self.assertEqual(d.wait_state(2),2)
        self.assertEqual([c.args for c in d.put.call_args_list],[(0x90074,4)]*2)
        d.word=Mock(side_effect=lambda addr:4 if addr==0x90074 else 0xffff)
        d.put=Mock()
        with self.assertRaises(ProtocolError):d.wait_state(2)
        d.put.assert_called_once_with(0x90074,4)

    def test_state_wait_is_bounded_without_reissuing_commands(self):
        from unittest.mock import patch
        d=Device(Mock());d.drain_states=Mock(return_value=1)
        with patch('dt295.time.monotonic',side_effect=[0,1]):
            with self.assertRaises(ProtocolError):d.wait_state(2,timeout=10)
        d.usb.control.assert_not_called()

    def test_pause_drains_bulk_without_repeating_pause_command(self):
        self.r.pause_state=4
        self.d.drain_states.side_effect=[2,2,2,4]
        self.d.usb.transfer.side_effect=[USBError(-7,'timeout'),bytes(188)]
        self.assertEqual(self.r.stop(),188)
        self.assertEqual([c.args for c in self.d.usb.transfer.call_args_list],[(0x81,65536,10)]*2)
        self.assertEqual([c.args for c in self.d.command.call_args_list],[(5,0,4),(5,0,1)])
        self.d.configure_idle.assert_called_once()

    def test_pause_disconnect_aborts_without_idle_command(self):
        self.r.pause_state=4;self.r.key=bytes(16);self.r.started=True
        self.d.drain_states.return_value=2
        self.d.usb.transfer.side_effect=USBError(-4,'disconnected')
        with self.assertRaises(USBError):self.r.stop()
        self.d.command.assert_called_once_with(5,0,4)
        self.d.configure_idle.assert_not_called()
        self.assertIsNone(self.r.key);self.assertFalse(self.r.started)

    def test_pause_timeout_does_not_force_idle(self):
        from unittest.mock import patch
        self.r.pause_state=4;self.d.drain_states.return_value=2
        with patch('receiver.time.monotonic',side_effect=[0,6]):
            with self.assertRaises(ProtocolError):self.r.stop()
        self.d.command.assert_called_once_with(5,0,4)
        self.d.configure_idle.assert_not_called()
        self.d.usb.transfer.assert_not_called()

    def test_dma_input_selects_pause_state(self):
        for value, state in [(0,4),(1,3)]:
            p=ReceiveProfile({'schema':1,'target':0,'ts_key_request_words':[0,0,0],
                'fields':[{'address':0x1002,'shift':8,'width':2,'value':value}]})
            self.assertEqual(p.pause_state,state)

    def test_secure_result_and_wire_encryption(self):
        d=Device(Mock());d.crypto=Crypto();d.session_key=bytes(range(16));d.session_iv=bytes(16)
        d.write=Mock();key=bytes(range(16,32))
        def response(prefix=0x12345678,n=4,status=0,rid=0x07010600):
            raw=struct.pack('>4I',rid,status,n,prefix)+key+bytes(480)
            return raw[:12]+d.crypto.cbc(d.session_key,d.session_iv,raw[12:508],True)+raw[508:]
        d.command=Mock(return_value=response())
        request=profile().key_request
        self.assertEqual(d.secure_command(0x07010600,request),key)
        wire=d.write.call_args.args[1]
        clear=d.crypto.cbc(d.session_key,d.session_iv,wire[12:508])
        self.assertEqual(clear[:16],b'\xff'*4+request)
        d.command.return_value=response(status=7)
        with self.assertRaises(ProtocolError):d.secure_command(0x07010600,request)
        d.command.return_value=response(rid=0x07010601)
        with self.assertRaises(ProtocolError):d.secure_command(0x07010600,request)
        d.command.return_value=response(n=125)
        with self.assertRaises(ProtocolError):d.secure_command(0x07010600,request)

if __name__=='__main__':unittest.main()
