"""Identity and integrity checks shared by kernel and external-module builds."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def model_module():
    spec = importlib.util.spec_from_file_location('w202ds_kernel_model', ROOT / 'scripts/kernel-build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def input_path(path):
    path = Path(path).resolve()
    return str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)


def configuration(plan):
    """Keep configuration provenance portable between matching checkouts."""
    return {'profile': plan['profile'], 'arch': plan['arch'], 'layers': plan['layers'],
            'model': {'path': input_path(plan['model']['path']), 'sha256': plan['model']['sha256']},
            'inputs': [dict(item, path=input_path(item['path'])) for item in plan['inputs']]}


def configuration_signature(config):
    # Absolute locations may differ after importing on another machine.
    return (config['profile'], config['arch'], config['layers'], config['model']['sha256'],
            [(item['layer'], item['sha256']) for item in config['inputs']])


def current_configuration(saved):
    model = ROOT / saved['model']['path']
    fragments = [ROOT / item['path'] for item in saved['inputs']
                 if item['layer'].startswith('external-')]
    return configuration(model_module().compose(model, saved['profile'], fragments))


def source_identity(kernel):
    kernel = Path(kernel).resolve()
    def git(*args):
        return subprocess.check_output(['git', '-C', str(kernel), *args], stderr=subprocess.PIPE)
    if Path(os.fsdecode(git('rev-parse', '--show-toplevel')).strip()).resolve() != kernel:
        raise ValueError('Kernel source must be its own Git checkout')
    commit = os.fsdecode(git('rev-parse', 'HEAD')).strip()
    if git('status', '--porcelain=v1', '--untracked-files=all').strip():
        raise ValueError('Kernel source is dirty; commit/revert its changes and use a new kernel output')
    links = {}
    # Git records the symlink itself, not the Devices/ implementation it points to.
    for record in git('ls-files', '--stage', '-z').split(b'\0'):
        if not record.startswith(b'120000 '):
            continue
        name = os.fsdecode(record.partition(b'\t')[2])
        link = kernel / name
        target = link.resolve(strict=True)
        if target.is_relative_to(kernel):
            continue
        paths = sorted(target.rglob('*')) if target.is_dir() else [target]
        files = {str(p.relative_to(target)) if target.is_dir() else '.': sha256(p)
                 for p in paths if p.is_file()}
        links[name] = {'target': os.readlink(link), 'files': files}
    return {'commit': commit, 'external_links': links}


def support_files(build):
    """The immutable files needed to compile ordinary arm64 C modules."""
    names = {'Makefile', '.config', 'Module.symvers', 'arch/arm64/boot/Image',
             'include/config/kernel.release', 'include/config/auto.conf',
             'include/generated/autoconf.h', 'include/generated/rustc_cfg',
             'include/generated/utsrelease.h', 'scripts/basic/fixdep',
             'scripts/mod/modpost', 'scripts/module.lds'}
    for relative in ('include', 'arch/arm64/include/generated'):
        directory = build / relative
        if not directory.is_dir():
            raise ValueError(f'Generated headers missing: {directory}')
        for path in directory.rglob('*'):
            if path.is_symlink():
                raise ValueError(f'Unexpected generated header symlink: {path}')
            if path.is_file() and not path.name.startswith('.') and path.suffix not in ('.cmd', '.d', '.o', '.a'):
                names.add(str(path.relative_to(build)))
    config = set((build / '.config').read_text().splitlines())
    if 'CONFIG_MODVERSIONS=y' in config:
        names.add('scripts/gendwarfksyms/gendwarfksyms' if 'CONFIG_GENDWARFKSYMS=y' in config
                  else 'scripts/genksyms/genksyms')
    if 'CONFIG_FTRACE_MCOUNT_RECORD=y' in config and 'CONFIG_HAVE_C_RECORDMCOUNT=y' in config:
        names.add('scripts/recordmcount')
    if 'CONFIG_GCC_PLUGINS=y' in config:
        plugins = list((build / 'scripts/gcc-plugins').glob('*.so'))
        if not plugins:
            raise ValueError('GCC plugin binaries missing')
        names.update(str(p.relative_to(build)) for p in plugins)
    return sorted(names)


def file_records(build):
    result = {}
    for name in support_files(build):
        path = build / name
        if not path.is_file() or path.is_symlink():
            raise ValueError(f'Required regular build file missing: {path}')
        result[name] = {'sha256': sha256(path), 'bytes': path.stat().st_size,
                        'mode': path.stat().st_mode & 0o777}
    return result


def describe_output(path):
    path = Path(path).resolve()
    if (path / 'kernel-state.json').is_file():
        root, kdir, metadata = path, path, path / 'kernel-state.json'
        kind = 'built'
    elif (path / 'manifest.json').is_file():
        root, kdir, metadata = path, path / 'build', path / 'manifest.json'
        kind = 'imported'
    elif path.name == 'build' and (path.parent / 'manifest.json').is_file():
        return describe_output(path.parent)
    else:
        raise ValueError(f'No verified kernel output in {path}; build/import a kernel into a new directory')
    return {'root': root, 'kdir': kdir, 'metadata': metadata, 'kind': kind}


def verify_output(path, *, check_freshness=True, plan=None):
    output = describe_output(path)
    root, kdir = output['root'], output['kdir']
    state = json.loads(output['metadata'].read_text())
    if state.get('schema') != 1 or state.get('kind') != 'w202ds-kernel-output' or state.get('arch') != 'arm64':
        raise ValueError('Unsupported kernel output metadata')
    if state.get('host_system') != platform.system() or state.get('host_machine') != platform.machine():
        raise ValueError('Kernel host tools do not match this host OS/CPU')
    files = state['files']
    prefix = 'build/' if output['kind'] == 'imported' else ''
    image_name = 'Image' if prefix else 'arch/arm64/boot/Image'
    required = {prefix + n for n in ('Makefile', '.config', 'Module.symvers',
                'include/config/kernel.release', 'include/config/auto.conf',
                'include/generated/autoconf.h', 'scripts/basic/fixdep', 'scripts/mod/modpost', 'scripts/module.lds')}
    if not required.union({image_name}).issubset(files):
        raise ValueError('Kernel output is missing required integrity records')
    for name, record in files.items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError(f'Unsafe kernel output record: {name}')
        file = root / relative
        if (not file.is_file() or file.is_symlink() or not file.resolve().is_relative_to(root)
                or file.stat().st_size != record['bytes'] or sha256(file) != record['sha256']
                or file.stat().st_mode & 0o777 != record['mode']):
            raise ValueError(f'Kernel output file changed or missing: {name}; use a new kernel output')
    # New generated headers must not silently augment the recorded baseline either.
    for directory in ('include', 'arch/arm64/include/generated'):
        for file in (kdir / directory).rglob('*'):
            if file.is_symlink():
                raise ValueError(f'Unexpected generated kernel symlink: {file}')
            if (file.is_file() and not file.name.startswith('.')
                    and file.suffix not in ('.cmd', '.d', '.o', '.a')
                    and prefix + str(file.relative_to(kdir)) not in files):
                raise ValueError(f'Unrecorded generated kernel file: {file}')
    release = (kdir / 'include/config/kernel.release').read_text().strip()
    if release != state['kernelrelease']:
        raise ValueError('Kernel release differs from recorded build')
    for name, key in (('.config', 'config_sha256'), ('Module.symvers', 'symvers_sha256')):
        if files[prefix + name]['sha256'] != state[key]:
            raise ValueError(f'Kernel {name} differs from recorded identity')
    kernel = (kdir / 'source').resolve(strict=True)
    if output['kind'] == 'imported':
        binding = json.loads((root / 'binding.json').read_text())
        if (Path(binding['kernel']).resolve() != kernel or binding['kernel_commit'] != state['kernel_commit']
                or binding['kernelrelease'] != release or binding['config_sha256'] != state['config_sha256']):
            raise ValueError('Imported source binding differs from the kernel manifest')
    if check_freshness:
        if '-dirty' in release:
            raise ValueError('Kernel release is dirty; use a clean kernel build')
        identity = source_identity(kernel)
        if identity['commit'] != state['kernel_commit']:
            raise ValueError('Kernel source Git commit changed; use a new kernel output')
        if 'source_identity' not in state or 'configuration' not in state:
            raise ValueError('Old kernel output lacks source/configuration provenance; rebuild it, or explicitly use --skip-kernel-check')
        if identity != state['source_identity']:
            raise ValueError('Kernel-linked Devices source changed; use a new kernel output')
        current = configuration(plan) if plan is not None else current_configuration(state['configuration'])
        if configuration_signature(current) != configuration_signature(state['configuration']):
            raise ValueError('Kernel configuration inputs/profile changed; use a new kernel output')
    return dict(output, state=state, kernel=kernel, kernelrelease=release,
                freshness_checked=check_freshness)
