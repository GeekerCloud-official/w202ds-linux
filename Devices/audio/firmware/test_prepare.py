#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Adversarial tests for externally supplied firmware containers and XML."""
import copy
import struct
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import prepare


def profile_xml():
    root = ET.Element("dsp_vbc", struct_size="0x6c4", num_mode="0x48")
    node = ET.SubElement(ET.SubElement(ET.SubElement(root, "Music"), "Handsfree"),
                         "Playback", mode="64")
    for offset in range(0, prepare.PROFILE_SIZE, 4):
        ET.SubElement(node, f"word{offset}", id=hex(64 * prepare.PROFILE_SIZE + offset),
                      t="u32", offset="0", bits="32", val=hex(0x12345678 + offset))
    return root, node


class FirmwareTests(unittest.TestCase):
    def test_little_endian_profile_and_hal_selector(self):
        root, _ = profile_xml()
        out = prepare.prepare_profile(ET.tostring(root), 75)
        self.assertEqual(struct.unpack_from("<8sIII", out),
                         (b"W202VBC\0", 0x404B0000, 0x6C4, 0x12345678))

    def test_rejects_missing_overlapping_or_out_of_range_words(self):
        for kind in ("missing", "duplicate", "outside"):
            with self.subTest(kind=kind):
                root, node = profile_xml()
                if kind == "missing":
                    node.remove(node[2])
                elif kind == "duplicate":
                    node.append(copy.deepcopy(node[2]))
                else:
                    node[0].set("id", "0xffffffff")
                with self.assertRaises(ValueError):
                    prepare.serialize_profile(ET.tostring(root))

    def test_rejects_overflowing_and_missing_bits(self):
        for value, width in (("0x100000000", "32"), ("0", "31")):
            root, node = profile_xml()
            node[0].set("val", value)
            node[0].set("bits", width)
            with self.assertRaises(ValueError):
                prepare.serialize_profile(ET.tostring(root))

    def test_rejects_overlapping_subfields(self):
        root, node = profile_xml()
        word = node[0]
        for key in ("val", "offset", "bits"):
            del word.attrib[key]
        ET.SubElement(word, "a", offset="0", bits="20", val="0")
        ET.SubElement(word, "b", offset="16", bits="16", val="0")
        with self.assertRaises(ValueError):
            prepare.serialize_profile(ET.tostring(root))

    def test_avb_header_retained_and_signature_area_trimmed(self):
        image = bytearray(0x600000)
        prefix = b"SharkL5_AUDCP_"
        image[:len(prefix)] = prefix
        image[0x500000:0x500004] = b"AVB0"
        struct.pack_into(">4sIIQQQ28s", image, len(image) - 64, b"AVBf", 1, 0,
                         0x500000, 0x500000, 0x840, bytes(28))
        with patch.object(prepare, "AUDCP_HASHES", {prepare.sha(image)}):
            out = prepare.prepare_audcp(image)
        self.assertEqual(len(out), 0x500000)
        self.assertEqual(out[:0x80], image[:0x80])
        struct.pack_into(">Q", image, len(image) - 64 + 20, 0x600000)
        with patch.object(prepare, "AUDCP_HASHES", {prepare.sha(image)}):
            with self.assertRaises(ValueError):
                prepare.prepare_audcp(image)

    def test_rejects_unknown_firmware_and_acf(self):
        with self.assertRaises(ValueError):
            prepare.prepare_audcp(bytes(0x600000))
        with self.assertRaises(ValueError):
            prepare.prepare_acf(bytes(100))


if __name__ == "__main__":
    unittest.main()
