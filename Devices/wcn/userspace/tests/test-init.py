#!/usr/bin/env python3
"""Exercise the real initializer over a PTY acting as an H4 controller."""
import errno
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import time

binary, firmware = map(Path, sys.argv[1:])


def run_controller(fail_opcode=None):
    master, slave = pty.openpty()
    command = [str(binary.resolve()), '-d', os.ttyname(slave),
               '-p', str(firmware / 'bt_configure_pskey.ini'),
               '-r', str(firmware / 'bt_configure_rf.ini'),
               '-a', '02:13:24:35:46:57']
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    received = []
    deadline = time.monotonic() + 5

    def read_exact(count):
        data = b''
        while len(data) < count:
            remaining = deadline - time.monotonic()
            assert remaining > 0, 'controller timeout'
            assert select.select([master], [], [], remaining)[0], 'controller timeout'
            try:
                data += os.read(master, count - len(data))
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
        return data

    try:
        for opcode, length in [(0xFCA0, 176), (0xFCA2, 252), (0xFCA1, 3)]:
            header = read_exact(4)
            kind, actual, size = struct.unpack('<BHB', header)
            assert (kind, actual, size) == (1, opcode, length), header.hex()
            params = read_exact(size)
            if opcode == 0xFCA0:
                assert params[20:26] == bytes.fromhex('574635241302'), params[20:26].hex()
            if opcode == 0xFCA1:
                assert params == b'\x00\x00\x01'
            received.append(opcode)
            # Unrelated event must be ignored. Split the real reply to test reads.
            os.write(master, b'\x04\xff\x01\x00')
            payload = b'\x01' + struct.pack('<H', opcode)
            if opcode == 0xFCA1:
                payload += b'\x00\x00' + bytes([0 if fail_opcode == opcode else 1])
            else:
                payload += bytes([1 if fail_opcode == opcode else 0])
            reply = b'\x04\x0e' + bytes([len(payload)]) + payload
            os.write(master, reply[:2])
            os.write(master, reply[2:])
            if opcode == fail_opcode:
                break
        stdout, stderr = process.communicate(timeout=2)
        assert process.returncode == (1 if fail_opcode else 0), (stdout, stderr)
        if fail_opcode:
            assert b'failed (1)' in stderr, stderr
            assert not select.select([master], [], [], 0)[0], 'commands sent after failure'
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)
    return received


assert run_controller() == [0xFCA0, 0xFCA2, 0xFCA1]
assert run_controller(0xFCA0) == [0xFCA0]
assert run_controller(0xFCA2) == [0xFCA0, 0xFCA2]
assert run_controller(0xFCA1) == [0xFCA0, 0xFCA2, 0xFCA1]
for address in ['102:13:24:35:46:57', '02:13:24:35:46:57junk', '02:13:24:35:46:zz']:
    result = subprocess.run([str(binary.resolve()), '-p', str(firmware / 'bt_configure_pskey.ini'),
                             '-r', str(firmware / 'bt_configure_rf.ini'), '-a', address],
                            capture_output=True, timeout=2)
    assert result.returncode == 2 and b'bad address' in result.stderr, result
print('PASS: H4 command order, lengths, address endian, unrelated/fragmented events, PSKey/RF/enable rejection, malformed addresses')
