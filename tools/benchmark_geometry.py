#!/usr/bin/env python3
"""Bounded, visible Deck comparison at a fixed camera; uses the current package binary."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import signal
import statistics
import subprocess
import threading
import time

from profile_steamdeck_live import capture_log, desktop_environment, games, read, telemetry


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--pose', default='-1700,-130,16', help='Fixed world position x,y,z.')
    parser.add_argument('--direction', default='0,-1', help='Camera direction x,y or x,y,z, including pitch.')
    parser.add_argument('--checkpoint', type=Path, help='Recorded camera JSON; pins the player for local world streaming.')
    parser.add_argument('--measure-frames', type=int, default=1100, help='Measured fixed-view frames after warmup (120..30000).')
    parser.add_argument('--vertex-census', action='store_true', help='Count packed/CPU vertices by GE format.')
    parser.add_argument('--orbit', action='store_true', help='Eight fixed directions around the pinned base position.')
    parser.add_argument('--matrix-stream', choices=['0', '1'], default='1')
    parser.add_argument('--register-stream', choices=['0', '1'], default='1')
    parser.add_argument('--realtime-capped', choices=['0','1','default'], default='default', help='Compare capped wall-clock pacing with the legacy synthetic clock.')
    parser.add_argument('--raw-model', choices=['0', '1'], default='1')
    parser.add_argument('--packed', choices=['0', '1'], default='1')
    parser.add_argument('--fps', choices=['30', '60', 'Uncapped'], default='60')
    parser.add_argument('--fps-phase', action='append', default=[], metavar='VBLANK:FPS',
                        help='Additional live FPS change, e.g. 6000:Uncapped; uses the F10 Apply path.')
    parser.add_argument('--phases', action='store_true', help='Measure GE preparation phases (adds timer overhead).')
    parser.add_argument('--async-ge', action='store_true', help='Experiment with the existing asynchronous GE worker.')
    parser.add_argument('--pin-player', action='store_true', help='Pin the player too, using the existing pose diagnostic.')
    args = parser.parse_args()
    checkpoint = None
    if args.checkpoint:
        checkpoint = json.loads(args.checkpoint.read_text())
        for key in ('position', 'direction'):
            values = checkpoint[key]
            if len(values) != 3 or not all(math.isfinite(float(v)) for v in values):
                parser.error('Checkpoint requires finite three-dimensional position and direction')
        if sum(float(v)**2 for v in checkpoint['direction']) < .000001:
            parser.error('Checkpoint direction is zero')
        args.pose = ','.join(map(str, checkpoint['position']))
        args.direction = ','.join(map(str, checkpoint['direction']))
        args.pin_player = True
    if args.orbit and not args.pin_player:
        parser.error('--orbit requires --pin-player')
    if not 120 <= args.measure_frames <= 30000:
        parser.error('--measure-frames must be 120..30000')
    measure_end = 8000 if args.orbit else 3600+args.measure_frames
    stop_vblank = measure_end+100
    deadline = max(180, min(1200, 100+args.measure_frames/30))
    root = args.root.expanduser().resolve()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.label):
        parser.error('Use a filename-safe label')
    if games():
        raise SystemExit('VCSNative already running; no game started')
    env = desktop_environment()
    report = root/'verification/vulkan-geometry'/args.label
    report.mkdir(parents=True, exist_ok=False)
    def user_files():
        return {str(p.relative_to(root/'userdata')): digest(p)
                for p in (root/'userdata').rglob('*') if p.is_file()
                and p.suffix.lower() != '.log' and p.name != '.launch.lock'}
    before = user_files()
    config = report/'settings.ini'
    config.write_text((root/'userdata/VCSNative.ini').read_text() + '\n' + '\n'.join([
        '[Display]', 'Enabled=true', 'Fullscreen=true', 'ShowFPS=true',
        'ResolutionMode=Custom', 'Width=1280', 'Height=800',
        '[Rendering]', 'Backend=Vulkan', 'InternalResolutionMode=Custom',
        'InternalWidth=1280', 'InternalHeight=725',
        'TextureFilter=Bilinear', 'Mipmapping=On', 'MipmapFilter=Linear',
        'AnisotropicFiltering=16', '[Textures]', 'Enabled=true',
        'Directory='+str(root/'textures/hdtextures'), '[GraphicsDistance]',
        'World=3', 'LOD=3', '[Timing]', 'FrameRate=60',
        '[Audio]', 'Enabled=true', '[Diagnostics]',
        'LogFile='+str(report/'runtime.log'), 'FlushEveryLine=false', '']))
    env.update(PSPRECOMP_CONFIG=str(config), PSPRECOMP_WINDOW='1', PSPRECOMP_AUDIO='1',
               PSPRECOMP_FRAME_TIME_DIAG='1', PSPRECOMP_GPU_TIMING_DIAG='1',
               PSPRECOMP_VULKAN_STATS='1', PSPRECOMP_STOP_VBLANK=str(stop_vblank),
               PSPRECOMP_GE_MATRIX_STREAM=args.matrix_stream,
               PSPRECOMP_GE_REGISTER_STREAM=args.register_stream,
               PSPRECOMP_DX12_PACKED_0115=args.packed, PSPRECOMP_GE_GPU_RAW_MODEL=args.raw_model,
               PSPRECOMP_TEST_WORLD_POSE=args.pose, PSPRECOMP_TEST_WORLD_DIRECTION=args.direction,
               PSPRECOMP_TEST_WORLD_CAMERA_ONLY='1', PSPRECOMP_TEST_WORLD_HOUR='12',
               PSPRECOMP_GE_GPU_DUMP_PATH=str(report/'frame.ppm'),
               PSPRECOMP_GE_GPU_DUMP_VBLANK='4200', PSPRECOMP_GE_GPU_DUMP_COUNT='1',
               PSPRECOMP_GE_GPU_COLOR_PREVIEW='1')
    env['PSPRECOMP_AUDIO_SUMMARY'] = '1'
    if args.realtime_capped == 'default':
        env.pop('PSPRECOMP_REALTIME_CAPPED', None)
    else:
        env['PSPRECOMP_REALTIME_CAPPED'] = args.realtime_capped
    if args.orbit or args.checkpoint:
        env['PSPRECOMP_TEST_CAPTURE_POSE'] = '1'
    if args.orbit:
        env['PSPRECOMP_TEST_WORLD_ORBIT'] = '1'
    if args.pin_player:
        env.pop('PSPRECOMP_TEST_WORLD_CAMERA_ONLY')
    if args.phases or args.vertex_census:
        env['PSPRECOMP_GE_PHASE_DIAG'] = '1'
    if args.vertex_census:
        env['PSPRECOMP_GE_VERTEX_CENSUS'] = '1'
    if args.async_ge:
        env['PSPRECOMP_GE_ASYNC'] = '1'
    phases=[]
    if args.fps != '60': phases.append((3300,args.fps))
    for text in args.fps_phase:
        try:
            at,mode=text.split(':');at=int(at)
            if at<2800 or at>=stop_vblank or mode not in ('30','60','Uncapped'): raise ValueError()
        except ValueError: parser.error('--fps-phase requires an in-game vblank and 30,60,Uncapped')
        phases.append((at,mode))
    if phases:
        # Boot and scripted inputs use the same 60 Hz clock in every run.
        # Switch only after reaching gameplay; uncapped boot can consume all
        # input pulses while the game is still waiting on wall-clock I/O.
        sequence = report/'sequence.txt'
        entries=[]
        for index,(at,mode) in enumerate(sorted(phases)):
            measured_config = report/f'measured-{index}.ini'
            measured_config.write_text(config.read_text() + '\n[Timing]\nFrameRate='+mode+'\n')
            entries.append(str(at)+' '+json.dumps(str(measured_config))+'\n')
        sequence.write_text(''.join(entries))
        env['PSPRECOMP_GRAPHICS_TEST_SEQUENCE'] = str(sequence)
    for i, start in enumerate([300,700,1200,1800,2100,2400,2600]):
        key = 'PSPRECOMP_CTRL_PULSE' + (str(i+1) if i else '')
        env.update({key+'_BUTTONS':'16384',key+'_START_VBLANK':str(start),key+'_END_VBLANK':str(start+15)})
    metadata = dict(binary_sha256=digest(root/'run/VCSNative'), packed=args.packed, raw_model=args.raw_model, matrix_stream=args.matrix_stream, orbit=args.orbit,
                    fps=args.fps, phases=args.phases or args.vertex_census, async_ge=args.async_ge,
                    window=True, audio=True, pin_player=args.pin_player, pose=env['PSPRECOMP_TEST_WORLD_POSE'],
                    direction=env['PSPRECOMP_TEST_WORLD_DIRECTION'], stop_vblank=stop_vblank,
                    measured_vblanks=[3600,measure_end], deadline_seconds=deadline)
    metadata['register_stream'] = args.register_stream
    metadata['realtime_capped'] = args.realtime_capped
    metadata['audio_summary'] = True
    metadata['fps_phases'] = sorted(phases)
    if checkpoint:
        metadata['checkpoint'] = checkpoint
    (report/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
    started = time.monotonic()
    perf = None
    reached_gameplay = threading.Event()
    def observe_gameplay(stream):
        for line in stream:
            if line.startswith('[game-fps]'):
                reached_gameplay.set()
            yield line
    with (report/'perf.log').open('w') as perf_log, (report/'telemetry.jsonl').open('w') as sensors:
        process = subprocess.Popen(['python3',str(root/'launch.py')], cwd=root, env=env,
                                   stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace')
        logger = threading.Thread(target=capture_log,args=(observe_gameplay(process.stdout),report),daemon=True)
        logger.start()
        try:
            while process.poll() is None and time.monotonic()-started < 10:
                if read(Path('/proc')/str(process.pid)/'comm') == 'VCSNative': break
                time.sleep(.1)
            if process.poll() is None:
                perf = subprocess.Popen(['perf','record','-e','cycles:u','-F','99','--clockid','mono',
                                         '--call-graph','dwarf,4096','-p',str(process.pid),
                                         '-o',str(report/'perf.data')],stdout=perf_log,stderr=subprocess.STDOUT)
            while process.poll() is None:
                sensors.write(json.dumps(telemetry(process.pid))+'\n'); sensors.flush()
                elapsed = time.monotonic()-started
                if elapsed > deadline or (elapsed > 75 and not reached_gameplay.is_set()):
                    metadata['failure'] = 'Benchmark deadline' if elapsed > deadline else 'Gameplay was not reached within 75 seconds'
                    break
                if any(p != process.pid for p in games()): raise RuntimeError('Another game appeared')
                time.sleep(1)
        finally:
            if perf and perf.poll() is None:
                perf.send_signal(signal.SIGINT)
                try: perf.wait(timeout=10)
                except subprocess.TimeoutExpired: perf.kill(); perf.wait()
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            logger.join(timeout=5)
    events = [json.loads(line) for line in (report/'events.jsonl').read_text().splitlines()]
    frames = []
    for event in events:
        if not event['line'].startswith('[frame-time]'): continue
        values = {k: float(v) for k,v in re.findall(r'(\w+)=([0-9.]+)',event['line'])}
        if 3600 <= values.get('vblank',0) < measure_end:
            frames.append(dict(monotonic=event['monotonic'],**values))
    metadata.update(seconds=time.monotonic()-started, exit_code=process.returncode,
                    pid=process.pid, perf_exit_code=perf.returncode if perf else None,
                    userdata_unchanged=before==user_files(), remaining_games=games(),
                    samples=len(frames), means={k: statistics.mean(f[k] for f in frames)
                    for k in frames[0]} if frames else {})
    metadata['gameplay_samples'] = sum(f.get('ge_calls',0) > 0 for f in frames)
    metadata['valid'] = (process.returncode == 4 and len(frames) > 100
                         and metadata['gameplay_samples'] > 100 and reached_gameplay.is_set()
                         and metadata['userdata_unchanged'] and not metadata['remaining_games'])
    if frames: metadata['perf_interval']=[frames[0]['monotonic'],frames[-1]['monotonic']]
    (report/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(json.dumps(metadata),flush=True)
    assert metadata['valid'], metadata.get('failure','Incomplete benchmark or user data/process isolation changed')


if __name__ == '__main__':
    main()
