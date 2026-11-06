#!/usr/bin/env python3
"""Create an immutable initial Bionic property snapshot from explicit JSON.

Uses the Android 13 legacy single-area reader format, not a captured Android
property database. Values and their origin belong in the caller's manifest.
Long property values and run-time additions are deliberately unsupported.
"""
import argparse
import json
from pathlib import Path
import re
import struct

AREA_SIZE = 128 * 1024
HEADER_SIZE = 128
VALUE_MAX = 92


def build_properties(properties):
    if not isinstance(properties, dict) or not properties:
        raise ValueError('a nonempty JSON object is required')
    root = {'children': {}, 'value': None, 'key': ''}
    for key, value in sorted(properties.items()):
        if not isinstance(key, str) or not re.fullmatch(r'[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*', key):
            raise ValueError(f'invalid property key: {key!r}')
        if len(key.encode('ascii')) > 255 or key.startswith('ctl.'):
            raise ValueError(f'unsupported property key: {key!r}')
        if not isinstance(value, str) or '\0' in value or len(value.encode('utf-8')) >= VALUE_MAX:
            raise ValueError(f'unsupported property value for {key}')
        node = root
        for token in key.split('.'):
            node = node['children'].setdefault(token, {'children': {}, 'value': None, 'key': key})
        node['key'], node['value'] = key, value

    # prop_bt root (20 bytes), followed by Bionic's dirty-value backup (92).
    data = bytearray(20 + VALUE_MAX)
    offsets = {}

    def allocate(size):
        offset = len(data)
        data.extend(bytes((size + 3) & ~3))
        if len(data) > AREA_SIZE - HEADER_SIZE:
            raise ValueError('property snapshot does not fit in one area')
        return offset

    def siblings(children):
        items = sorted(children.items(), key=lambda kv: (len(kv[0]), kv[0]))

        def tree(items):
            if not items:
                return 0
            mid = len(items) // 2
            token, node = items[mid]
            token = token.encode('ascii')
            pos = allocate(20 + len(token) + 1)
            data[pos + 20:pos + 20 + len(token)] = token
            left, right = tree(items[:mid]), tree(items[mid + 1:])
            prop = 0
            if node['value'] is not None:
                key, value = node['key'].encode('ascii'), node['value'].encode('utf-8')
                prop = allocate(96 + len(key) + 1)
                struct.pack_into('<I', data, prop, len(value) << 24)
                data[prop + 4:prop + 4 + len(value)] = value
                data[prop + 96:prop + 96 + len(key)] = key
                offsets[node['key']] = HEADER_SIZE + prop
            child = siblings(node['children'])
            struct.pack_into('<5I', data, pos, len(token), prop, left, right, child)
            return pos

        return tree(items)

    struct.pack_into('<I', data, 16, siblings(root['children']))
    header = bytearray(HEADER_SIZE)
    struct.pack_into('<4I', header, 0, len(data), 0, 0x504f5250, 0xfc6ed0ab)
    area = header + data
    area.extend(bytes(AREA_SIZE - len(area)))
    return bytes(area), {'format': 'bionic-single-area-android13', 'bytes': AREA_SIZE,
                         'data_bytes_used': len(data), 'properties': dict(sorted(offsets.items()))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('values', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    area, manifest = build_properties(json.loads(args.values.read_text()))
    with args.output.open('xb') as output:
        output.write(area)
    with args.output.with_suffix(args.output.suffix + '.layout.json').open('x') as output:
        json.dump(manifest, output, indent=2)
        output.write('\n')


if __name__ == '__main__':
    main()
