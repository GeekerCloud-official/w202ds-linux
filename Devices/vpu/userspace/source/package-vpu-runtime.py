#!/usr/bin/env python3
"""Package the factory AVC dependency closure in a separate research root.

Inputs are extracted stock ELF files. Never copies modem binaries, properties,
device nodes or mutable state from the running modem's Android root.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tarfile


def dynamic(path):
    data = path.read_bytes()
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[0][:6] != b'\x7fELF\x02\x01' or header[2] != 183 or header[11] != 64:
        raise ValueError(f'not a little endian AArch64 ELF: {path}')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + i * 64)
                for i in range(header[12])]
    section = next(s for s in sections if s[1] == 6)
    strings = sections[section[6]]
    table = data[strings[4]:strings[4] + strings[5]]
    tags = []
    for offset in range(section[4], section[4] + section[5], 16):
        tag, value = struct.unpack_from('<qQ', data, offset)
        if not tag:
            break
        if tag in (1, 14):
            tags.append((tag, table[value:table.index(0, value)].decode('ascii')))
    return [v for k, v in tags if k == 1], {v for k, v in tags if k == 14}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('modem_evidence', type=Path)
    parser.add_argument('avc_library', type=Path)
    parser.add_argument('client', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = args.output / 'android'
    root.mkdir(parents=True)
    old = args.modem_evidence
    search = [(old / 'bionic-inspect/lib64/bionic', Path('apex/com.android.runtime/lib64/bionic')),
              (old / 'runtime-subset/system/lib64', Path('system/lib64'))]
    linker = old / 'bionic-inspect/bin/linker64'
    _, provided = dynamic(linker)
    if provided != {'ld-android.so'}:
        raise ValueError(f'unexpected linker SONAME: {provided}')
    queue = collections.deque([(args.avc_library, Path('vendor/lib64') / args.avc_library.name),
                               (linker, Path('apex/com.android.runtime/bin/linker64')),
                               (args.client, Path('test-vendor-avc'))])
    records = {}
    while queue:
        source, target = queue.popleft()
        if str(target) in records:
            continue
        if source.is_symlink() or not source.is_file():
            raise ValueError(f'expected an original regular file: {source}')
        needed, _ = dynamic(source)
        dest = root / target
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
        dest.chmod(0o755 if target == Path('test-vendor-avc') or 'bin' in target.parts else 0o644)
        records[str(target)] = dict(source=str(source.resolve()), bytes=dest.stat().st_size,
                                   sha256=hashlib.sha256(dest.read_bytes()).hexdigest(), needed=needed)
        for name in needed:
            if name in provided:
                continue
            choice = next(((folder / name, dest / name) for folder, dest in search
                           if (folder / name).is_file()), None)
            if choice is None:
                raise ValueError(f'unresolved dependency of {target}: {name}')
            queue.append(choice)
    for folder in ('system/bin', 'proc', 'dev/dri', 'linkerconfig', 'input', 'output', 'tmp'):
        (root / folder).mkdir(parents=True, exist_ok=True)
    (root / 'system/bin/linker64').symlink_to('/apex/com.android.runtime/bin/linker64')
    (root / 'linkerconfig/ld.config.txt').write_text('')
    (args.output / 'runtime-closure.json').write_text(json.dumps(dict(
        linker_provides=sorted(provided), files=records, autoload=False,
        provenance='W202DS factory partitions extracted read-only; separate VPU validation root',
        hardware_nodes='none in package; create only reviewed VPU/render nodes before test'), indent=2) + '\n')
    with tarfile.open(args.output / 'vpu-android.tar.gz', 'x:gz') as archive:
        for path in sorted(root.rglob('*')):
            info = archive.gettarinfo(str(path), str(path.relative_to(root)))
            info.uid = info.gid = 0
            info.uname = info.gname = 'root'
            if info.isreg():
                with path.open('rb') as source:
                    archive.addfile(info, source)
            else:
                archive.addfile(info)
    print(json.dumps({'elf_files': len(records), 'elf_bytes': sum(x['bytes'] for x in records.values())}))


if __name__ == '__main__':
    main()
