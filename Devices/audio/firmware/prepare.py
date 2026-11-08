#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare local W202DS firmware from the owner's factory files; no device IO."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

AUDCP_HASHES = {
    "ccb87fc69fc7a22a54544824fb22a5541fcb5da7afba72ae03867f2c2a2ae9d8",
    "2150b2d49033ae990484e0d6b7f64b42ebccaa5e933769d57841dddbe718f57d",
}
ACF_HASH = "36173c4963efe4d3f580933bee544bb97754416550f874f67c9bb3894982c10b"
PROFILE_PATH = "Music/Handsfree/Playback"
PROFILE_SIZE = 0x6C4


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def prepare_audcp(data):
    require(len(data) == 0x600000, "expected a complete 6 MiB AUDCP partition")
    require(sha(data) in AUDCP_HASHES, "unrecognized W202DS AUDCP image SHA256")
    magic, major, minor, size, offset, length, _ = struct.unpack_from(
        ">4sIIQQQ28s", data, len(data) - 64)
    require((magic, major, minor) == (b"AVBf", 1, 0), "invalid AVB footer")
    require(0x80 < size <= offset and offset + length <= len(data) - 64,
            "AVB extent outside the partition")
    require(data[offset:offset + 4] == b"AVB0", "invalid vbmeta magic")
    require(data.startswith(b"SharkL5_AUDCP_"), "invalid AUDCP header")
    # The boot vector uses reservation base + 0x80: retain the original header.
    return data[:size]


def serialize_profile(data):
    root = ET.fromstring(data)
    require(root.tag == "dsp_vbc", "expected dsp_vbc.xml")
    size = int(root.attrib["struct_size"], 0)
    require(size == PROFILE_SIZE, "unsupported DSP VBC structure size")
    profile = root.find(PROFILE_PATH)
    require(profile is not None, "factory speaker playback mode is absent")
    mode = int(profile.attrib["mode"], 0)
    require(0 <= mode < int(root.attrib["num_mode"], 0), "invalid profile index")
    result, covered = bytearray(size), bytearray(size)
    for item in profile:
        offset = int(item.attrib["id"], 0) - mode * size
        kind = item.attrib["t"]
        require(kind.startswith("u") and kind[1:].isdigit(), "unsupported scalar type")
        bits = int(kind[1:])
        require(bits > 0 and bits % 8 == 0, "unaligned scalar type")
        length = bits // 8
        require(0 <= offset <= size - length, "profile field outside selected mode")
        require(not any(covered[offset:offset + length]), "overlapping profile fields")
        value, mask = 0, 0
        fields = list(item) if len(item) else [item]
        for field in fields:
            shift, width = int(field.attrib["offset"], 0), int(field.attrib["bits"], 0)
            scalar = int(field.attrib["val"], 0)
            require(0 <= shift < bits and 0 < width <= bits - shift, "invalid bit range")
            field_mask = ((1 << width) - 1) << shift
            require(not mask & field_mask, "overlapping bit fields")
            require(0 <= scalar < 1 << width, "field value does not fit")
            mask |= field_mask
            value |= scalar << shift
        # Two factory controls omit reserved bits 17:18; the HAL zeroes them.
        holes = {"audio_pt_control_0": 0x60000, "audio_vt_control_0": 0x60000}
        require(mask == ((1 << bits) - 1) ^ holes.get(item.tag, 0),
                "profile has unspecified bits")
        result[offset:offset + length] = value.to_bytes(length, "little")
        covered[offset:offset + length] = b"\1" * length
    require(all(covered), "profile has unspecified bytes")
    return bytes(result), mode


def prepare_profile(data, param_id):
    profile, mode = serialize_profile(data)
    require(0 <= param_id < 256 and mode < 256, "selector index outside 8 bits")
    # Factory vbc_profile_put: offset << 24 | logical parameter ID << 16 | DSP case.
    # Case 0 is normal music playback. param_id must come from the matching HAL.
    selector = mode << 24 | param_id << 16
    return struct.pack("<8sII", b"W202VBC\0", selector, len(profile)) + profile


def prepare_acf(data):
    require(sha(data) == ACF_HASH, "unrecognized W202DS AW88166 ACF SHA256")
    return data  # Mainline ignores the same extra MULTIBIN section as the factory.


def write_new(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        require(path.read_bytes() == data, f"refusing to replace different file: {path}")
    else:
        with path.open("xb") as stream:
            stream.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audcp", type=Path, required=True)
    parser.add_argument("--acf", type=Path, required=True)
    parser.add_argument("--dsp-vbc-xml", type=Path, required=True)
    parser.add_argument("--param-id", type=lambda x: int(x, 0), required=True,
                        help="logical Music/Handsfree/Playback ID from matching factory HAL")
    parser.add_argument("--out", type=Path, required=True,
                        help="new local firmware staging directory, not /lib/firmware")
    args = parser.parse_args()
    try:
        inputs = {"audcp": args.audcp.read_bytes(), "acf": args.acf.read_bytes(),
                  "dsp_vbc_xml": args.dsp_vbc_xml.read_bytes()}
        outputs = {
            "w202ds/agdsp.bin": prepare_audcp(inputs["audcp"]),
            "aw88166_acf.bin": prepare_acf(inputs["acf"]),
            "w202ds/vbc-speaker.bin": prepare_profile(inputs["dsp_vbc_xml"], args.param_id),
        }
        manifest = {
            "schema": 1,
            "inputs": {k: {"bytes": len(v), "sha256": sha(v)} for k, v in inputs.items()},
            "outputs": {k: {"bytes": len(v), "sha256": sha(v)} for k, v in outputs.items()},
            "logical_param_id": args.param_id,
            "profile_selector": hex(struct.unpack_from("<I", outputs["w202ds/vbc-speaker.bin"], 8)[0]),
            "firmware_executed": False,
        }
        # Validate every input before creating any output.
        for name, data in outputs.items():
            write_new(args.out / name, data)
        write_new(args.out / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())
        print(json.dumps(manifest, indent=2))
    except (ValueError, KeyError, OSError, ET.ParseError) as error:
        parser.exit(1, f"firmware preparation failed: {error}\n")


if __name__ == "__main__":
    main()
