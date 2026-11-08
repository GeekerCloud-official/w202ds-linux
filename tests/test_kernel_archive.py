"""Archive integrity, relocation and filesystem protection checks."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EXPORT = load('kernel-export')
IMPORT = load('kernel-import')
from kernel_state import configuration, file_records, model_module, source_identity, verify_output


class KernelArchiveTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get('KERNEL_ARCHIVE_TEST_OUTPUT', ROOT / 'output/kernel-archive-tests'))
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.kernel = self.root / 'kernel'
        self.kernel.mkdir()
        (self.kernel / 'Makefile').write_text('# test source identity\n')
        self.git('init', '-q')
        self.git('add', 'Makefile')
        self.git('-c', 'user.name=Archive Test', '-c', 'user.email=archive@example.invalid', 'commit', '-qm', 'baseline')
        self.build = self.root / 'built'
        self.build.mkdir()
        self.archive = self.root / 'kernel.tar.gz'
        self.destination = self.root / 'moved/imported'
        self.files = {'.config':b'CONFIG_MODULES=y\n', 'Module.symvers':b'0x00000000 symbol vmlinux EXPORT_SYMBOL\n',
                      'include/config/kernel.release':b'6.18.35-test\n', 'include/config/auto.conf':b'CONFIG_MODULES=y\n',
                      'include/generated/autoconf.h':b'#define CONFIG_MODULES 1\n', 'include/generated/rustc_cfg':b'',
                      'include/generated/utsrelease.h':b'#define UTS_RELEASE "6.18.35-test"\n',
                      'arch/arm64/include/generated/asm/test.h':b'#define TEST 1\n',
                      'scripts/basic/fixdep':b'host fixdep fixture\n', 'scripts/mod/modpost':b'host modpost fixture\n',
                      'scripts/module.lds':b'SECTIONS {}\n', 'arch/arm64/boot/Image':b'kernel image fixture\n'}
        for name, data in self.files.items():
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.build / 'scripts/basic/fixdep').chmod(0o755)
        for name in ('vmlinux', 'leftover.o', 'already-built.ko', 'build.log', 'include/.old.cmd'):
            (self.build / name).write_text('must not be exported')
        report = {'status':'built', 'arch':'arm64', 'kernel_commit':self.git('rev-parse', 'HEAD'),
                  'kernelrelease':'6.18.35-test', 'config_sha256':self.digest('.config'),
                  'symvers_sha256':self.digest('Module.symvers'),
                  'image_sha256':self.digest('arch/arm64/boot/Image'), 'compiler':'test compiler'}
        (self.build / 'kernel-build.json').write_text(json.dumps(report))

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.kernel), *args], stderr=subprocess.DEVNULL, text=True).strip()

    def digest(self, name):
        return hashlib.sha256((self.build / name).read_bytes()).hexdigest()

    def export(self):
        return EXPORT.export(self.build, self.kernel, self.archive)

    def rewrite(self, destination, change):
        with tarfile.open(self.archive, 'r:gz') as before:
            files = {member.name:before.extractfile(member).read() for member in before.getmembers()}
        change(files)
        with tarfile.open(destination, 'w:gz') as after:
            for name, data in files.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                after.addfile(entry, io.BytesIO(data))

    def test_relocated_import_preserves_baseline_and_omits_runtime_modules_and_objects(self):
        result = self.export()
        binding = IMPORT.import_output(self.archive, self.destination, self.kernel)
        self.assertEqual(binding['kernelrelease'], '6.18.35-test')
        self.assertEqual((self.destination / 'build/source').resolve(), self.kernel)
        self.assertFalse(os.path.isabs(os.readlink(self.destination / 'build/source')))
        self.assertEqual((self.destination / 'Image').read_bytes(), self.files['arch/arm64/boot/Image'])
        self.assertEqual((self.destination / 'build/Module.symvers').read_bytes(), self.files['Module.symvers'])
        self.assertTrue((self.destination / 'build/scripts/basic/fixdep').stat().st_mode & 0o100)
        self.assertEqual(list(self.destination.rglob('*.ko')), [])
        self.assertEqual(list(self.destination.rglob('*.o')), [])
        self.assertFalse((self.destination / 'build/vmlinux').exists())
        self.assertFalse((self.destination / 'build/build.log').exists())
        self.assertFalse((self.destination / 'build/kernel-build.json').exists())
        self.assertEqual(result['image_bytes'], len(self.files['arch/arm64/boot/Image']))

    def test_corrupted_image_does_not_publish_partial_output(self):
        self.export()
        broken = self.root / 'corrupted.tar.gz'
        self.rewrite(broken, lambda files:files.update(Image=b'X' + files['Image'][1:]))
        with self.assertRaisesRegex(ValueError, 'checksum'):
            IMPORT.import_output(broken, self.destination, self.kernel)
        self.assertFalse(self.destination.exists())

    def test_archive_cannot_escape_output(self):
        self.export()
        broken = self.root / 'traversal.tar.gz'
        self.rewrite(broken, lambda files:files.update({'../outside':b'bad'}))
        with self.assertRaisesRegex(ValueError, 'Unsafe archive path'):
            IMPORT.import_output(broken, self.destination, self.kernel)
        self.assertFalse((self.destination.parent / 'outside').exists())
        self.assertFalse(self.destination.exists())

    def test_links_are_rejected_before_extraction(self):
        with tarfile.open(self.archive, 'w:gz') as stream:
            entry = tarfile.TarInfo('build/linked')
            entry.type = tarfile.SYMTYPE
            entry.linkname = '/tmp'
            stream.addfile(entry)
        with self.assertRaisesRegex(ValueError, 'regular files'):
            IMPORT.import_output(self.archive, self.destination, self.kernel)
        self.assertFalse(self.destination.exists())

    def test_source_revision_mismatch_does_not_create_output(self):
        self.export()
        (self.kernel / 'Makefile').write_text('# changed source\n')
        self.git('-c', 'user.name=Archive Test', '-c', 'user.email=archive@example.invalid', 'commit', '-qam', 'different')
        with self.assertRaisesRegex(ValueError, 'commit does not match'):
            IMPORT.import_output(self.archive, self.destination, self.kernel)
        self.assertFalse(self.destination.exists())

    def test_existing_user_output_and_archive_are_preserved(self):
        self.archive.write_bytes(b'existing user archive')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.export()
        self.assertEqual(self.archive.read_bytes(), b'existing user archive')
        self.destination.mkdir(parents=True)
        sentinel = self.destination / 'keep'
        sentinel.write_text('user data')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            IMPORT.import_output(self.archive, self.destination, self.kernel)
        self.assertEqual(sentinel.read_text(), 'user data')

    def test_export_rejects_configuration_changed_after_build(self):
        (self.build / '.config').write_bytes(b'CONFIG_MODULES=y\nCONFIG_CHANGED=y\n')
        with self.assertRaisesRegex(ValueError, 'completed build report'):
            self.export()
        self.assertFalse(self.archive.exists())

    def test_provenance_survives_export_import_and_rejects_later_config_change(self):
        model = self.root / 'model.toml'
        model.write_text('schema=1\narch="arm64"\ndefault="debug"\n'
                         '[profiles.debug]\nfragments=["base.config"]\n')
        fragment = self.root / 'base.config'
        fragment.write_text('CONFIG_MODULES=y\n')
        (self.build / 'Makefile').write_text('# full output fixture\n')
        (self.build / 'source').symlink_to('../kernel', target_is_directory=True)
        report = json.loads((self.build / 'kernel-build.json').read_text())
        state = dict(report, schema=1, kind='w202ds-kernel-output',
                     host_system=platform.system(), host_machine=platform.machine(),
                     source_identity=source_identity(self.kernel),
                     configuration=configuration(model_module().compose(model)), files=file_records(self.build))
        (self.build / 'kernel-state.json').write_text(json.dumps(state))
        self.export()
        IMPORT.import_output(self.archive, self.destination, self.kernel)
        self.assertTrue(verify_output(self.destination)['freshness_checked'])
        self.assertEqual(verify_output(self.destination / 'build')['root'], self.destination)
        fragment.write_text('CONFIG_MODULES=y\nCONFIG_CPU_FREQ=y\n')
        with self.assertRaisesRegex(ValueError, 'configuration inputs/profile changed'):
            verify_output(self.destination)
        self.assertFalse(verify_output(self.destination, check_freshness=False)['freshness_checked'])


if __name__ == '__main__':
    unittest.main()
