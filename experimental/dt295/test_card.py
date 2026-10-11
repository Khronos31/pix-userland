import unittest
from unittest.mock import Mock, patch

from card import Card
from dt295 import Device, ProtocolError


class CardTests(unittest.TestCase):
    def device(self):
        return Mock(session_key=bytes(16), drain_states=Mock(return_value=0x11))

    def test_notification_consumed_before_ack_other_bits_untouched(self):
        d = self.device()
        d.word.side_effect = [0x284, 0x84]
        d.read.side_effect = [b'\x30\x00' + bytes(272), b'\x30\x00']
        self.assertEqual(Card(d).state(), 3)
        d.put.assert_called_once_with(0x90074, 0x200)
        self.assertEqual(d.mock_calls[1].args, (0x822ac, 274))
        self.assertEqual(d.mock_calls[2].args, (0x90074, 0x200))

    def test_pending_response_or_short_read_not_acknowledged(self):
        for pending, data in [(0x600, bytes(274)), (0x200, bytes(2))]:
            d = self.device(); d.word.return_value = pending; d.read.return_value = data
            with self.assertRaises(ProtocolError): Card(d).state()
            d.put.assert_not_called()

    def test_activation_waits_for_card_ready(self):
        d = self.device(); card = Card(d)
        with patch.object(card, 'state', side_effect=[1, 2, 3]), patch('card.time.sleep'):
            self.assertEqual(card.activate(), 3)
        d.command.assert_called_once_with(0x0a, 1, 1, i2c=True)

    def test_initialization_failure_prevents_status_read_and_no_retry(self):
        d = self.device(); card = Card(d)
        d.command.side_effect = ProtocolError('rejected')
        with patch.object(card, 'state') as state:
            with self.assertRaisesRegex(ProtocolError, 'rejected'): card.initialize()
            state.assert_not_called()
        d.command.assert_called_once_with(0x0a, 0, 1, i2c=True)

    def test_absent_or_error_card_never_reset_or_activated(self):
        for state in (0, 2, 4, 5, 15):
            d = self.device(); card = Card(d)
            card.notified_state = state
            with patch.object(card, 'state', return_value=state):
                with self.assertRaises(ProtocolError): card.activate()
            d.command.assert_not_called()

    def test_initial_register_zero_is_not_a_card_absence_notification(self):
        d = self.device(); card = Card(d)
        with patch.object(card, 'state', side_effect=[0, 0, 3]), patch('card.time.sleep'):
            self.assertEqual(card.activate(), 3)
        d.command.assert_called_once_with(0x0a, 1, 1, i2c=True)

    def test_card_command_uses_secondary_usb_and_completion_lane(self):
        usb = Mock(); d = Device(usb)
        # Event 8 from the primary lane must not look like a stale secondary
        # completion. Secondary completion comes from 80016 and ACKs only 2.
        with patch.object(d, 'word', side_effect=[8, 0x0a00]) as word, \
             patch.object(d, 'wait_event') as wait, patch.object(d, 'put') as put:
            d.command(0x0a, 1, 1, i2c=True)
        usb.control.assert_called_once_with(0x40, 0xb9, 0, 0, b'\x0a\x01\x00\x01')
        self.assertEqual(word.call_args_list[-1].args, (0x80016,))
        wait.assert_called_once_with(2, 5000)
        put.assert_called_once_with(0x90074, 2)

    def test_ready_card_not_reactivated_and_auth_required(self):
        d = self.device(); card = Card(d)
        with patch.object(card, 'state', return_value=3):
            self.assertEqual(card.activate(), 3)
        d.command.assert_not_called()
        d.session_key = None
        with self.assertRaises(ProtocolError): card.activate()
        d.command.assert_not_called()

    def test_timeout_and_command_failure_never_retried(self):
        d = self.device(); card = Card(d)
        with patch.object(card, 'state', return_value=1), patch('card.time.monotonic', side_effect=[0, 3]):
            with self.assertRaisesRegex(ProtocolError, 'timed out'): card.activate()
        d.command.assert_called_once()
        d.command.reset_mock(); d.command.side_effect = ProtocolError('rejected')
        with patch.object(card, 'state', return_value=1):
            with self.assertRaisesRegex(ProtocolError, 'rejected'): card.activate()
        d.command.assert_called_once()
