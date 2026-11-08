#!/usr/bin/env python3
"""Exercise recovery/sleep shell control flow with fake sysfs and commands."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

rootfs = Path(sys.argv[1]).resolve()
work_root = Path(sys.argv[2]).resolve()
work_root.mkdir(parents=True, exist_ok=True)

DISPATCH = r'''
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
state_path = Path(os.environ['W202_TEST_STATE'])
state = json.loads(state_path.read_text())
event = {'command': name, 'args': args}
soft = Path(os.environ['W202_TEST_SOFT'])
if soft.exists():
    event['soft'] = soft.read_text().strip()
with Path(os.environ['W202_TEST_LOG']).open('a') as log:
    log.write(json.dumps(event) + '\n')
code = 0
if name == 'systemctl':
    words = [arg for arg in args if not arg.startswith('-')]
    if words[0] == 'is-active':
        code = 0 if state['bluetooth'] else 3
    elif words[0] == 'stop':
        state['bluetooth'] = False
    elif words[0] == 'start':
        state['bluetooth'] = True
    else:
        raise AssertionError(words)
elif name == 'ip':
    if args[:2] == ['link', 'show']:
        print('1: wlan0: <BROADCAST,MULTICAST' + (',UP' if state['up'] else '') + '>')
    elif args[:2] == ['link', 'set']:
        state['up'] = args[-1] == 'up'
    else:
        raise AssertionError(args)
elif name == 'nmcli':
    print('100 (connected)' if state['nm_ready'] else '20 (unavailable)')
elif name != 'sleep':
    raise AssertionError(name)
state_path.write_text(json.dumps(state))
sys.exit(code)
'''


class Harness:
    def __init__(self, *, present=True, bluetooth=True, up=True, blocked=False,
                 nm_ready=True):
        self.temp = tempfile.TemporaryDirectory(prefix='recovery-', dir=work_root)
        self.base = Path(self.temp.name)
        self.net = self.base / 'sys/class/net'
        self.soft = self.net / 'wlan0/phy80211/rfkill0/soft'
        if present:
            self.soft.parent.mkdir(parents=True)
            self.soft.write_text('1' if blocked else '0')
        self.marker = self.base / 'run/w202-wcn-bt-stopped'
        self.marker.parent.mkdir()
        self.state = self.base / 'state.json'
        self.state.write_text(json.dumps({'bluetooth': bluetooth, 'up': up,
                                          'nm_ready': nm_ready}))
        self.log = self.base / 'commands.jsonl'
        commands = self.base / 'bin'
        commands.mkdir()
        dispatcher = commands / 'dispatcher'
        dispatcher.write_text('#!' + sys.executable + '\n' + DISPATCH)
        dispatcher.chmod(0o755)
        for command in ('systemctl', 'ip', 'nmcli', 'sleep'):
            (commands / command).symlink_to(dispatcher)
        self.env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ['PATH'],
                        W202_TEST_STATE=str(self.state), W202_TEST_LOG=str(self.log),
                        W202_TEST_SOFT=str(self.soft))

    def run(self, kind, *args):
        source = rootfs / ('usr/libexec/w202-wcn-reopen' if kind == 'reopen' else
                           'usr/lib/systemd/system-sleep/w202-wcn')
        # Redirect only filesystem locations; run the actual shell flow unchanged.
        text = source.read_text().replace('/sys/class/net/', str(self.net) + '/')
        text = text.replace('/run/w202-wcn-bt-stopped', str(self.marker))
        script = self.base / (kind + '.sh')
        script.write_text(text)
        result = subprocess.run(['sh', str(script), *args], env=self.env,
                                capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, (result.stdout, result.stderr)

    def events(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def actions(self):
        return [event['args'] for event in self.events() if event['command'] == 'systemctl'
                and any(arg in ('stop', 'start') for arg in event['args'])]

    def close(self):
        self.temp.cleanup()


def exercise(options, check):
    harness = Harness(**options)
    try:
        check(harness)
    finally:
        harness.close()


def absent(h):
    h.run('reopen')
    assert h.events() == []


def active_recovery(h):
    h.run('reopen')
    assert h.actions() == [['stop', 'w202-bluetooth.service'],
                           ['--no-block', 'start', 'w202-bluetooth.service']]
    assert json.loads(h.state.read_text())['bluetooth']
    assert h.soft.read_text().strip() == '0'
    assert sum(event['command'] == 'sleep' and event['args'] == ['1']
               for event in h.events()) == 1


def inactive_recovery(h):
    h.run('reopen')
    assert h.actions() == []
    assert not json.loads(h.state.read_text())['bluetooth']


def down_blocked(h):
    h.run('reopen')
    state = json.loads(h.state.read_text())
    assert not state['up'] and not state['bluetooth']
    assert h.soft.read_text().strip() == '1'
    changes = [event['args'][-1] for event in h.events()
               if event['command'] == 'ip' and event['args'][:2] == ['link', 'set']]
    assert changes == ['up', 'down']


def exhausted(h):
    h.run('reopen')
    assert sum(event['command'] == 'sleep' and event['args'] == ['1']
               for event in h.events()) == 3
    assert h.actions()[-1] == ['--no-block', 'start', 'w202-bluetooth.service']


def sleep_active(h):
    h.run('sleep', 'pre')
    assert h.marker.exists() and not json.loads(h.state.read_text())['bluetooth']
    h.run('sleep', 'post')
    assert not h.marker.exists() and json.loads(h.state.read_text())['bluetooth']
    assert len(h.actions()) == 2
    h.run('sleep', 'post')
    assert len(h.actions()) == 2


def sleep_inactive(h):
    h.run('sleep', 'pre')
    h.run('sleep', 'post')
    assert not h.marker.exists() and not json.loads(h.state.read_text())['bluetooth']
    assert h.actions() == []


exercise({'present': False}, absent)
exercise({}, active_recovery)
exercise({'bluetooth': False}, inactive_recovery)
exercise({'bluetooth': False, 'up': False, 'blocked': True}, down_blocked)
exercise({'nm_ready': False}, exhausted)
exercise({}, sleep_active)
exercise({'bluetooth': False}, sleep_inactive)
service = (rootfs / 'usr/lib/systemd/system/w202-wcn-reopen.service').read_text()
rule = (rootfs / 'usr/lib/udev/rules.d/70-w202-wcn.rules').read_text()
assert 'ExecStart=/usr/libexec/w202-wcn-reopen' in service
assert 'FW_ERROR|TX_STALL' in rule and 'start w202-wcn-reopen.service' in rule
print('PASS: recovery ordering, inactive-state preservation, bounded retries, '
      'sleep marker lifecycle and installed service/rule paths')
