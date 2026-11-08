"""Reject stale kernels before module builds, including in a new module OUT."""

import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import kernel_state as STATE
import build as BUILD


class KernelStateTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'output/kernel-state-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.kernel = self.root / 'kernel'
        self.kernel.mkdir()
        (self.kernel / 'Makefile').write_text('# kernel fixture\n')
        self.linked = self.root / 'linked-driver'
        self.linked.mkdir()
        (self.linked / 'driver.c').write_text('/* builtin source */\n')
        (self.kernel / 'board-driver').symlink_to('../linked-driver', target_is_directory=True)
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.name=State Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'baseline')
        self.fragment = self.root / 'base.config'
        self.fragment.write_text('CONFIG_MODULES=y\n')
        self.model = self.root / 'model.toml'
        self.model.write_text('schema=1\narch="arm64"\ndefault="debug"\n'
                              '[profiles.debug]\nfragments=["base.config"]\n')
        self.plan = STATE.model_module().compose(self.model)
        self.output = self.root / 'output'
        self.output.mkdir()
        files = {'Makefile': '# output fixture\n', '.config': 'CONFIG_MODULES=y\n',
                 'Module.symvers': '0x0000 symbol vmlinux EXPORT_SYMBOL\n',
                 'arch/arm64/boot/Image': 'image\n', 'include/config/kernel.release': '6.18.35-test\n',
                 'include/config/auto.conf': 'CONFIG_MODULES=y\n',
                 'include/generated/autoconf.h': '#define CONFIG_MODULES 1\n',
                 'include/generated/rustc_cfg': '', 'include/generated/utsrelease.h': 'release\n',
                 'arch/arm64/include/generated/asm/test.h': 'header\n',
                 'scripts/basic/fixdep': 'fixdep\n', 'scripts/mod/modpost': 'modpost\n',
                 'scripts/module.lds': 'SECTIONS {}\n'}
        for name, data in files.items():
            path = self.output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data)
        (self.output / 'source').symlink_to('../kernel', target_is_directory=True)
        self.state = {'schema': 1, 'kind': 'w202ds-kernel-output', 'arch': 'arm64', 'profile': 'debug',
                      'host_system': platform.system(), 'host_machine': platform.machine(),
                      'kernel_commit': self.git('rev-parse', 'HEAD'),
                      'source_identity': STATE.source_identity(self.kernel),
                      'configuration': STATE.configuration(self.plan), 'kernelrelease': '6.18.35-test',
                      'config_sha256': STATE.sha256(self.output / '.config'),
                      'symvers_sha256': STATE.sha256(self.output / 'Module.symvers'),
                      'files': STATE.file_records(self.output)}
        self.save()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.kernel), *args], text=True,
                                       stderr=subprocess.DEVNULL).strip()

    def save(self):
        (self.output / 'kernel-state.json').write_text(json.dumps(self.state))

    def args(self, **overrides):
        values = dict(kernel_out=self.output, modules_out=None, skip_kernel=False,
                      skip_kernel_check=False, model=self.model, profile=None, fragment=[],
                      kernel=self.kernel, cross_compile='test-', jobs=2)
        return argparse.Namespace(**dict(values, **overrides))

    def test_valid_output_and_derived_module_location(self):
        result = BUILD.prepare(self.args(skip_kernel=True))
        self.assertTrue(result['freshness_checked'])
        self.assertEqual(result['modules'], self.output / 'modules')

    def test_dirty_source_is_rejected_even_without_a_previous_module_receipt(self):
        (self.kernel / 'Makefile').write_text('# source changed\n')
        with self.assertRaisesRegex(ValueError, 'dirty'):
            BUILD.prepare(self.args(skip_kernel=True))
        self.assertFalse((self.output / 'modules').exists())
        self.assertFalse(BUILD.prepare(self.args(skip_kernel_check=True))['freshness_checked'])

    def test_new_clean_git_commit_is_rejected(self):
        (self.kernel / 'Makefile').write_text('# committed change\n')
        self.git('-c', 'user.name=State Test', '-c', 'user.email=test@example.invalid', 'commit', '-qam', 'new')
        with self.assertRaisesRegex(ValueError, 'Git commit changed'):
            STATE.verify_output(self.output)

    def test_configuration_fragment_change_requires_explicit_override(self):
        self.fragment.write_text('CONFIG_MODULES=y\nCONFIG_CPU_FREQ=y\n')
        with self.assertRaisesRegex(ValueError, 'configuration inputs/profile changed'):
            BUILD.prepare(self.args(skip_kernel=True))
        self.assertEqual(BUILD.prepare(self.args(skip_kernel_check=True))['kernelrelease'], '6.18.35-test')

    def test_changed_model_and_external_fragment_are_checked(self):
        self.model.write_text(self.model.read_text() + '# model changed\n')
        with self.assertRaisesRegex(ValueError, 'configuration inputs/profile changed'):
            STATE.verify_output(self.output)
        extra = self.root / 'external.config'
        extra.write_text('CONFIG_CPU_FREQ=y\n')
        with self.assertRaisesRegex(ValueError, 'configuration inputs/profile changed'):
            BUILD.prepare(self.args(fragment=[extra]))

    def test_builtin_devices_source_is_checked_outside_kernel_git(self):
        (self.linked / 'driver.c').write_text('/* changed builtin */\n')
        self.assertEqual(self.git('status', '--porcelain'), '')
        with self.assertRaisesRegex(ValueError, 'Kernel-linked Devices source changed'):
            STATE.verify_output(self.output)

    def test_output_corruption_is_rejected_even_with_skip_checks(self):
        for name in ('.config', 'Module.symvers', 'include/generated/autoconf.h',
                     'include/config/kernel.release', 'arch/arm64/boot/Image', 'scripts/mod/modpost'):
            with self.subTest(name=name):
                path = self.output / name
                before = path.read_bytes()
                path.write_bytes(before + b'changed')
                try:
                    with self.assertRaisesRegex(ValueError, 'file changed or missing'):
                        STATE.verify_output(self.output, check_freshness=False)
                finally:
                    path.write_bytes(before)

    def test_unrecorded_generated_header_is_rejected(self):
        (self.output / 'include/generated/added.h').write_text('new header\n')
        with self.assertRaisesRegex(ValueError, 'Unrecorded generated'):
            STATE.verify_output(self.output)

    def test_dirty_release_is_rejected(self):
        release = self.output / 'include/config/kernel.release'
        release.write_text('6.18.35-test-dirty\n')
        self.state['kernelrelease'] = release.read_text().strip()
        self.state['files'] = STATE.file_records(self.output)
        self.save()
        with self.assertRaisesRegex(ValueError, 'release is dirty'):
            STATE.verify_output(self.output)

    def test_skip_requires_an_explicit_existing_output(self):
        for flag in ('skip_kernel', 'skip_kernel_check'):
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, 'explicit --kernel-out'):
                BUILD.prepare(self.args(kernel_out=None, **{flag: True}))
        with self.assertRaisesRegex(ValueError, 'no kernel output'):
            BUILD.prepare(self.args(kernel_out=self.root / 'absent', skip_kernel=True))
        self.assertFalse((self.root / 'absent').exists())

    def test_missing_output_builds_kernel_before_validation(self):
        model = STATE.model_module()
        events = []
        expected = STATE.verify_output(self.output)
        def generate(*args):
            events.append('kernel build')
        def verify(*args, **kwargs):
            events.append('kernel verification')
            return expected
        with patch.object(BUILD, 'model_module', return_value=model), \
                patch.object(model, 'generate', side_effect=generate), \
                patch.object(BUILD, 'verify_output', side_effect=verify):
            BUILD.prepare(self.args(kernel_out=self.root / 'new'))
        self.assertEqual(events, ['kernel build', 'kernel verification'])

    def test_module_entry_rejects_tampering_before_creating_new_out(self):
        (self.output / '.config').write_text('tampered\n')
        modules = self.root / 'new-modules'
        command = [sys.executable, '-B', str(ROOT / 'Devices/build-module.py'),
                   '--id', 'test-driver', '--devices', str(ROOT / 'Devices'),
                   '--source', str(ROOT / 'Devices/hall/source/w202-hall-input'),
                   '--target', 'w202_hall_input.ko', '--kdir', str(self.output), '--out', str(modules)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('file changed or missing: .config', result.stderr)
        self.assertFalse(modules.exists())


if __name__ == '__main__':
    unittest.main()
