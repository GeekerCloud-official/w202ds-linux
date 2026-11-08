#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Recorded protocol vectors; no device or Android code is needed."""
import io
import unittest
from unittest.mock import patch

import w202ds_gnss as gnss

# Captured from this board, before sending any navigation command.
BOOT = bytes.fromhex('7e7e7e7e03070000fffb0000')
INIT = bytes.fromhex('7e7e7e7e0503000efdf14e41563a494e4954205355430d0a4641')
WAKE = bytes.fromhex('7e7e7e7e04010002feff0100feff')


class ProtocolTests(unittest.TestCase):
    def test_board_vectors_and_every_split(self):
        packet = BOOT + INIT + WAKE
        expected = [(3, 7, b''), (5, 3, b'NAV:INIT SUC\r\n'), (4, 1, b'\x01\x00')]
        for split in range(len(packet) + 1):
            parser = gnss.FrameParser()
            self.assertEqual(parser.feed(packet[:split]) + parser.feed(packet[split:]), expected)
        self.assertEqual(gnss.encode(5, 3, b'NAV:INIT SUC\r\n'), INIT)
        self.assertEqual(gnss.encode(4, 1, b'\x01\x00'), WAKE)
        self.assertEqual(gnss.encode(3, 7), BOOT)

    def test_recover_after_noise_bad_crc_and_bad_length(self):
        damaged = bytearray(INIT)
        damaged[10] ^= 1
        # Header has a valid checksum but an unsupported 65535-byte length.
        bad_size = bytes.fromhex('7e7e7e7e0503fffffdfb')
        parser = gnss.FrameParser()
        result = parser.feed(b'noise' + damaged + bad_size + BOOT + INIT)
        self.assertEqual(result, [(3, 7, b''), (5, 3, b'NAV:INIT SUC\r\n')])
        self.assertEqual(parser.errors['bad_payload'], 1)
        self.assertGreaterEqual(parser.errors['bad_header'], 1)

    def test_nmea_validity_is_preserved(self):
        # Actual no-fix output. Never turn RMC V / GGA 0 into a claimed fix.
        line = b'$GPGSV,1,1,1,7,1,,33*4E\r\n'
        invalid = b'$GPGSV,1,1,1,7,1,,33*00\r\n'
        session = gnss.Session()
        output = io.BytesIO()
        with patch.object(gnss.sys, 'stdout') as stdout:
            stdout.buffer = output
            session.nmea_buffer.extend(line[:10])
            session.emit_nmea()
            self.assertEqual(output.getvalue(), b'')
            session.nmea_buffer.extend(line[10:] + invalid)
            session.emit_nmea()
        self.assertEqual(output.getvalue(), line)
        self.assertEqual(session.nmea_count, 1)
        self.assertEqual(session.parser.errors['bad_nmea'], 1)


if __name__ == '__main__':
    unittest.main()
