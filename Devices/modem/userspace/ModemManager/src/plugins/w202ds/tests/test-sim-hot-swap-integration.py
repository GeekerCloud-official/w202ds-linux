#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise W202DS SIM hotplug in a private D-Bus/abstract AT socket.

No real ports, system services, network interfaces or SIM are accessed.
Requires a tests-enabled
build with plugin_w202ds, cli/mmcli, dbus-daemon and gdbus. Pass --builddir and
--output; runtime library paths are inherited from the caller.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time


class FakeModem:
    def __init__(self, output):
        self.name = f'w202ds-sim-test-{os.getpid()}'
        self.present = True
        self.failure = None
        self.imsi = '460001234567890'
        self.iccid = '98680021436587092143'
        self.cpin_count = 0
        self.iccid_count = 0
        self.clients = set()
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.log = (output / 'at.log').open('w')
        self.commands = {}
        common = Path(__file__).resolve().parents[2] / 'tests/gsm-port.conf'
        for line in common.read_text().splitlines():
            if line.strip() and not line.startswith('#'):
                command, reply = line.split(None, 1)
                self.commands[command] = reply.encode().decode('unicode_escape').encode()
        self.commands.update({
            'AT+CEREG=?': b'\r\n+CEREG: (0-2)\r\n\r\nOK\r\n',
            'AT+C5GREG=?': b'\r\n+C5GREG: (0-2)\r\n\r\nOK\r\n',
            'AT+CEREG?': b'\r\n+CEREG: 2,1,"1234","001122BB",7\r\n\r\nOK\r\n',
            'AT+C5GREG?': b'\r\n+C5GREG: 2,1,"1234","001122BB",11\r\n\r\nOK\r\n',
            'AT+CEREG=2': b'\r\nOK\r\n',
            'AT+C5GREG=2': b'\r\nOK\r\n',
            'AT+CEREG=0': b'\r\nOK\r\n',
            'AT+C5GREG=0': b'\r\nOK\r\n',
            'AT+CFUN=1': b'\r\nOK\r\n',
        })
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind('\0abstract:' + self.name)
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def reply(self, command):
        if command == 'AT+CPIN?':
            self.cpin_count += 1
            if self.failure == 'timeout':
                return b''
            if self.failure == 'sim-failure':
                return b'\r\n+CME ERROR: 13\r\n'
            return b'\r\n+CPIN: READY\r\n\r\nOK\r\n' if self.present else b'\r\n+CME ERROR: 10\r\n'
        if command == 'AT+CIMI':
            return f'\r\n{self.imsi}\r\n\r\nOK\r\n'.encode()
        if command == 'AT+CRSM=176,12258,0,0,10':
            self.iccid_count += 1
            return f'\r\n+CRSM: 144,0,"{self.iccid}"\r\n\r\nOK\r\n'.encode()
        return self.commands.get(command, b'\r\nERROR\r\n')

    def serve(self):
        buffers = {}
        while not self.stop.is_set():
            try:
                client, _ = self.listener.accept()
                client.settimeout(0.01)
                with self.lock:
                    self.clients.add(client)
                    buffers[client] = b''
            except socket.timeout:
                pass
            with self.lock:
                for client in list(self.clients):
                    try:
                        data = client.recv(4096)
                        if not data:
                            self.clients.remove(client)
                            client.close()
                            continue
                        buffers[client] += data
                        while b'\r' in buffers[client]:
                            command, buffers[client] = buffers[client].split(b'\r', 1)
                            command = command.strip().decode()
                            if command:
                                reply = self.reply(command)
                                self.log.write(f'{command} -> {reply!r}\n')
                                self.log.flush()
                                client.sendall(reply)
                    except socket.timeout:
                        pass
                    except (BrokenPipeError, ConnectionResetError):
                        self.clients.remove(client)
                        client.close()

    def notify(self, data=b'\r\n+ECIND: 3,6,0\r\n'):
        with self.lock:
            assert self.clients, 'SIM monitor did not keep AT port open'
            for client in self.clients:
                client.sendall(data)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=5)
        assert not self.thread.is_alive()
        self.listener.close()
        for client in self.clients:
            client.close()
        self.log.close()


