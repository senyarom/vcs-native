#!/usr/bin/env python3
"""One bounded, silent native-resolution run: fixed pose and LOD 1 -> 3 -> 1."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import time
from verify_billboards import verify

ROOT = Path(__file__).resolve().parents[1]


def games():
    lines = subprocess.check_output(['ps', '-axo', 'pid=,comm='], text=True).splitlines()
    return [int(fields[0]) for line in lines
            if len(fields := line.strip().split(maxsplit=1)) == 2 and Path(fields[1]).name == 'VCSNative']


def player_files():
    return {str(p.relative_to(ROOT/'userdata')): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (ROOT/'userdata').rglob('*') if p.is_file() and p.name != '.launch.lock'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--visible', action='store_true', help='Show one window; audio stays silent.')
    args = parser.parse_args()
    if games():
        raise SystemExit('VCSNative is already running; no test game was started.')
    report = ROOT/'work/billboards'/('live-'+time.strftime('%Y%m%d-%H%M%S'))
    report.mkdir(parents=True)
    text = (ROOT/'userdata/VCSNative.ini').read_text()
    # Separate INIs: Apply saves back to active.ini, never to the input presets
    # or userdata. Reusing active.ini as the 1x preset would silently test 3x twice.
    text += '\n[Textures]\nDirectory='+str(ROOT/'textures/hdtextures')+'\n'
    if args.visible:
        text += '\n[Display]\nResolutionMode=Custom\nWidth=1440\nHeight=816\nFullscreen=true\n'
    for lod in (1, 3):
        (report/f'lod{lod}.ini').write_text(text+f'\n[GraphicsDistance]\nWorld=3\nLOD={lod}\n')
    active = report/'active.ini'
    active.write_text((report/'lod1.ini').read_text())
    sequence = report/'sequence.txt'
    sequence.write_text(f'3600 "{report / "lod3.ini"}"\n4200 "{report / "lod1.ini"}"\n')
    env = os.environ.copy()
    env.update(PSPRECOMP_CONFIG=str(active), PSPRECOMP_WINDOW='0', PSPRECOMP_AUDIO='1',
               SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy', PSPRECOMP_STOP_VBLANK='4601',
               PSPRECOMP_GE_GPU_COLOR_PREVIEW='1', PSPRECOMP_GE_GPU_DUMP_VBLANK='3300',
               PSPRECOMP_GE_GPU_DUMP_COUNT='3', PSPRECOMP_GE_GPU_DUMP_INTERVAL='600',
               PSPRECOMP_GE_GPU_DUMP_PATH=str(report/'frame.ppm'),
               PSPRECOMP_TEST_WORLD_POSE='-1700,-130,16',
               PSPRECOMP_GRAPHICS_TEST_SEQUENCE=str(sequence), PSPRECOMP_RAM_DUMP_DIR=str(report),
               PSPRECOMP_RAM_DUMP_START_VBLANK='3400', PSPRECOMP_RAM_DUMP_END_VBLANK='4600',
               PSPRECOMP_RAM_DUMP_INTERVAL='600')
    if args.visible:
        env['PSPRECOMP_WINDOW'] = '1'
        env.pop('SDL_VIDEODRIVER', None)
    for i, start in enumerate([300, 700, 1200, 1800, 2100, 2400, 2600]):
        key = 'PSPRECOMP_CTRL_PULSE'+(str(i+1) if i else '')
        env.update({key+'_BUTTONS': '16384', key+'_START_VBLANK': str(start), key+'_END_VBLANK': str(start+15)})
    before = player_files()
    start = time.monotonic()
    game = (ROOT/'run/world-game').resolve()
    binary = ROOT/('run/VCSNative.app/Contents/MacOS/VCSNative' if args.visible and os.uname().sysname == 'Darwin' else 'build/bin/Release/VCSNative')
    with (report/'game.log').open('w') as log:
        process = subprocess.Popen([str(binary), '--game-root', str(game)],
                                   cwd=report, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                if any(pid != process.pid for pid in games()):
                    raise RuntimeError('Another VCSNative appeared; closing only the test process.')
                if time.monotonic()-start >= 120:
                    raise RuntimeError('Billboard test exceeded 120 seconds.')
                time.sleep(1)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    result = dict(exit_code=process.returncode, seconds=round(time.monotonic()-start, 2),
                  user_files_unchanged=before == player_files(), remaining_processes=games(),
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  pose=[-1700, -130, 16], lod_sequence=[1, 3, 1], window=args.visible, audible_audio=False)
    (report/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(report)
    print(json.dumps(result))
    if process.returncode != 4 or not result['user_files_unchanged']:
        raise SystemExit('Test did not finish normally; inspect game.log.')
    verify(report)
    print('LOD lists, billboard visibility and existing flags verified from all three RAM snapshots.')


if __name__ == '__main__':
    main()
