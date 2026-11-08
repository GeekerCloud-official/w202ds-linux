#!/usr/bin/env python3
"""Import an exported Image/module build output and bind matching local source."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import subprocess
import sys
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_state import source_identity
REQUIRED = {'Image', 'build/Makefile', 'build/.config', 'build/Module.symvers',
            'build/include/config/kernel.release', 'build/include/config/auto.conf',
            'build/include/generated/autoconf.h', 'build/include/generated/rustc_cfg',
            'build/include/generated/utsrelease.h', 'build/scripts/basic/fixdep',
            'build/scripts/mod/modpost', 'build/scripts/module.lds'}


def safe_name(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or str(path) != name or '\\' in name:
        raise ValueError(f'Unsafe archive path: {name}')
    if name not in ('manifest.json', 'Image') and not name.startswith('build/'):
        raise ValueError(f'Unexpected archive path: {name}')
    if name == 'build/source' or name.startswith('build/source/'):
        raise ValueError('The source binding must be created locally')
    return path


def import_output(archive, out, kernel):
    archive, out, kernel = (path.resolve() for path in (archive, out, kernel))
    if out.exists():
        raise ValueError(f'Output already exists; choose a new directory: {out}')
    if any(character.isspace() for character in str(out) + str(kernel)):
        raise ValueError('Kbuild paths cannot contain whitespace')
    if out.is_relative_to(kernel) or kernel.is_relative_to(out):
        raise ValueError('Output and kernel source must be separate directories')
    if not (kernel / 'Makefile').is_file():
        raise ValueError(f'Kernel source not found: {kernel}')
    if Path(subprocess.check_output(['git', '-C', str(kernel), 'rev-parse', '--show-toplevel'], text=True).strip()).resolve() != kernel:
        raise ValueError('The kernel source must be its own Git checkout')
    with tarfile.open(archive, 'r:gz') as stream:
        members = {}
        for member in stream.getmembers():
            safe_name(member.name)
            if not member.isfile() or member.name in members:
                raise ValueError(f'Archive must contain unique regular files: {member.name}')
            members[member.name] = member
        if 'manifest.json' not in members or members['manifest.json'].size > 16 * 1024 * 1024:
            raise ValueError('Archive manifest missing or too large')
        manifest = json.load(stream.extractfile(members['manifest.json']))
        if not isinstance(manifest, dict) or manifest.get('schema') != 1 or manifest.get('kind') != 'w202ds-kernel-output' or manifest.get('arch') != 'arm64':
            raise ValueError('Unsupported kernel archive format')
        files = manifest.get('files')
        if not isinstance(files, dict) or not REQUIRED.issubset(files) or set(members) != {'manifest.json', *files}:
            raise ValueError('Archive contents differ from manifest or required build files are missing')
        for name, record in files.items():
            safe_name(name)
            if (not isinstance(record, dict) or type(record.get('bytes')) is not int or record['bytes'] < 0
                    or record['bytes'] != members[name].size
                    or not re.fullmatch('[0-9a-f]{64}', record.get('sha256', ''))
                    or type(record.get('mode')) is not int or not 0 <= record['mode'] <= 0o777):
                raise ValueError(f'Invalid file metadata: {name}')
        if manifest.get('host_system') != platform.system() or manifest.get('host_machine') != platform.machine():
            raise ValueError('Kbuild host tools require the same host OS and CPU architecture as the exporter')
        commit = subprocess.check_output(['git', '-C', str(kernel), 'rev-parse', 'HEAD'], text=True).strip()
        if commit != manifest.get('kernel_commit'):
            raise ValueError('Kernel source commit does not match archive: ' + str(manifest.get('kernel_commit')))
        if subprocess.check_output(['git', '-C', str(kernel), 'status', '--porcelain=v1'], text=True).strip():
            raise ValueError('Kernel source must be clean and match the exported build')
        if 'source_identity' in manifest and source_identity(kernel) != manifest['source_identity']:
            raise ValueError('Kernel-linked Devices sources do not match the exported build')
        out.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.' + out.name + '-', dir=out.parent) as temporary:
            staging = Path(temporary)
            for name, member in members.items():
                destination = staging / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                digest = hashlib.sha256()
                with stream.extractfile(member) as source, destination.open('xb') as target:
                    while chunk := source.read(1024 * 1024):
                        target.write(chunk)
                        digest.update(chunk)
                if name in files:
                    if digest.hexdigest() != files[name]['sha256']:
                        raise ValueError(f'Archive file checksum mismatch: {name}')
                    destination.chmod(files[name]['mode'])
            for name, key in [('build/.config', 'config_sha256'), ('build/Module.symvers', 'symvers_sha256')]:
                if files[name]['sha256'] != manifest.get(key):
                    raise ValueError(f'Kernel baseline differs from manifest: {name}')
            release = (staging / 'build/include/config/kernel.release').read_text().strip()
            if release != manifest.get('kernelrelease'):
                raise ValueError('Kernel release differs from manifest')
            (staging / 'build/source').symlink_to(os.path.relpath(kernel, out / 'build'), target_is_directory=True)
            binding = {'kernel': str(kernel), 'kernel_commit': commit, 'kernelrelease': release,
                       'kdir': str(out / 'build'), 'image': str(out / 'Image'),
                       'modules': str(out / 'modules'),
                       'config_sha256': manifest['config_sha256'], 'compiler': manifest.get('compiler')}
            (staging / 'binding.json').write_text(json.dumps(binding, indent=2) + '\n')
            if out.exists():
                raise ValueError(f'Output appeared during import: {out}')
            staging.rename(out)
    return binding


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path, help='archive created by kernel-export.py')
    parser.add_argument('--out', required=True, type=Path, help='new local output directory to create')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'kernel', help='matching clean local kernel source')
    args = parser.parse_args()
    try:
        print(json.dumps(import_output(args.archive, args.out, args.kernel), indent=2))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f'kernel-import: {error}\n')


if __name__ == '__main__':
    main()
