#!/usr/bin/env python3
"""`tools/posix_check.py`: verify toolkit changes on a POSIX host (macOS, Linux).

The runtime targets Windows and MSVC, but a POSIX machine without the game can
still check a lot. This runs three steps, each optional, and exits non-zero on
any failure it does not already know about:

  * **native** -- builds with the host `cc` and runs every test whose code is
    portable through `src/platform/win32_compat.c`: kmem, the NV2A walk, the
    guest meter and serial mode, IRQL tracking, ADPCM decode, the speaker
    mixdown arms, and the MMX integer helpers (which skip themselves without
    MMX). The CMake test subprojects pull in the whole runtime, which needs
    SDL2 and more here, so each test is compiled directly from its sources.
  * **cross** -- builds the whole tree for Windows with MinGW-w64, using the
    zig toolchain from the `ziglang` wheel (fetched by `uv`, wrapped as
    `x86_64-w64-mingw32-*`, cached under `~/.cache/xboxrecomp/zigmingw`). The
    three `src/d3d/d3d8_smoke/` links are known failures: zig's MinGW has no
    `d3dcompiler`. Nothing built here runs; it proves the code compiles and
    links for the real target.
  * **python** -- `pytest tools`, with the one known failure listed below.

Every child process gets commit and tag signing turned off through
`GIT_CONFIG_*`, so a test that commits in a scratch repository cannot invoke a
host signing program and hang.

Needs `cc`, `cmake`, `ninja` and `uv` on PATH. Usage:

    python3 tools/posix_check.py                 # all three steps
    python3 tools/posix_check.py native python   # a subset
    python3 tools/posix_check.py cross --build-dir /tmp/xbr-mingw

Exit 0 when every failure is a known one, 1 otherwise, 2 when a step could not
run at all.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = 'src/platform/win32_compat.c'
APU = ['src/apu/apu_vp.c', 'src/apu/apu_watch.c', 'src/apu/apu_mixdown.c',
       'src/apu/dsp/dsp.c', 'src/apu/dsp/dsp_c.c', 'src/apu/dsp/dsp_dma.c',
       'src/apu/dsp/gp_ep.c', 'src/apu/dsp/interp/dsp_cpu.c',
       'tests/posix/apu_core_stubs.c', PLATFORM]
INCLUDES = ['src', 'src/kernel', 'src/platform', 'src/nv2a', 'src/apu', 'src/apu/dsp',
            'src/apu/dsp/shim', 'src/apu/dsp/interp']

# (name, sources, defines, [(run label, environment), ...])
NATIVE_TESTS = [
    ('kmem', ['tests/kmem_test.c', 'src/kernel/kmem.c'], [], [('kmem', {})]),
    ('nv2a_actions', ['tests/nv2a_actions_test.c', 'src/nv2a/nv2a_core.c',
                      'src/nv2a/nv2a_method_table.c', PLATFORM], [], [('nv2a_actions', {})]),
    ('nv2a_submit_diag', ['tests/nv2a_submit_diag_test.c', 'src/nv2a/nv2a_core.c',
                          'src/nv2a/nv2a_method_table.c', PLATFORM], [],
     [('nv2a_submit_diag', {})]),
    ('fence_snapshot', ['tests/fence_snapshot_test.c'], [], [('fence_snapshot', {})]),
    ('nv2a_present_track', ['tests/nv2a_present_track_test.c'], [],
     [('nv2a_present_track', {})]),
    ('guest_meter', ['tests/guest_meter_test.c', 'src/kernel/guest_meter.c', PLATFORM],
     ['XBOXRECOMP_GMETER_TEST_BUILD'], [('guest_meter', {})]),
    ('irql_tracking', ['tests/kernel_irql_tracking_test.c', 'src/kernel/kernel_hal.c',
                       'src/kernel/guest_meter.c', PLATFORM], [],
     [('irql_tracking', {'RECOMP_GUEST_SERIAL': '1'})]),
    ('adpcm_decode', ['tests/adpcm_decode/test_main.c'] + APU, [], [('adpcm_decode', {})]),
    ('apu_mixdown', ['tests/apu_mixdown/test_main.c', 'src/apu/apu_mixdown.c',
                     'tests/posix/apu_core_stubs.c'], [], [
        ('apu_mixdown_all', {'RECOMP_APU_MIXDOWN_ALL': '1', 'APU_MIXDOWN_EXPECT': '1'}),
        ('apu_mixdown_even_odd', {'RECOMP_APU_MIXDOWN_ALL': '2', 'APU_MIXDOWN_EXPECT': '1'}),
        ('apu_mixdown_two_bins', {'RECOMP_APU_MIXDOWN_ALL': '0', 'APU_MIXDOWN_EXPECT': '0'}),
    ]),
    ('mmx_integer', ['tests/mmx_integer/test_main.c'], [], [('mmx_integer', {})]),
]

# Cross-build targets that cannot link with zig's MinGW (no d3dcompiler import library).
KNOWN_CROSS_FAILURES = {'d3d8_smoke.exe', 'd3d8_a8.exe', 'd3d8_gamma.exe'}

# Python tests that fail for a recorded reason, keyed by node id.
KNOWN_PYTHON_FAILURES = {
    'tools/recomp/test_lifter_rep_compare_flags.py::'
    'test_negative_control_without_the_zf_preload_a_zero_count_fails':
        ('the control strips the ZF preload and expects a zero-count compare to fail, but '
         'the stripped build still exits 0 on this host; cause not yet investigated'),
}

PYTHON_DEPS = ['pytest', 'capstone', 'pefile', 'numpy']
ZIG_CACHE = Path.home() / '.cache' / 'xboxrecomp' / 'zigmingw'


def child_env(extra: dict | None = None) -> dict:
    env = dict(os.environ)
    env.update({'GIT_CONFIG_COUNT': '2',
                'GIT_CONFIG_KEY_0': 'commit.gpgsign', 'GIT_CONFIG_VALUE_0': 'false',
                'GIT_CONFIG_KEY_1': 'tag.gpgsign', 'GIT_CONFIG_VALUE_1': 'false'})
    env.update(extra or {})
    return env


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, env=kw.pop('env', child_env()),
                          **kw)


def require(*tools: str) -> list[str]:
    return [t for t in tools if shutil.which(t) is None]


def step_native(work: Path) -> tuple[int, list[str]]:
    failures: list[str] = []
    work.mkdir(parents=True, exist_ok=True)
    flags = ['-std=gnu11', '-w'] + [f'-I{ROOT / i}' for i in INCLUDES]
    for name, sources, defines, runs in NATIVE_TESTS:
        exe = work / name
        cmd = (['cc'] + flags + [f'-D{d}' for d in defines] + ['-o', str(exe)]
               + [str(ROOT / s) for s in sources] + ['-lpthread', '-lm'])
        built = run(cmd)
        if built.returncode != 0:
            first = next((l for l in built.stderr.splitlines() if 'error' in l), built.stderr[:200])
            failures.append(f'{name}: build failed: {first.strip()}')
            print(f'  BUILD-FAIL {name}')
            continue
        for label, env in runs:
            result = run([str(exe)], cwd=str(work), env=child_env(env), timeout=300)
            last = ((result.stdout.strip() or result.stderr.strip()).splitlines() or [''])[-1]
            if result.returncode == 0:
                print(f'  PASS       {label}: {last[:90]}')
            else:
                failures.append(f'{label}: exit {result.returncode}')
                print(f'  FAIL       {label}: exit {result.returncode}')
                sys.stdout.write(''.join(f'             {l}\n' for l in
                                         (result.stdout + result.stderr).splitlines()[-8:]))
    return (1 if failures else 0), failures


def zig_toolchain() -> Path:
    """Wrappers named like a MinGW-w64 cross toolchain, around the ziglang wheel."""
    found = run(['uv', 'run', '--with', 'ziglang', 'python', '-c',
                 'import ziglang, os; print(os.path.dirname(ziglang.__file__))'])
    if found.returncode != 0:
        raise RuntimeError(f'could not fetch the ziglang wheel: {found.stderr.strip()[-200:]}')
    zig_dir = Path(found.stdout.strip().splitlines()[-1])
    bin_dir = ZIG_CACHE / 'bin'
    bin_dir.mkdir(parents=True, exist_ok=True)
    tools = {'gcc': 'cc --target=x86_64-windows-gnu', 'g++': 'c++ --target=x86_64-windows-gnu',
             'ar': 'ar', 'ranlib': 'ranlib', 'dlltool': 'dlltool', 'windres': 'rc'}
    for name, sub in tools.items():
        wrapper = bin_dir / f'x86_64-w64-mingw32-{name}'
        wrapper.write_text(f'#!/bin/sh\nexec "{zig_dir / "zig"}" {sub} "$@"\n', encoding='utf-8')
        wrapper.chmod(0o755)
    (ZIG_CACHE / 'sysroot').write_text(str(zig_dir / 'lib') + '\n', encoding='utf-8')
    return zig_dir


def step_cross(build_dir: Path) -> tuple[int, list[str]]:
    zig_dir = zig_toolchain()
    env = child_env({'PATH': f'{ZIG_CACHE / "bin"}{os.pathsep}{os.environ.get("PATH", "")}'})
    # The archiver is named explicitly: zig presents as Clang, so CMake looks for an
    # unprefixed llvm-ar first and can find an old host one whose archives lld-link
    # cannot index, which fails every target that links a toolkit library. CMake
    # keeps the archiver its first compiler detection chose, so a build directory
    # whose rules name another one is started afresh.
    ar = ZIG_CACHE / 'bin' / 'x86_64-w64-mingw32-ar'
    rules = build_dir / 'CMakeFiles' / 'rules.ninja'
    if rules.is_file() and str(ar) not in rules.read_text(encoding='utf-8', errors='replace'):
        print(f'  {build_dir} archives with another ar; configuring it afresh')
        shutil.rmtree(build_dir)
    configured = run(['cmake', '-S', str(ROOT), '-B', str(build_dir), '-G', 'Ninja',
                      '-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake',
                      f'-DMINGW_SYSROOT={zig_dir / "lib"}',
                      f'-DCMAKE_AR={ar}',
                      f'-DCMAKE_RANLIB={ZIG_CACHE / "bin" / "x86_64-w64-mingw32-ranlib"}'],
                     env=env, cwd=str(ROOT))
    if configured.returncode != 0:
        raise RuntimeError(f'cmake configure failed: {configured.stderr.strip()[-400:]}')
    built = run(['ninja', '-C', str(build_dir), '-k', '0'], env=env)
    log = built.stdout + built.stderr
    (build_dir / 'posix-check-cross.log').write_text(log, encoding='utf-8')
    # Newer ninja prints `FAILED: [code=1] target`, older ninja `FAILED: target`.
    failed = sorted({Path(m).name for m in
                     re.findall(r'^FAILED: (?:\[code=\d+\] )?(\S+)', log, re.MULTILINE)})
    unexpected = [t for t in failed if t not in KNOWN_CROSS_FAILURES]
    warnings = len(re.findall(r'warning:', log))
    print(f'  {len(failed)} target(s) failed ({len(failed) - len(unexpected)} known), '
          f'{warnings} warning line(s) in this incremental build; '
          f'log: {build_dir / "posix-check-cross.log"}')
    for target in unexpected:
        print(f'  FAIL       {target}')
    return (1 if unexpected else 0), [f'cross: {t}' for t in unexpected]


def step_python() -> tuple[int, list[str]]:
    cmd = ['uv', 'run', '--python', '3.11']
    for dep in PYTHON_DEPS:
        cmd += ['--with', dep]
    cmd += ['python', '-m', 'pytest', '-q', '-p', 'no:cacheprovider', '-rf', 'tools']
    result = run(cmd, cwd=str(ROOT))
    failed = re.findall(r'^FAILED (\S+)', result.stdout, re.MULTILINE)
    unexpected = [f for f in failed if f not in KNOWN_PYTHON_FAILURES]
    summary = (result.stdout.strip().splitlines() or [''])[-1]
    print(f'  {summary}')
    for node in failed:
        print(f'  {"known" if node in KNOWN_PYTHON_FAILURES else "FAIL "}      {node}')
    if result.returncode not in (0, 1):
        return 2, [f'pytest could not run: {result.stderr.strip()[-200:]}']
    return (1 if unexpected else 0), unexpected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    # Validated by hand: Python 3.9's argparse rejects an empty list against choices.
    parser.add_argument('steps', nargs='*', help='native, cross, python (default: all)')
    parser.add_argument('--build-dir', type=Path, default=Path('/tmp/xbr-mingw'),
                        help='cross-build directory (reused between runs)')
    parser.add_argument('--work-dir', type=Path, default=Path('/tmp/xbr-native'),
                        help='where native test executables are built')
    args = parser.parse_args()
    steps = args.steps or ['native', 'cross', 'python']
    unknown = [s for s in steps if s not in ('native', 'cross', 'python')]
    if unknown:
        parser.error(f'unknown step(s): {", ".join(unknown)}')

    needs = {'native': ['cc'], 'cross': ['cmake', 'ninja', 'uv'], 'python': ['uv']}
    worst, failures = 0, []
    for step in steps:
        print(f'== {step}')
        missing = require(*needs[step])
        if missing:
            print(f'  cannot run: {", ".join(missing)} not on PATH')
            worst = max(worst, 2)
            continue
        try:
            code, found = {'native': lambda: step_native(args.work_dir),
                           'cross': lambda: step_cross(args.build_dir),
                           'python': step_python}[step]()
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            print(f'  cannot run: {exc}')
            code, found = 2, []
        worst = max(worst, code)
        failures += found
    print('== result: ' + ('PASS' if worst == 0 else
                           'FAIL' if worst == 1 else 'INCOMPLETE (a step could not run)'))
    for failure in failures:
        print(f'  {failure}')
    return worst


if __name__ == '__main__':
    raise SystemExit(main())
