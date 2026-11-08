#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Experimental Marlin GNSS session over the existing SIPC navigation pipe.

Protocol facts were checked against this board's stock libgnssmgt.so with REA.
This does not load Android libraries or modify Android/NV partitions.
"""
import argparse
from collections import Counter
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import struct
import sys
import time

MAGIC = b'\x7e' * 4
MAX_PAYLOAD = 2038


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack('>' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def encode(kind, subtype, payload=b''):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError('GNSS payload too large')
    header = MAGIC + struct.pack('>BBH', kind, subtype, len(payload))
    # The stock encoder leaves the zero-payload trailer zeroed.
    return (header + struct.pack('>H', checksum(header)) + payload +
            struct.pack('>H', checksum(payload) if payload else 0))


class FrameParser:
    def __init__(self):
        self.buffer = bytearray()
        self.errors = Counter()

    def feed(self, data):
        self.buffer.extend(data)
        packets = []
        while len(self.buffer) >= 4:
            index = self.buffer.find(MAGIC)
            if index < 0:
                # Preserve a possible split sync word.
                self.errors['discarded_bytes'] += max(0, len(self.buffer) - 3)
                del self.buffer[:-3]
                break
            if index:
                self.errors['discarded_bytes'] += index
                del self.buffer[:index]
            if len(self.buffer) < 10:
                break
            kind, subtype, length = struct.unpack('>BBH', self.buffer[4:8])
            if length > MAX_PAYLOAD or checksum(bytes(self.buffer[:10])):
                self.errors['bad_header'] += 1
                del self.buffer[0]
                continue
            if len(self.buffer) < length + 12:
                break
            payload = bytes(self.buffer[10:10 + length])
            trailer = struct.unpack('>H', self.buffer[10 + length:12 + length])[0]
            if length and trailer != checksum(payload):
                self.errors['bad_payload'] += 1
                del self.buffer[0]
                continue
            packets.append((kind, subtype, payload))
            del self.buffer[:12 + length]
        return packets


def log(event, **fields):
    print(json.dumps(dict(event=event, **fields)), file=sys.stderr, flush=True)


class Session:
    def __init__(self, device='/dev/data0_gnss'):
        self.device = device
        self.fd = None
        self.lock = None
        self.owned = False
        self.awake = False
        self.parser = FrameParser()
        self.counts = Counter()
        self.nmea_count = 0
        self.nmea_buffer = bytearray()
        self.raw = None

    def __enter__(self):
        try:
            self.lock = open('/run/lock/w202ds-gnss.lock', 'a')
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            radios = [p for p in Path('/sys/class/rfkill').glob('*')
                      if (p / 'name').read_text().strip() == 'w202ds-gnss']
            if len(radios) != 1:
                raise RuntimeError('Expected one w202ds-gnss rfkill; load w202ds_gnss_power')
            self.radio = radios[0]
            if int((self.radio / 'soft').read_text()) != 1:
                raise RuntimeError('GNSS already requested by another owner')
            self.fd = os.open(self.device, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
            # A failed enable can retain the module's request if cleanup failed.
            self.owned = True
            (self.radio / 'soft').write_text('0\n')
            self.poller = select.poll()
            self.poller.register(self.fd, select.POLLIN)
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise
        return self

    def __exit__(self, *unused):
        try:
            if self.owned:
                try:
                    if self.awake:
                        try:
                            self.send(7, 3)
                            idle = self.receive(1, lambda p: p[:2] == (8, 3))
                            self.send(4, 1, b'\x02\x00')
                            asleep = self.receive(1, lambda p: p[:2] == (4, 1)
                                                  and p[2][:1] == b'\x02')
                            log('navigation_stopped', idle_ack=idle, sleep_ack=asleep)
                        except (OSError, EOFError, TimeoutError) as error:
                            log('stop_error', message=str(error))
                finally:
                    try:
                        (self.radio / 'soft').write_text('1\n')
                        log('power_blocked', soft=(self.radio / 'soft').read_text().strip())
                    finally:
                        self.owned = False
        finally:
            if self.raw is not None:
                self.raw.close()
                self.raw = None
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None
            if self.lock is not None:
                self.lock.close()

    def send(self, kind, subtype, payload=b''):
        packet = encode(kind, subtype, payload)
        offset = 0
        deadline = time.monotonic() + 2
        while offset < len(packet):
            try:
                written = os.write(self.fd, packet[offset:])
            except BlockingIOError:
                written = 0
            if written:
                offset += written
            else:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('GNSS write timeout')
                select.select([], [self.fd], [], remaining)
        log('tx', kind=kind, subtype=subtype, payload_hex=payload.hex())

    def receive(self, timeout, expected=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not self.poller.poll(max(1, int((deadline - time.monotonic()) * 1000))):
                continue
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if not data:
                raise EOFError('GNSS transport closed')
            if self.raw and not self.raw.closed:
                self.raw.write(data)
                self.raw.flush()
            matched = False
            for packet in self.parser.feed(data):
                kind, subtype, payload = packet
                self.counts[f'{kind:02x}:{subtype:02x}'] += 1
                count = self.counts[f'{kind:02x}:{subtype:02x}']
                # Keep diagnostics bounded; never label a firmware ACK as a fix.
                if count <= 3:
                    log('rx', kind=kind, subtype=subtype, length=len(payload),
                        sample_hex=payload[:96].hex())
                if (kind, subtype) == (5, 1) or kind == 12:
                    self.nmea_buffer.extend(payload)
                    self.emit_nmea()
                if expected and expected(packet):
                    matched = True
            if matched:
                return True
        return False

    def emit_nmea(self):
        while b'\n' in self.nmea_buffer:
            line, _, tail = self.nmea_buffer.partition(b'\n')
            self.nmea_buffer[:] = tail
            line = line.strip(b'\r')
            if not line.startswith(b'$') or b'*' not in line:
                continue
            body, check = line[1:].rsplit(b'*', 1)
            crc = 0
            for byte in body:
                crc ^= byte
            try:
                valid = len(check) == 2 and int(check, 16) == crc
            except ValueError:
                valid = False
            if valid:
                sys.stdout.buffer.write(line + b'\r\n')
                sys.stdout.buffer.flush()
                self.nmea_count += 1
            else:
                self.parser.errors['bad_nmea'] += 1
        if len(self.nmea_buffer) > 8192:
            self.nmea_buffer.clear()
            self.parser.errors['nmea_overflow'] += 1

    def start(self):
        self.receive(0.25)
        # Original gps_SendNotify(WAKEUP_NOTIFY): [ready=1, sequence=0].
        self.awake = True
        self.send(4, 1, b'\x01\x00')
        if not self.receive(3, lambda p: p[:2] == (4, 1) and p[2][:1] == b'\x01'):
            raise TimeoutError('No stock-protocol wake ACK; navigation not started')
        self.send(2, 1)  # Read firmware version, never clear flash or aiding data.
        self.receive(0.25)
        # Standalone NMEA output: disable the Android CMCC output override.
        self.send(1, 33, struct.pack('<I', 0))
        # SET_OUTTYPE: NMEA bit 1; SET_INTERVAL counts navigation epochs.
        self.send(1, 12, struct.pack('<I', 2))
        self.send(1, 13, struct.pack('<I', 1))
        # Standalone operation: no Android SUPL/network-assisted service here.
        self.send(1, 34, struct.pack('<I', 0))
        utc_ms = int((time.time() - 315964800) * 1000)
        week, tow_ms = divmod(utc_ms, 604800000)
        self.send(7, 4, struct.pack('<II', week, tow_ms))
        if not self.receive(3, lambda p: p[:2] == (8, 5)):
            raise TimeoutError('No stock-protocol idle-off ACK; navigation unconfirmed')
        log('navigation_acknowledged')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duration', type=int, default=30,
                        help='Finite observation interval after navigation ACK (1..3600 seconds)')
    parser.add_argument('--raw', type=Path, help='Private binary RX trace for diagnostics')
    args = parser.parse_args()
    if not 1 <= args.duration <= 3600:
        parser.error('--duration must be 1..3600')
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    signal.signal(signal.SIGINT, lambda *_: sys.exit(130))
    try:
        with Session() as session:
            if args.raw:
                fd = os.open(args.raw, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                session.raw = os.fdopen(fd, 'wb')
            session.start()
            session.receive(args.duration)
            log('summary', packets=dict(session.counts), nmea=session.nmea_count,
                parse_errors=dict(session.parser.errors))
    except (OSError, RuntimeError, TimeoutError, EOFError) as error:
        log('error', message=str(error))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
