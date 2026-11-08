#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""Expose private NV images as loop devices for the original CP disk server."""

import argparse
import array
import grp
import json
import os
from pathlib import Path
import pwd
import stat
import struct
import subprocess
import sys


NV = {
    'nr_fixnv1_a': (0xEC0000, 52428800),
    'nr_fixnv2_a': (0xEC0000, 52428800),
    'nr_runtimenv1': (0xF40000, 57671680),
    'nr_runtimenv2': (0xF40000, 57671680),
}


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def validate_nv(path, payload_size, capacity):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size != capacity:
        raise ValueError(f'{path.name}: NV image must be a full {capacity}-byte regular file')
    with path.open('rb') as stream:
        header = stream.read(512)
        payload = stream.read(payload_size)
    magic, length, checksum, version = struct.unpack_from('<4I', header)
    if (magic, length, version) != (0x4E56, payload_size, 0x65):
        raise ValueError(f'{path.name}: unexpected NV header')
    words = array.array('H', payload)
    if sys.byteorder != 'little':
        words.byteswap()
    total = sum(words)

    def folded(value):
        value = (value >> 16) + (value & 0xFFFF)
        return (~(value + (value >> 16))) & 0xFFFF

    # Original cp_diskserver accepts both its 64-bit and legacy 32-bit sum.
    if checksum & 0xFFFF not in (folded(total), folded(total & 0xFFFFFFFF)):
        raise ValueError(f'{path.name}: NV checksum mismatch')


def loop_info(image):
    result = json.loads(run('losetup', '--associated', str(image), '--json',
                            '--output', 'NAME,BACK-FILE,OFFSET,SIZELIMIT,RO'))
    loops = result['loopdevices']
    if len(loops) > 1:
        raise ValueError(f'{image.name}: multiple loop mappings')
    if not loops:
        return None
    info = loops[0]
    if (Path(info['back-file']).resolve() != image.resolve()
            or int(info['offset']) != 0 or int(info['sizelimit']) != 0
            or info['ro']):
        raise ValueError(f'{image.name}: incompatible loop mapping')
    node = Path(info['name'])
    device = node.stat()
    if not stat.S_ISBLK(device.st_mode) or os.major(device.st_rdev) != 7:
        raise ValueError(f'{image.name}: expected a Linux loop device')
    return node


def verify_node(node, loop):
    info = node.lstat()
    if not stat.S_ISBLK(info.st_mode) or info.st_rdev != loop.stat().st_rdev:
        raise ValueError(f'{node.name}: private NV node does not map its image')


def state_paths(value):
    state = Path(value)
    if (not state.is_absolute() or state.resolve() != state
            or not str(state).startswith('/var/lib/w202ds/')):
        raise ValueError('Expected a canonical private state directory below /var/lib/w202ds')
    for path in (state, state / 'android'):
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError(f'Unexpected private directory ownership or permissions: {path}')
    by_name = state / 'android/dev/block/by-name'
    if by_name.resolve() != by_name or not by_name.is_dir():
        raise ValueError('Unexpected private by-name directory')
    images = state / 'nv'
    if images.is_symlink():
        raise ValueError('NV image directory must not be a symlink')
    return state, by_name, images


def verify_private_blocks(root, by_name, loops):
    # Never expose the physical eMMC or an unrelated loop device to the daemon.
    for directory, dirs, files in os.walk(root / 'dev', followlinks=False):
        for name in files:
            node = Path(directory) / name
            if stat.S_ISBLK(node.lstat().st_mode):
                if node.parent != by_name or name not in loops:
                    raise ValueError(f'Unexpected block device in private root: {node}')
                verify_node(node, loops[name])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('prepare', 'verify', 'release'))
    parser.add_argument('--state', required=True)
    parser.add_argument('--check-nv', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise ValueError('Root is required')
    state, by_name, images = state_paths(args.state)
    if args.operation == 'release':
        for name in NV:
            image = images / (name + '.img')
            if image.is_file():
                loop = loop_info(image)
                if loop is not None:
                    run('losetup', '--detach', str(loop))
        return
    if args.operation == 'prepare':
        images.mkdir(mode=0o700, exist_ok=True)
        os.chown(images, 0, 0)
        os.chmod(images, 0o700)
    loops = {}
    uid = pwd.getpwnam('w202modem').pw_uid
    gid = grp.getgrnam('w202modem').gr_gid
    for name, (payload_size, capacity) in NV.items():
        image = images / (name + '.img')
        node = by_name / name
        if args.operation == 'prepare' and not image.exists():
            # One-time migration of an intact legacy regular-file snapshot.
            validate_nv(node, payload_size, capacity)
            node.rename(image)
        if args.operation == 'prepare' or args.check_nv:
            validate_nv(image, payload_size, capacity)
        info = image.lstat()
        if (not stat.S_ISREG(info.st_mode) or info.st_size != capacity
                or info.st_nlink != 1):
            raise ValueError(f'{name}: invalid private image')
        if args.operation == 'prepare':
            os.chown(image, 0, 0)
            os.chmod(image, 0o600)
        elif info.st_uid != 0 or info.st_mode & 0o077:
            raise ValueError(f'{name}: unexpected image permissions')
        loop = loop_info(image)
        if loop is None and args.operation == 'prepare':
            run('losetup', '--find', '--show', '--nooverlap', str(image))
            loop = loop_info(image)
        if loop is None:
            raise ValueError(f'{name}: loop mapping missing')
        if args.operation == 'prepare':
            if node.exists():
                info = node.lstat()
                if not stat.S_ISBLK(info.st_mode) or os.major(info.st_rdev) != 7:
                    raise ValueError(f'{name}: refusing to replace an existing non-loop node')
                if info.st_rdev != loop.stat().st_rdev:
                    node.unlink()
            if not node.exists():
                os.mknod(node, stat.S_IFBLK | 0o600, loop.stat().st_rdev)
            os.chown(node, uid, gid)
            os.chmod(node, 0o600)
        verify_node(node, loop)
        loops[name] = loop
    verify_private_blocks(state / 'android', by_name, loops)
    print('Private NV loop mappings verified; physical eMMC is not exposed.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'w202-modem-storage: {error}', file=sys.stderr)
        sys.exit(1)
