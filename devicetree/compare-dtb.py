#!/usr/bin/env python3
"""Compare DTB hardware semantics, ignoring layout, padding and sibling order."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def read_tree(path):
    data = Path(path).read_bytes()
    header = struct.unpack_from('>10I', data)
    magic, size, tree, strings, reserve, version, _, bootcpu, stringsz, treesz = header
    if magic != 0xd00dfeed or version < 17 or size > len(data):
        raise ValueError(f'invalid FDT header: {path}')
    if strings + stringsz > size or tree + treesz > size:
        raise ValueError('FDT block exceeds size')
    reservations = []
    while True:
        address, length = struct.unpack_from('>QQ', data, reserve)
        reserve += 16
        if not address and not length:
            break
        reservations.append((address, length))
    nodes, stack, end = {}, [], tree + treesz
    while tree < end:
        token, = struct.unpack_from('>I', data, tree)
        tree += 4
        if token == 1:
            stop = data.index(b'\0', tree, end)
            stack.append(data[tree:stop].decode())
            key = '/' + '/'.join(stack[1:])
            if key in nodes:
                raise ValueError(f'duplicate node {key}')
            nodes[key] = {}
            tree = (stop + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, nameoff = struct.unpack_from('>II', data, tree)
            tree += 8
            if nameoff >= stringsz or tree + length > end:
                raise ValueError('invalid FDT property bounds')
            name = data[strings + nameoff:data.index(b'\0', strings + nameoff,
                                                   strings + stringsz)].decode()
            props = nodes['/' + '/'.join(stack[1:])]
            if name in props:
                raise ValueError(f'duplicate property {name}')
            props[name] = data[tree:tree + length].hex()
            tree = (tree + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9 and not stack:
            return {'nodes': nodes, 'reservations': sorted(reservations), 'bootcpu': bootcpu}
        else:
            raise ValueError(f'invalid FDT structure token {token}')
    raise ValueError('missing FDT end')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline')
    parser.add_argument('candidate')
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    before, after = read_tree(args.baseline), read_tree(args.candidate)
    differences = []
    for node in sorted(before['nodes'].keys() | after['nodes'].keys()):
        a, b = before['nodes'].get(node), after['nodes'].get(node)
        if a is None or b is None:
            differences.append({'node': node, 'before': a, 'after': b})
        else:
            for prop in sorted(a.keys() | b.keys()):
                if a.get(prop) != b.get(prop):
                    differences.append({'node': node, 'property': prop,
                                        'before': a.get(prop), 'after': b.get(prop)})
    for name in ('reservations', 'bootcpu'):
        if before[name] != after[name]:
            differences.append({'field': name, 'before': before[name], 'after': after[name]})
    report = {'ok': not differences, 'node_count': len(after['nodes']),
              'property_count': sum(map(len, after['nodes'].values())),
              'sha256': {k: hashlib.sha256(Path(v).read_bytes()).hexdigest()
                         for k, v in [('baseline', args.baseline), ('candidate', args.candidate)]},
              'differences': differences}
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'differences'}))
    raise SystemExit(0 if report['ok'] else 1)


if __name__ == '__main__':
    main()
