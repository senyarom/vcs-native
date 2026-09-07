#!/usr/bin/env python3
"""One bounded, silent native-resolution run: fixed pose and LOD 1 -> 3 -> 1."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import os
import subprocess
import struct
import sys
import time

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
    parser.add_argument('--pose', default='-1700,-130,16', help='Fixed world camera position x,y,z.')
    parser.add_argument('--hold-vblanks', type=int, default=600, help='Duration of the 3x phase (minimum 600).')
    parser.add_argument('--packaged', action='store_true', help='Test the ordinary packaged binary and automatic cache discovery.')
    parser.add_argument('--trace', action='store_true', help='Trace GE control flow and dump RAM on a command error.')
    parser.add_argument('--direction', default='0,-1', help='Horizontal camera direction x,y.')
    parser.add_argument('--hour', type=int, choices=range(24), help='Hold game hour for a repeatable day/night scene.')
    parser.add_argument('--camera-only', action='store_true', help='Move only the camera, keeping the player in the starting mission area.')
    args = parser.parse_args()
    pose = [float(v) for v in args.pose.split(',')]
    direction = [float(v) for v in args.direction.split(',')]
    if len(pose) != 3 or not all(map(math.isfinite,pose)) or args.hold_vblanks < 600:
        parser.error('pose needs three coordinates; hold-vblanks must be at least 600')
    if len(direction) != 2 or not all(map(math.isfinite,direction)) or math.hypot(*direction) <= .001:
        parser.error('direction needs two finite coordinates and a nonzero length')
    restore = 3600 + args.hold_vblanks
    stop = restore + 401
    if games():
        raise SystemExit('VCSNative is already running; no test game was started.')
    report = ROOT/'work/model-lod'/('live-'+time.strftime('%Y%m%d-%H%M%S'))
    report.mkdir(parents=True)
    text = (ROOT/'userdata/VCSNative.ini').read_text()
    # Separate INIs: Apply saves back to active.ini, never to the input presets
    # or userdata. Reusing active.ini as the 1x preset would silently test 3x twice.
    text += '\n[Textures]\nDirectory='+str(ROOT/'textures/hdtextures')+'\n'
    text += '\n[Diagnostics]\nLogFile='+str(report/'runtime.log')+'\n'
    if args.visible:
        text += '\n[Display]\nResolutionMode=Custom\nWidth=1440\nHeight=816\nFullscreen=true\n'
    for lod in (1, 3):
        (report/f'lod{lod}.ini').write_text(text+f'\n[GraphicsDistance]\nWorld=3\nLOD={lod}\n')
    active = report/'active.ini'
    active.write_text((report/'lod1.ini').read_text())
    sequence = report/'sequence.txt'
    sequence.write_text(f'3600 "{report / "lod3.ini"}"\n{restore} "{report / "lod1.ini"}"\n')
    env = os.environ.copy()
    env.update(PSPRECOMP_CONFIG=str(active), PSPRECOMP_WINDOW='0', PSPRECOMP_AUDIO='1',
               SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy', PSPRECOMP_STOP_VBLANK=str(stop),
               PSPRECOMP_GE_GPU_COLOR_PREVIEW='1', PSPRECOMP_GE_GPU_DUMP_VBLANK='3300',
               PSPRECOMP_GE_GPU_DUMP_COUNT=str((stop-3300)//600+1), PSPRECOMP_GE_GPU_DUMP_INTERVAL='600',
               PSPRECOMP_GE_GPU_DUMP_PATH=str(report/'frame.ppm'),
               PSPRECOMP_TEST_WORLD_POSE=args.pose,
               PSPRECOMP_TEST_WORLD_DIRECTION=args.direction,
               PSPRECOMP_GRAPHICS_TEST_SEQUENCE=str(sequence), PSPRECOMP_RAM_DUMP_DIR=str(report),
               PSPRECOMP_RAM_DUMP_START_VBLANK='3400', PSPRECOMP_RAM_DUMP_END_VBLANK=str(stop-1),
               PSPRECOMP_RAM_DUMP_INTERVAL='600')
    if args.hour is not None:env['PSPRECOMP_TEST_WORLD_HOUR']=str(args.hour)
    if args.camera_only:env['PSPRECOMP_TEST_WORLD_CAMERA_ONLY']='1'
    if args.visible:
        env['PSPRECOMP_WINDOW'] = '1'
        env.pop('SDL_VIDEODRIVER', None)
    for i, start in enumerate([300, 700, 1200, 1800, 2100, 2400, 2600]):
        key = 'PSPRECOMP_CTRL_PULSE'+(str(i+1) if i else '')
        env.update({key+'_BUTTONS': '16384', key+'_START_VBLANK': str(start), key+'_END_VBLANK': str(start+15)})
    before = player_files()
    start = time.monotonic()
    game = ROOT/'assets/game'
    env['PSPRECOMP_WORLD_CATALOG']=str(ROOT/'work/model-lod/catalog-cache')
    env['PSPRECOMP_WORLD_LOD_TRACE']='1'
    if args.trace:
        env['PSPRECOMP_GE_CONTROL_TRACE']='1'
        env['PSPRECOMP_GE_FAILURE_DUMP']=str(report/'failure-ram.bin')
    binary = ROOT/'build/bin/Release/VCSNative'
    if args.packaged:
        binary = ROOT/('run/VCSNative.app/Contents/MacOS/VCSNative' if sys.platform=='darwin' else 'run/VCSNative')
        env.pop('PSPRECOMP_WORLD_CATALOG',None)
    with (report/'game.log').open('w') as log:
        process = subprocess.Popen([str(binary), '--game-root', str(game)],
                                   cwd=report, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                if any(pid != process.pid for pid in games()):
                    raise RuntimeError('Another VCSNative appeared; closing only the test process.')
                if time.monotonic()-start >= stop/30+30:
                    raise RuntimeError('World LOD test exceeded its wall-clock limit.')
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
                  pose=pose, direction=args.direction, hour=args.hour, camera_only=args.camera_only,
                  lod_sequence=[1, 3, 1], window=args.visible, audible_audio=False)
    camera_samples = {}
    for ram in sorted(report.glob('ram_vblank_*.bin')):
        with ram.open('rb') as snapshot:
            snapshot.seek(0x08BC7E30 + 0x9B0 - 0x08000000)
            camera_samples[ram.name] = struct.unpack('<3f', snapshot.read(12))
    result['camera_samples'] = camera_samples
    (report/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(report)
    print(json.dumps(result))
    if process.returncode != 4 or not result['user_files_unchanged'] or f'VBlank diagnostic stop at {stop}' not in (report/'game.log').read_text():
        raise SystemExit('Test did not finish normally; inspect game.log.')
    if '[world-lod] lod=3 ' not in (report/'game.log').read_text():
        raise SystemExit('Native world LOD never rendered at 3x; this is not a valid comparison.')
    if args.camera_only and (len(camera_samples) < 3 or any(
            math.dist(position, pose) > .01 for position in camera_samples.values())):
        raise SystemExit('The rendered camera moved away from the requested pose; comparison invalid.')
    for frame in range(3299,stop,600):
        if not (report/f'frame_{frame}.ppm').is_file():
            raise SystemExit(f'Missing frame capture {frame}')
    print('Native world LOD captures complete; inspect frames and RAM.')


if __name__ == '__main__':
    main()
