#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[1] / 'runtime/w202-modem-storage.py'
SPEC = importlib.util.spec_from_file_location('modem_storage', SOURCE)
storage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(storage)


class NVStorageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.image = Path(self.directory.name) / 'nv.img'
        self.data = bytearray(8192)
        # A zero payload has the known one's-complement checksum 0xffff.
        struct.pack_into('<4I', self.data, 0, 0x4E56, 4096, 0xFFFF, 0x65)
        self.image.write_bytes(self.data)

    def validate(self):
        storage.validate_nv(self.image, 4096, 8192)

    def test_valid_full_image(self):
        self.validate()

    def test_truncated_image_is_not_silently_padded(self):
        self.image.write_bytes(self.data[:1024])
        with self.assertRaisesRegex(ValueError, 'full 8192-byte'):
            self.validate()

    def test_changed_payload_requires_matching_checksum(self):
        self.data[512] = 1
        self.image.write_bytes(self.data)
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.validate()
        struct.pack_into('<I', self.data, 8, 0xFFFE)
        self.image.write_bytes(self.data)
        self.validate()

    def test_wrong_nv_type_is_rejected(self):
        struct.pack_into('<I', self.data, 4, 8192)
        self.image.write_bytes(self.data)
        with self.assertRaisesRegex(ValueError, 'unexpected NV header'):
            self.validate()

    def test_symlink_image_is_rejected(self):
        original = self.image.with_name('outside.img')
        self.image.rename(original)
        self.image.symlink_to(original)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.validate()

    def test_unrelated_loop_mapping_is_rejected(self):
        result = {'loopdevices': [{'name': '/dev/loop0',
                                  'back-file': '/var/lib/some-other-project/image',
                                  'offset': 0, 'sizelimit': 0, 'ro': False}]}
        with patch.object(storage, 'run', return_value=json.dumps(result)):
            with self.assertRaisesRegex(ValueError, 'incompatible loop mapping'):
                storage.loop_info(self.image)


if __name__ == '__main__':
    unittest.main()
