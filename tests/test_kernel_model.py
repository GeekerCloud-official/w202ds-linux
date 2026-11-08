"""Model/parser checks; real Kconfig replay is validated separately on CM2."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('kernel_model', ROOT / 'scripts/kernel-build.py')
MODEL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODEL)


class KernelModelTests(unittest.TestCase):
    def setUp(self):
        root = Path(os.environ.get('KERNEL_MODEL_TEST_OUTPUT', ROOT / 'output/kernel-model-tests'))
        root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def model(self, entries):
        return self.write('model.toml', 'schema = 1\ndefault = "debug"\narch = "arm64"\n' + entries)

    def test_inheritance_and_ordered_external_overrides(self):
        self.write('base.config', 'CONFIG_TEST=y\nCONFIG_NUMBER=1\n')
        self.write('desktop.config', 'CONFIG_NUMBER=2\n')
        self.write('debug.config', 'CONFIG_NUMBER=3\n')
        path = self.model('[profiles.base]\nfragments=["base.config"]\n'
                          '[profiles.desktop]\nextends=["base"]\nfragments=["desktop.config"]\n'
                          '[profiles.debug]\nextends=["desktop"]\nfragments=["debug.config"]\n')
        first = self.write('first.config', 'CONFIG_NUMBER=4\n')
        last = self.write('last.config', 'CONFIG_TEST=m\nCONFIG_NUMBER=5\n')
        plan = MODEL.compose(path, fragments=[first, last])
        self.assertEqual(plan['layers'], ['base', 'desktop', 'debug'])
        self.assertEqual(plan['requested']['CONFIG_NUMBER']['value'], '5')
        self.assertEqual(plan['requested']['CONFIG_TEST']['value'], 'm')
        self.assertEqual(plan['requested']['CONFIG_NUMBER']['file'], str(last))
        self.assertEqual(plan['requested']['CONFIG_NUMBER']['line'], 2)
        self.assertFalse(MODEL.unmet_settings(plan['requested'], MODEL.parse_config(MODEL.seed_config(plan['requested']), 'result')))

    def test_repeated_external_fragment_is_applied_again(self):
        path = self.model('[profiles.debug]\nfragments=["base.config"]\n')
        self.write('base.config', 'CONFIG_NUMBER=1\n')
        first = self.write('first.config', 'CONFIG_NUMBER=2\n')
        second = self.write('second.config', 'CONFIG_NUMBER=3\n')
        self.assertEqual(MODEL.compose(path, fragments=[first, second, first])['requested']['CONFIG_NUMBER']['value'], '2')

    def test_shared_ancestor_does_not_override_later_parent(self):
        self.write('base.config', 'CONFIG_NUMBER=1\n')
        self.write('left.config', 'CONFIG_NUMBER=2\n')
        path = self.model('[profiles.base]\nfragments=["base.config"]\n'
                          '[profiles.left]\nextends=["base"]\nfragments=["left.config"]\n'
                          '[profiles.right]\nextends=["base"]\n'
                          '[profiles.debug]\nextends=["left", "right"]\n')
        plan = MODEL.compose(path)
        self.assertEqual(plan['layers'], ['base', 'left', 'right', 'debug'])
        self.assertEqual(plan['requested']['CONFIG_NUMBER']['value'], '2')

    def test_cycle_is_rejected(self):
        path = self.model('[profiles.debug]\nextends=["desktop"]\n'
                          '[profiles.desktop]\nextends=["debug"]\n')
        with self.assertRaisesRegex(MODEL.ConfigError, 'cycle'):
            MODEL.compose(path)

    def test_missing_parent_is_rejected(self):
        path = self.model('[profiles.debug]\nextends=["missing"]\n')
        with self.assertRaisesRegex(MODEL.ConfigError, 'Unknown kernel profile'):
            MODEL.compose(path)

    def test_manifest_typo_is_rejected(self):
        path = self.model('[profiles.debug]\nfragment=["base.config"]\n')
        with self.assertRaisesRegex(MODEL.ConfigError, 'invalid profile'):
            MODEL.compose(path)

    def test_invalid_fragment_lines_are_rejected(self):
        for data in (b'CONFIG_TEST=yes\n', b'CONFIG_TEST=y # comment\n',
                     b'CONFG_TEST=y\n', b'# CONFIG_TEST disabled\n'):
            with self.subTest(data=data), self.assertRaises(MODEL.ConfigError):
                MODEL.parse_config(data, 'bad.config')

    def test_duplicate_symbol_is_rejected(self):
        with self.assertRaisesRegex(MODEL.ConfigError, 'duplicate setting'):
            MODEL.parse_config(b'CONFIG_TEST=y\nCONFIG_TEST=m\n', 'duplicate.config')

    def test_strings_numbers_and_disabled_options_round_trip(self):
        data = (r'CONFIG_TEXT="a \"quote\""' + '\nCONFIG_NUMBER=0x10\n# CONFIG_OFF is not set\n').encode()
        settings = MODEL.parse_config(data, 'input')
        final = MODEL.parse_config(MODEL.seed_config(settings), 'output')
        self.assertFalse(MODEL.unmet_settings(settings, final))

    def test_dependency_loss_reports_the_request_location(self):
        settings = MODEL.parse_config(b'CONFIG_TEST=y\n', 'external.config')
        unmet = MODEL.unmet_settings(settings, {})
        self.assertEqual(unmet[0]['requested']['file'], 'external.config')
        self.assertEqual(unmet[0]['actual'], 'n')

    def test_kconfig_environment_does_not_leak_into_build(self):
        with patch.dict(os.environ, KCONFIG_ALLCONFIG='/unrelated/config',
                        KBUILD_OUTPUT='/unrelated/output', MAKEFLAGS='-e', LOCALVERSION='-unexpected'):
            env = MODEL.clean_environment()
        self.assertNotIn('KCONFIG_ALLCONFIG', env)
        self.assertNotIn('KBUILD_OUTPUT', env)
        self.assertNotIn('MAKEFLAGS', env)
        self.assertEqual(env['LOCALVERSION'], '')
        self.assertEqual(env['KCONFIG_WERROR'], '1')

    def test_plan_does_not_create_output(self):
        output = self.root / 'does-not-exist'
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'scripts/kernel-build.py'),
                                 'plan', '--out', str(output)], check=True, capture_output=True, text=True)
        report = json.loads(result.stdout)
        self.assertEqual(report['profile'], 'debug')
        self.assertEqual(report['layers'], ['desktop', 'debug'])
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
