import io
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

from capture import capture
from dt295 import ProtocolError


class CaptureTests(unittest.TestCase):
    def run_capture(self, output, card_error=None):
        d = Mock(session_key=bytes(16))
        d.word.side_effect = [0, 2]
        d.drain_states.side_effect = [2, 17]
        packet = b'\x47\x01\x00\x10' + bytes(184)
        d.usb.transfer.return_value = packet * 2
        with patch('capture.driver_material'), patch('capture.authenticate'), \
             patch('capture.Crypto') as crypto, patch('capture.Card') as card, \
             patch('capture.Receiver') as receiver, patch('capture.Terrestrial'), \
             patch('capture.renew_if_due'), patch('capture.time.monotonic', side_effect=[0, 0, 61]):
            crypto.return_value.packet.side_effect = lambda key, iv, b: b
            card.return_value.initialize.side_effect = card_error
            if card_error:
                # A failed card initialization happens before receiver preparation.
                d.drain_states.side_effect = [17]
                with self.assertRaisesRegex(ProtocolError, 'card rejected'):
                    capture(d, Path('.'), Mock(), bytes(16), 'T27', 60, output)
                receiver.return_value.prepare.assert_not_called()
                d.usb.transfer.assert_not_called()
            else:
                try:
                    capture(d, Path('.'), Mock(), bytes(16), 'T27', 60, output)
                finally:
                    receiver.return_value.stop.assert_called_once()
        return packet * 2

    def test_stdout_contains_only_ts(self):
        out = io.BytesIO()
        expected = self.run_capture(out)
        self.assertEqual(out.getvalue(), expected)

    def test_rejected_card_does_not_start_reception(self):
        out = io.BytesIO()
        self.run_capture(out, ProtocolError('card rejected'))
        self.assertEqual(out.getvalue(), b'')

    def test_consumer_closing_output_stops_receiver(self):
        with self.assertRaises(BrokenPipeError):
            self.run_capture(Mock(write=Mock(side_effect=BrokenPipeError())))
