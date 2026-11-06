#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run against a native build of modem-at-fixture.c, not against live hardware."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class ContextResponse(unittest.TestCase):
    def parse(self, fields, expected=0):
        reply = '\r\n+CGCONTRDP: ' + fields + '\r\n\r\nOK\r\n'
        result = subprocess.run([BINARY, 'parse'], input=reply, text=True, capture_output=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result.stdout

    def test_local_response_with_netmask(self):
        self.assertEqual(self.parse('1,6,cmnet,10.137.54.215.255.0.0.0,,120.196.165.7,221.179.38.7,,,,,0,,0'),
                         'IPV4=10.137.54.215\nDNS1=120.196.165.7\nDNS2=221.179.38.7\n')

    def test_quotes_without_netmask(self):
        self.assertEqual(self.parse('1,6,"internet","10.2.3.4","","223.5.5.5","8.8.8.8",'),
                         'IPV4=10.2.3.4\nDNS1=223.5.5.5\nDNS2=8.8.8.8\n')

    def test_optional_dns(self):
        self.assertEqual(self.parse('1,6,apn,10.2.3.4,,0.0.0.0,8.8.8.8,'),
                         'IPV4=10.2.3.4\nDNS1=\nDNS2=8.8.8.8\n')

    def test_bad_fields_rejected(self):
        for address in ['999.1.2.3', '0.0.0.0', '127.0.0.1', '224.1.2.3',
                        '10.1.2.3.255.0.255.0', '10.1.2.3.255.0.0', '10.1.2.3;id']:
            with self.subTest(address=address):
                self.assertEqual(self.parse(f'1,6,apn,{address},,8.8.8.8,,', 4), '')
        for dns in ['0.0.0.0', '8.8.8.8;id', '127.0.0.1', '']:
            with self.subTest(dns=dns):
                self.assertEqual(self.parse(f'1,6,apn,10.1.2.3,,{dns},,', 4), '')

    def test_no_cid1(self):
        self.assertEqual(self.parse('11,6,ims,10.1.2.3,,8.8.8.8,,', 4), '')
        result = subprocess.run([BINARY, 'parse'], input='\r\nOK\r\n', text=True, capture_output=True)
        self.assertEqual(result.returncode, 4)

    def test_truncated_and_oversized(self):
        self.parse('1,6,apn,"10.1.2.3', 4)
        self.parse('1,6,' + 'a' * 160 + ',10.1.2.3,,8.8.8.8,,', 4)


@unittest.skipUnless(os.geteuid() == 0, 'Root ownership is part of the production lock contract')
class ChannelLock(unittest.TestCase):
    def test_concurrent_reader_and_release(self):
        with tempfile.TemporaryDirectory() as parent:
            directory = str(Path(parent) / 'locks')
            holder = subprocess.Popen([BINARY, 'lock', directory, 'hold'], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertEqual(holder.stdout.readline(), 'locked\n')
                busy = subprocess.run([BINARY, 'lock', directory, 'once'], capture_output=True)
                self.assertEqual(busy.returncode, 75, busy.stderr)
            finally:
                holder.communicate('\n', timeout=5)
            self.assertEqual(holder.returncode, 0)
            acquired = subprocess.run([BINARY, 'lock', directory, 'once'], capture_output=True)
            self.assertEqual(acquired.returncode, 0, acquired.stderr)

    def test_reject_symlink_and_writable_lock(self):
        with tempfile.TemporaryDirectory() as parent:
            directory = Path(parent) / 'locks'
            directory.mkdir()
            dev = os.stat('/dev/null').st_rdev
            lock = directory / f'channel-{os.major(dev)}-{os.minor(dev)}.lock'
            target = Path(parent) / 'untouched'
            target.write_text('preserved')
            lock.symlink_to(target)
            result = subprocess.run([BINARY, 'lock', str(directory), 'once'], capture_output=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(target.read_text(), 'preserved')
            lock.unlink()
            lock.touch()
            lock.chmod(0o666)
            result = subprocess.run([BINARY, 'lock', str(directory), 'once'], capture_output=True)
            self.assertEqual(result.returncode, 1)
            directory.chmod(0o777)
            result = subprocess.run([BINARY, 'lock', str(directory), 'once'], capture_output=True)
            self.assertEqual(result.returncode, 1)


if __name__ == '__main__':
    unittest.main()