def wait_for(check, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError(f'timed out waiting for {check}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builddir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--dbus-daemon', default='dbus-daemon')
    parser.add_argument('--gdbus', default='gdbus')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['G_DEBUG'] = 'fatal-criticals'
    config = args.output / 'bus.conf'
    config.write_text('''<busconfig><type>session</type>
<listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>
<policy context="default"><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>''')
    bus = daemon = fake = None
    try:
        with (args.output / 'dbus.log').open('w') as bus_log, (args.output / 'modemmanager.log').open('w') as daemon_log:
            bus = subprocess.Popen([args.dbus_daemon, '--nofork', '--print-address=1',
                                    '--config-file=' + str(config)],
                                   stdout=subprocess.PIPE, stderr=bus_log, text=True, env=env)
            env['DBUS_SESSION_BUS_ADDRESS'] = bus.stdout.readline().strip()
            assert env['DBUS_SESSION_BUS_ADDRESS'], 'private D-Bus failed to start'
            daemon = subprocess.Popen([str(args.builddir / 'src/ModemManager'), '--test-session',
                '--test-enable', '--no-auto-scan', '--debug'], env=env,
                stdout=daemon_log, stderr=subprocess.STDOUT)

            def run(command, check=True):
                assert daemon.poll() is None, 'ModemManager exited; see modemmanager.log'
                result = subprocess.run(command, env=env, capture_output=True, text=True,
                                        timeout=15)
                if check and result.returncode:
                    raise AssertionError(f'{command}: {result.stderr}')
                return result

            cli = [str(args.builddir / 'cli/mmcli'), '--test-session']
            wait_for(lambda: run(cli + ['-L', '-J'], check=False).returncode == 0, timeout=10)
            fake = FakeModem(args.output)
            run([args.gdbus, 'call', '--session', '--dest', 'org.freedesktop.ModemManager1',
                '--object-path', '/org/freedesktop/ModemManager1',
                '--method', 'org.freedesktop.ModemManager1.Test.SetProfile',
                'w202ds-sim-hot-swap', 'w202ds', f"['abstract:{fake.name}', 'net:sipa_eth0']"])

            snapshots = []

            def snapshot():
                listing = json.loads(run(cli + ['-L', '-J']).stdout)
                paths = listing.get('modem-list', [])
                if len(paths) != 1:
                    return None
                result = run(cli + ['-m', paths[0], '-J'], check=False)
                return json.loads(result.stdout) if result.returncode == 0 else None

            def state(value):
                data = snapshot()
                if data and data['modem']['generic']['state'] == value:
                    snapshots.append(data)
                    return data

            initial = wait_for(lambda: state('disabled'))
            original_path = initial['modem']['dbus-path']
            original_sim = initial['modem']['generic']['sim']
            assert original_sim.startswith('/org/freedesktop/ModemManager1/SIM/')
            print('PASS: initial SIM is available', flush=True)

            # Other ECIND groups must not issue CPIN or reprobe.
            before = fake.cpin_count
            fake.notify(b'\r\n+ECIND: 30,1,1\r\n\r\n+ECIND: 1,3,1\r\n')
            time.sleep(0.5)
            assert fake.cpin_count == before
            assert snapshot()['modem']['dbus-path'] == original_path
            print('PASS: unrelated notifications are ignored', flush=True)

            # An ambiguous error must preserve the current SIM and modem.
            fake.failure = 'sim-failure'
            before = fake.cpin_count
            fake.notify()
            wait_for(lambda: fake.cpin_count > before)
            time.sleep(0.5)
            assert snapshot()['modem']['dbus-path'] == original_path
            assert snapshot()['modem']['generic']['sim'] == original_sim
            fake.failure = None
            print('PASS: SIM failure does not falsely remove the card', flush=True)

            # A delayed/lost AT response must leave the SIM in place. A hint
            # arriving while that query is pending must be checked afterwards.
            fake.failure = 'timeout'
            before = fake.cpin_count
            fake.notify()
            wait_for(lambda: fake.cpin_count > before)
            assert snapshot()['modem']['generic']['sim'] == original_sim
            fake.failure = None
            identity_before = fake.iccid_count
            fake.notify()
            wait_for(lambda: fake.iccid_count > identity_before, timeout=15)
            time.sleep(0.5)
            assert snapshot()['modem']['dbus-path'] == original_path
            assert snapshot()['modem']['generic']['sim'] == original_sim
            print('PASS: AT timeout preserves SIM and queued checks recover', flush=True)

            run(cli + ['-m', original_path, '--enable'])
            wait_for(lambda: state('registered'))
            before = fake.cpin_count
            fake.notify(b'\r\n+CEREG: 0\r\n\r\n+C5GREG: 0\r\n')
            wait_for(lambda: fake.cpin_count > before)
            wait_for(lambda: state('enabled'))
            assert snapshot()['modem']['dbus-path'] == original_path
            assert snapshot()['modem']['generic']['sim'] == original_sim
            print('PASS: registration loss checks CPIN without falsely removing SIM', flush=True)

            # Exercise removal and duplicate-event coalescing during one read.
            fake.present = False
            fake.notify(b'\r\n+ECIND: 3,6,0\r\n' * 8)
            missing = wait_for(lambda: state('failed'))
            assert missing['modem']['generic']['state-failed-reason'] == 'sim-missing'
            assert missing['modem']['generic']['sim'] == '--'
            missing_path = missing['modem']['dbus-path']
            assert missing_path != original_path
            time.sleep(0.5)
            assert snapshot()['modem']['dbus-path'] == missing_path
            print('PASS: removal clears the SIM and exports SIM_MISSING', flush=True)

            # Already absent events must not cause endless reprobing.
            before = fake.cpin_count
            fake.notify()
            wait_for(lambda: fake.cpin_count > before)
            time.sleep(0.5)
            assert snapshot()['modem']['dbus-path'] == missing_path
            print('PASS: repeated removal keeps the missing state stable', flush=True)

            fake.present = True
            fake.imsi = '460009876543210'
            fake.iccid = '98680021436587092165'
            fake.notify(b'\r\n+ECIND: 3,1,1\r\n')
            inserted = wait_for(lambda: state('disabled'))
            assert inserted['modem']['dbus-path'] != missing_path
            assert inserted['modem']['generic']['sim'] != original_sim
            sim = json.loads(run(cli + ['-i', inserted['modem']['generic']['sim'], '-J']).stdout)
            assert sim['sim']['properties']['imsi'] == fake.imsi
            print('PASS: insertion creates a fresh SIM without restarting the daemon', flush=True)
            (args.output / 'snapshots.json').write_text(json.dumps(snapshots + [sim], indent=2) + '\n')
    finally:
        for process in (daemon, bus):
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if fake is not None:
            fake.close()


if __name__ == '__main__':
    main()
