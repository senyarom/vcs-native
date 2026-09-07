#!/usr/bin/env python3
"""Cross-compile on macOS for Steam Deck with Clang/LLD 21 and -O3."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
from prepare_steamdeck_sysroot import prepare, LOCK

ROOT = Path(__file__).resolve().parents[1]


def brew_prefix(formula):
    return Path(subprocess.check_output(['brew', '--prefix', formula], text=True).strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-steamdeck')
    parser.add_argument('--sysroot', type=Path, default=ROOT / 'work/toolchains/steamdeck/sysroot')
    parser.add_argument('--jobs', type=int, default=int(os.environ.get('CMAKE_BUILD_PARALLEL_LEVEL', '4')))
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--deploy', action='store_true', help='Test and install directly in steamdeck:~/Downloads/VCSNative.')
    parser.add_argument('--smoke', action='store_true', help='With --deploy, also run a bounded headless game test.')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('This toolchain runs on macOS. Use tools/build.sh for a native Linux build.')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.smoke and not args.deploy:
        parser.error('--smoke requires --deploy')
    if args.configure_only and args.deploy:
        parser.error('--configure-only cannot be combined with --deploy')
    build = args.build_dir.resolve()
    cache = build / 'CMakeCache.txt'
    toolchain = ROOT / 'cmake/toolchains/steamdeck.cmake'
    if cache.exists():
        values = dict(line.split('=', 1) for line in cache.read_text().splitlines()
                      if '=' in line and line.startswith('CMAKE_TOOLCHAIN_FILE:'))
        if str(toolchain) not in values.values():
            parser.error('Build directory belongs to another toolchain; use a separate --build-dir')
    build.mkdir(parents=True, exist_ok=True)
    # CMake may compact Ninja's log while regenerating. Never configure the
    # same directory while its Ninja process is still writing build records.
    build_lock = (build / '.steamdeck-build.lock').open('a')
    try:
        fcntl.flock(build_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error('Another Steam Deck build/configure is using this build directory')
    llvm = Path(os.environ.get('VCS_LLVM_ROOT') or brew_prefix('llvm@21')).resolve()
    # Keep the ld.lld symlink name: the multicall driver selects ELF by argv[0].
    lld = Path(os.environ.get('VCS_LLD') or brew_prefix('lld@21') / 'bin/ld.lld').absolute()
    for path in (llvm / 'bin/clang++', llvm / 'bin/llvm-ar', lld):
        if not path.is_file():
            parser.error(f'Missing {path}; install brew install llvm@21 lld@21 cmake ninja pkgconf glslang')
    for program in ('cmake', 'ninja', 'pkg-config', 'glslangValidator'):
        if not shutil.which(program):
            parser.error(f'Missing host tool: {program}')
    version = subprocess.check_output([str(llvm / 'bin/clang++'), '--version'], text=True)
    if 'clang version 21.' not in version:
        parser.error('Use LLVM Clang 21 for this verified toolchain')
    sysroot = prepare(args.sysroot)
    env = os.environ.copy()
    # Target discovery must not inherit Homebrew headers or macOS SDK flags.
    for key in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH',
                'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS',
                'PKG_CONFIG_PATH', 'CMAKE_PREFIX_PATH', 'CC', 'CXX'):
        env.pop(key, None)
    env.update(VCS_STEAMDECK_SYSROOT=str(sysroot), VCS_LLVM_ROOT=str(llvm), VCS_LLD=str(lld),
               PKG_CONFIG_SYSROOT_DIR=str(sysroot), PKG_CONFIG_PATH='',
               PKG_CONFIG_LIBDIR=os.pathsep.join(str(sysroot / p) for p in
                   ('usr/lib/x86_64-linux-gnu/pkgconfig', 'usr/lib/pkgconfig', 'usr/share/pkgconfig')))
    command = ['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja',
        f'-DCMAKE_TOOLCHAIN_FILE={ROOT / "cmake/toolchains/steamdeck.cmake"}',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG',
        '-DPSPRECOMP_GENERATED_OPT_LEVEL=3', '-DPSPRECOMP_HOT_GENERATED_OPT_LEVEL=3',
        '-DPSPRECOMP_HOST_OPT_LEVEL=3', '-DPSPRECOMP_ENABLE_SANITIZERS=OFF',
        '-DPSPRECOMP_BUILD_TESTS=ON', '-DPSPRECOMP_BUILD_PROFILE_TESTS=ON',
        f'-DPython3_EXECUTABLE={sys.executable}']
    subprocess.run(command, env=env, check=True)
    commands = json.loads((build / 'compile_commands.json').read_text())
    for entry in commands:
        flags = shlex.split(entry['command'])
        levels = [flag for flag in flags if flag in ('-O0', '-O1', '-O2', '-O3', '-Os', '-Oz', '-Og', '-Ofast')]
        if not levels or levels[-1] != '-O3':
            raise RuntimeError(f"Not compiled with -O3: {entry['file']}: {levels}")
    print(f'Verified -O3 on all {len(commands)} compilation commands', flush=True)
    if args.configure_only:
        return
    start = time.monotonic()
    subprocess.run(['cmake', '--build', str(build), '--parallel', str(args.jobs)], env=env, check=True)
    binary = build / 'bin/Release/VCSNative'
    identity = subprocess.check_output(['file', str(binary)], text=True).strip()
    if 'ELF 64-bit' not in identity or 'x86-64' not in identity:
        raise RuntimeError(f'Unexpected output format: {identity}')
    (build / 'cross-build.json').write_text(json.dumps({
        'host': 'macOS', 'target': 'Linux x86-64', 'compiler': version.strip(),
        'linker': subprocess.check_output([str(lld), '--version'], text=True).strip(),
        'optimization': 'O3 (all compilation units)', 'compile_commands': len(commands),
        'build_seconds': round(time.monotonic() - start, 2), 'binary': str(binary),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'sysroot_lock_sha256': hashlib.sha256(LOCK.read_bytes()).hexdigest(),
        'tests': 'Built for Linux; run them on Steam Deck before publishing.'
    }, indent=2) + '\n')
    print(f'Ready: {binary}\nLinux tests built; execution requires Steam Deck. macOS run/ unchanged.')
    if args.deploy:
        command = [sys.executable, str(ROOT / 'tools/test_steamdeck.py'), '--build-dir', str(build)]
        if args.smoke:
            command.append('--smoke')
        subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
