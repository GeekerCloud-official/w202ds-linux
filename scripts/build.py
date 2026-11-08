#!/usr/bin/env python3
"""Build a missing kernel, then external modules; reject changed kernel baselines."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

from kernel_state import ROOT, model_module, verify_output


def prepare(args):
    explicit_output = args.kernel_out is not None
    if args.skip_kernel_check:
        args.skip_kernel = True
    if args.skip_kernel and not explicit_output:
        raise ValueError('--skip-kernel/--skip-kernel-check requires an explicit --kernel-out')
    model = model_module()
    plan = None if args.skip_kernel_check else model.compose(args.model, args.profile, args.fragment)
    profile = args.profile or (plan['profile'] if plan else 'debug')
    output = (args.kernel_out or ROOT / 'output' / ('kernel-' + profile)).resolve()
    missing = not output.exists() or not any(output.iterdir())
    report_path = output / 'kernel-build.json'
    if not missing and not (output / 'kernel-state.json').exists() and report_path.exists():
        missing = json.loads(report_path.read_text()).get('status') in ('configured', 'failed', 'building')
    if missing:
        if args.skip_kernel:
            raise ValueError(f'Cannot skip kernel build: no kernel output in {output}')
        print(f'No kernel output; building {profile} in {output}', flush=True)
        model.generate(argparse.Namespace(action='build', kernel=args.kernel, out=output,
                       cross_compile=args.cross_compile, jobs=args.jobs), plan)
    result = verify_output(output, check_freshness=not args.skip_kernel_check, plan=plan)
    if args.skip_kernel_check:
        print(f'Explicitly reusing {result["kernelrelease"]}; Git/config freshness checks skipped. '
              'Current configuration changes are not included.', file=sys.stderr)
    result['modules'] = (args.modules_out or result['root'] / 'modules').resolve()
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('targets', nargs='*', help='Devices targets; default: all')
    parser.add_argument('--kernel-out', type=Path, help='kernel-build output or imported archive root/build directory')
    parser.add_argument('--modules-out', type=Path, help='default: <kernel-output-root>/modules')
    parser.add_argument('--skip-kernel', action='store_true', help='use an existing explicit --kernel-out; still check freshness')
    parser.add_argument('--skip-kernel-check', action='store_true', help='reuse an old explicit output without Git/config freshness checks; implies --skip-kernel')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'kernel')
    parser.add_argument('--model', type=Path, default=ROOT / 'config/kernel/model.toml')
    parser.add_argument('--profile')
    parser.add_argument('--fragment', action='append', type=Path, default=[])
    parser.add_argument('--cross-compile', default='aarch64-linux-gnu-')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--warnings', choices=['0', '1', '2', '3'], default='1')
    parser.add_argument('--prepare-only', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    try:
        result = prepare(args)
        summary = {'kernel_out': str(result['root']), 'kdir': str(result['kdir']),
                   'modules_out': str(result['modules']), 'kernelrelease': result['kernelrelease'],
                   'kernel_freshness_checked': result['freshness_checked']}
        if not args.prepare_only:
            command = ['make', '--no-print-directory', '-C', str(ROOT / 'Devices'),
                       f'PYTHON={sys.executable}', f'KDIR={result["kdir"]}', f'OUT={result["modules"]}',
                       'KERNEL_PREPARED=1', f'SKIP_KERNEL_CHECK={int(args.skip_kernel_check)}',
                       f'MODEL={args.model.resolve()}', f'PROFILE={result["state"].get("profile") or "debug"}',
                       'FRAGMENTS=' + ' '.join(str(f.resolve()) for f in args.fragment),
                       f'CROSS_COMPILE={args.cross_compile}', f'JOBS={args.jobs}', f'W={args.warnings}',
                       *(args.targets or ['all'])]
            print('$ ' + shlex.join(command), flush=True)
            subprocess.run(command, check=True)
            summary['command'] = command
            (result['root'] / 'module-build.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f'build: {error}\n')


if __name__ == '__main__':
    main()
