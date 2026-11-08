#!/usr/bin/env python3
"""Build one external .ko, preserving the kernel baseline and provider identity.

The parent Makefile resolves provider order. This script never installs or loads
anything. Its build.json is also the compatibility receipt for leaf builds.
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from kernel_state import describe_output, model_module, verify_output

def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inventory(directory):
    return {str(p.relative_to(directory)): sha256(p)
            for p in sorted(directory.rglob('*')) if p.is_file()}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def checked_path(value, label):
    require(value, f'{label} is required')
    result = Path(value).resolve()
    require(not re.search(r'\s', str(result)), f'{label}: Kbuild paths cannot contain whitespace')
    return result


def baseline(kdir):
    return {name: sha256(kdir / name)
            for name in ('.config', 'Module.symvers', 'include/config/kernel.release')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('id', 'devices', 'source', 'target', 'kdir', 'out'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--arch', default='arm64', choices=['arm64'])
    parser.add_argument('--cross-compile', default='aarch64-linux-gnu-')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--warnings', choices=['0', '1', '2', '3'], default='1')
    parser.add_argument('--provider', action='append', default=[])
    parser.add_argument('--kernel-provider', action='append', default=[])
    parser.add_argument('--external-source-root')
    parser.add_argument('--make-var', action='append', default=[])
    parser.add_argument('--skip-kernel-check', action='store_true',
                        help='explicitly reuse the recorded old kernel without Git/config freshness checks')
    parser.add_argument('--model', type=Path)
    parser.add_argument('--profile')
    parser.add_argument('--fragment', type=Path, action='append', default=[])
    args = parser.parse_args()
    require(re.fullmatch(r'[a-z0-9][a-z0-9-]*', args.id), 'invalid module ID')
    require(args.jobs > 0, 'jobs must be positive')
    devices = checked_path(args.devices, 'devices')
    source = checked_path(args.source, 'source')
    kdir = checked_path(args.kdir, 'KDIR')
    out = checked_path(args.out, 'OUT')
    output = describe_output(kdir)
    kernel_lock = (output['root'] / '.kernel-build.lock').open('a')
    try:
        fcntl.flock(kernel_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
    except BlockingIOError as error:
        raise ValueError('Kernel output is currently being built') from error
    plan = None
    if args.model and not args.skip_kernel_check:
        plan = model_module().compose(args.model, args.profile, args.fragment)
    verified = verify_output(kdir, check_freshness=not args.skip_kernel_check, plan=plan)
    kdir = verified['kdir']
    if args.external_source_root:
        require(source == checked_path(args.external_source_root, 'external source root'),
                'external source must match the explicitly supplied project root')
    else:
        require(source.is_relative_to(devices) and '/source/' in str(source) + '/',
                'source must be a hardware source directory')
    require(not out.is_relative_to(devices)
            and (not out.is_relative_to(kdir) or out == verified['root'] / 'modules')
            and not kdir.is_relative_to(out) and not out.is_relative_to(source)
            and not source.is_relative_to(out),
            'OUT must be separate from Devices, source and the kernel')
    require((source / 'Kbuild').is_file() or (source / 'Makefile').is_file(),
            'source lacks Kbuild/Makefile')
    target = Path(args.target)
    require(not target.is_absolute() and '..' not in target.parts
            and re.fullmatch(r'[A-Za-z0-9_./-]+\.ko', args.target), 'expected one relative .ko target')
    reserved = {'M', 'MO', 'O', 'ARCH', 'CROSS_COMPILE', 'KBUILD_EXTRA_SYMBOLS',
                'KBUILD_MODPOST_WARN', 'KBUILD_MODPOST_NOFINAL', 'KBUILD_OUTPUT',
                'KBUILD_EXTMOD', 'KBUILD_EXTMOD_OUTPUT', 'LOCALVERSION', 'KCONFIG_CONFIG'}
    for variable in args.make_var:
        key, separator, _ = variable.partition('=')
        require(separator and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', key)
                and key not in reserved, f'unsupported make variable: {key}')
    shared_inputs = {}
    for variable in args.make_var:
        key, _, value = variable.partition('=')
        if key == 'WCN_BSP_DIR':
            header_dir = checked_path(value, key) / 'kinclude'
            require((header_dir / 'mu300_compat.h').is_file(), 'shared WCN headers missing')
            shared_inputs[str(header_dir)] = inventory(header_dir)
    kernel_before = baseline(kdir)
    kernel_release = (kdir / 'include/config/kernel.release').read_text().strip()
    config = (kdir / '.config').read_text().splitlines()
    require('CONFIG_MODULES=y' in config, 'kernel does not enable modules')
    require('CONFIG_KASAN=y' not in config, 'KASAN is outside this project build policy')
    providers = []
    symbols = []
    for value in args.provider:
        receipt_path = checked_path(value, 'provider receipt')
        receipt = json.loads(receipt_path.read_text())
        require(receipt.get('ok') is True, f'provider failed: {receipt_path}')
        require(receipt['kernel_baseline'] == kernel_before, f'provider kernel mismatch: {receipt_path}')
        for kind in ('module', 'symbols'):
            record = receipt[kind]
            path = receipt_path.parent / record['path']
            require(sha256(path) == record['sha256'], f'provider {kind} changed: {path}')
            if kind == 'symbols' and path.stat().st_size:
                symbols.append(str(path))
        providers.append({'id': receipt['id'], 'receipt': str(receipt_path),
                          'sha256': sha256(receipt_path)})
    kernel_providers = []
    for value in args.kernel_provider:
        provider_path = checked_path(value, 'kernel provider receipt')
        provider = json.loads(provider_path.read_text())
        require(provider['kernel_release'] == kernel_release
                and all(kernel_before.get(k) == v for k, v in provider['kernel_baseline'].items()),
                f'kernel provider baseline mismatch: {provider_path}')
        record = provider['module']
        relative = Path(record['path'])
        require(not relative.is_absolute() and '..' not in relative.parts,
                'invalid kernel provider module path')
        require(sha256(kdir / relative) == record['sha256'],
                f'kernel provider module changed: {kdir / relative}')
        record = provider['symbols']
        relative = Path(record['path'])
        require(not relative.is_absolute() and '..' not in relative.parts,
                'invalid kernel provider symbol path')
        symbol_file = provider_path.parent / relative
        require(sha256(symbol_file) == record['sha256'],
                f'kernel provider symbols changed: {symbol_file}')
        require(symbol_file.stat().st_size > 0, 'kernel provider export list is empty')
        symbols.append(str(symbol_file))
        kernel_providers.append({'receipt': str(provider_path), 'sha256': sha256(provider_path),
                                 'module': provider['module'], 'symbols': provider['symbols']})
    source_before = inventory(source)
    build = out / args.id
    build.mkdir(parents=True, exist_ok=True)
    receipt_path = build / 'build.json'
    # Existing outputs are reusable only for exactly the same module and kernel.
    if receipt_path.exists():
        previous = json.loads(receipt_path.read_text())
        require(previous['id'] == args.id and previous['source'] == str(source)
                and previous['target'] == args.target
                and previous['kernel_baseline'] == kernel_before,
                'output belongs to a different source/target/kernel; use a new OUT')
    else:
        require(not any(build.iterdir()), 'output has no receipt; inspect it or use a new OUT')
    command = ['make', '-C', str(kdir), f'M={source}', f'MO={build}',
               f'ARCH={args.arch}', f'CROSS_COMPILE={args.cross_compile}',
               f'W={args.warnings}', f'-j{args.jobs}',
               'LOCALVERSION=', 'KCONFIG_CONFIG=.config',
               'KBUILD_EXTRA_SYMBOLS=' + ' '.join(symbols),
               *args.make_var, args.target]
    receipt = {'schema': 1, 'id': args.id, 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'source': str(source), 'target': args.target, 'command': command,
               'kernel_release': kernel_release, 'kernel_baseline': kernel_before,
               'kernel_output': str(verified['root']),
               'kernel_metadata_sha256': sha256(verified['metadata']),
               'kernel_freshness_checked': verified['freshness_checked'],
               'kernel_integrity_unchanged': False,
               'providers': providers, 'source_files': source_before,
               'kernel_providers': kernel_providers, 'shared_inputs': shared_inputs, 'ok': False,
               'verification_scope': 'build-only; no installation or hardware operation'}
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    try:
        print(f'[{args.id}] {args.target}', flush=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith('KCONFIG_')}
        # Parent GNU make owns its jobserver; the nested build has its own -j.
        for name in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'MAKEOVERRIDES', 'KBUILD_EXTRA_SYMBOLS',
                     'KBUILD_OUTPUT', 'KBUILD_EXTMOD', 'KBUILD_EXTMOD_OUTPUT',
                     'KBUILD_MODPOST_WARN', 'KBUILD_MODPOST_NOFINAL', 'KBUILD_SRC', 'LOCALVERSION'):
            env.pop(name, None)
        env['LOCALVERSION'] = ''
        with (build / 'build.log').open('w') as log:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, env=env)
            for line in process.stdout:
                log.write(line)
                print(line, end='', flush=True)
            require(process.wait() == 0, f'Kbuild failed; inspect {build}/build.log')
        module = build / target
        require(sorted(build.rglob('*.ko')) == [module], 'output must contain exactly the requested .ko')
        with module.open('rb') as stream:
            header = stream.read(20)
        require(header[:6] == b'\x7fELF\x02\x01' and int.from_bytes(header[18:20], 'little') == 183,
                'module is not little-endian ELF64 AArch64')
        vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(module)], text=True).strip()
        require(vermagic.split()[0] == kernel_release, 'module vermagic does not match KDIR')
        receipt['vermagic'] = vermagic
        receipt['depends'] = subprocess.check_output(['modinfo', '-F', 'depends', str(module)], text=True).strip()
        receipt['module'] = {'path': args.target, 'sha256': sha256(module), 'bytes': module.stat().st_size}
        symvers = build / 'Module.symvers'
        receipt['symbols'] = {'path': 'Module.symvers', 'sha256': sha256(symvers)}
        require(kernel_before == baseline(kdir), 'kernel configuration/symbol baseline changed')
        require(sha256(verified['metadata']) == receipt['kernel_metadata_sha256'],
                'kernel integrity metadata changed during build')
        after_plan = model_module().compose(args.model, args.profile, args.fragment) if plan else None
        verify_output(kdir, check_freshness=not args.skip_kernel_check, plan=after_plan)
        receipt['kernel_integrity_unchanged'] = True
        require(source_before == inventory(source), 'Kbuild modified the source directory')
        require(all(inventory(Path(p)) == files for p, files in shared_inputs.items()),
                'shared headers changed during build')
        receipt['ok'] = True
    except BaseException as exc:
        receipt['error'] = str(exc)
        raise
    finally:
        receipt['kernel_unchanged'] = kernel_before == baseline(kdir)
        receipt['source_unchanged'] = source_before == inventory(source)
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'[{args.id}] OK {module}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
