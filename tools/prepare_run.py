#!/usr/bin/env python3
"""Prepare a local run folder using only this checkout and installed libraries."""
from pathlib import Path
import os
import plistlib
import shutil
import sys
from prepare_world_catalog import prepare as prepare_world_catalog

ROOT = Path(__file__).resolve().parents[1]


def link(target, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        if path.resolve() == target.resolve():
            return
        path.unlink()
    elif path.exists():
        raise RuntimeError(f'Refusing to replace a local file: {path}')
    path.symlink_to(os.path.relpath(target, path.parent), target_is_directory=target.is_dir())


def main():
    build = Path(sys.argv[1]).expanduser().resolve() if len(sys.argv) > 1 else ROOT / 'build'
    source = build / 'bin/Release/VCSNative'
    if not source.is_file():
        raise SystemExit(f'Build VCSNative first: {source}')
    data = ROOT / 'userdata'
    data.mkdir(exist_ok=True)
    (data / 'SAVEDATA').mkdir(exist_ok=True)
    for source_name, target_name in [('VCSNative.sdl.ini', 'VCSNative.ini'), ('ProperShaders.sdl.ini', 'ProperShaders.ini')]:
        target = data / target_name
        if not target.exists():
            shutil.copy2(ROOT / 'config' / source_name, target)
    run = ROOT / 'run'
    if sys.platform == 'darwin':
        contents = run / 'VCSNative.app/Contents'
        executable_dir = contents / 'MacOS'
        contents.mkdir(parents=True, exist_ok=True)
        info = dict(CFBundleDevelopmentRegion='en', CFBundleDisplayName='VCSNative',
                    CFBundleExecutable='VCSNative', CFBundleIdentifier='local.vcsnative',
                    CFBundleName='VCSNative', CFBundlePackageType='APPL',
                    CFBundleShortVersionString='0.1', CFBundleVersion='1',
                    NSHighResolutionCapable=True)
        (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    else:
        executable_dir = run
    executable_dir.mkdir(parents=True, exist_ok=True)
    for name in ('VCSNative.ini', 'ProperShaders.ini', 'SAVEDATA'):
        link(data / name, executable_dir / name)
    catalog = prepare_world_catalog(ROOT / 'assets/game', run / 'native-world')
    link(catalog, executable_dir / 'NativeWorld')
    link(ROOT / 'assets/game', executable_dir / 'PSP_DATA')
    link(ROOT / 'assets/data/VCSProject2DFX_Lights.bin', executable_dir / 'VCSProject2DFX_Lights.bin')
    for name in ('PSP_DATA', 'NativeWorld', 'SAVEDATA', 'VCSNative.ini', 'ProperShaders.ini', 'VCSProject2DFX_Lights.bin'):
        link((executable_dir / name).resolve(), source.parent / name)
    # Publish only after every dependency is ready. Publishing the executable
    # first lets a concurrent launch silently miss NativeWorld during startup.
    temporary = executable_dir / 'VCSNative.new'
    shutil.copy2(source, temporary)
    temporary.replace(executable_dir / 'VCSNative')
    print(f'Ready: {executable_dir / "VCSNative"}')


if __name__ == '__main__':
    main()
