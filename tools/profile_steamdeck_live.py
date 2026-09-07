#!/usr/bin/env python3
"""Visible, player-controlled Linux profiling session in an existing package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time


def games():
    found = subprocess.run(['pgrep', '-x', 'VCSNative'], capture_output=True, text=True)
    return [int(value) for value in found.stdout.split()]


def read(path):
    try:
        return Path(path).read_text().strip()
    except (OSError, ProcessLookupError):
        return None


def desktop_environment():
    """Use the user's visible Plasma session, never an invisible Gaming Mode X11 display."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('PSPRECOMP_') and k not in {'SDL_VIDEODRIVER', 'SDL_AUDIODRIVER'}}
    session = dict(line.split('=', 1) for line in subprocess.check_output(
        ['systemctl', '--user', 'show-environment'], text=True).splitlines() if '=' in line)
    if subprocess.run(['pgrep', '-x', 'plasmashell'], capture_output=True).returncode != 0:
        raise SystemExit('Visible profiling requires Steam Deck Desktop Mode; no game started.')
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR',
                'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP'):
        env.pop(key, None)
        if key in session:
            env[key] = session[key]
    if not (env.get('DISPLAY') or env.get('WAYLAND_DISPLAY')):
        raise SystemExit('Desktop display is unavailable; no game started.')
    return env


def telemetry(pid):
    task = Path('/proc') / str(pid)
    # Several duplicated FDs can refer to the same DRM client. Count its
    # cumulative engine time once, not once per descriptor.
    drm_clients = {}
    for fd in (task / 'fdinfo').glob('*'):
        fields = dict(line.split(':', 1) for line in (read(fd) or '').splitlines() if ':' in line)
        if 'drm-client-id' in fields:
            drm_clients[fields['drm-client-id'].strip()] = {
                key: value.strip() for key, value in fields.items()
                if key.startswith(('drm-', 'amd-'))}
    threads = {}
    for thread in (task / 'task').glob('*'):
        stat = read(thread / 'stat')
        if stat:
            fields = stat[stat.rfind(')') + 2:].split()
            threads[thread.name] = dict(name=read(thread/'comm'), state=fields[0],
                                       user_ticks=int(fields[11]), system_ticks=int(fields[12]))
    gpu = {}
    for device in Path('/sys/class/drm').glob('card[0-9]*/device'):
        busy = read(device/'gpu_busy_percent')
        if busy is None:
            continue
        values = {'busy_percent': busy, 'mem_busy_percent': read(device/'mem_busy_percent'),
                  'vram_bytes': read(device/'mem_info_vram_used'),
                  'gtt_bytes': read(device/'mem_info_gtt_used')}
        for hwmon in (device/'hwmon').glob('hwmon*'):
            for name in ['temp1_input', 'power1_average', 'power2_average', 'power1_cap',
                         'power2_cap', 'freq1_input']:
                values[name] = read(hwmon/name)
        gpu[device.parent.name] = values
    return dict(wall_time=time.time(), monotonic=time.monotonic(), threads=threads, gpu=gpu,
                drm_clients=drm_clients,
                process_status=read(task/'status'), io=read(task/'io'),
                cpu_stat=read('/proc/stat'),
                cpu_khz={p.parent.parent.name: read(p) for p in
                         Path('/sys/devices/system/cpu').glob('cpu[0-9]*/cpufreq/scaling_cur_freq')})


def capture_log(stream, report):
    # Record receipt times on the same CLOCK_MONOTONIC timebase as perf. This
    # lets an FPS dip select its own function samples rather than an average
    # over the whole route. stderr diagnostic lines are emitted unbuffered.
    with (report/'game.log').open('w') as log, (report/'events.jsonl').open('w') as events:
        for line in stream:
            log.write(line)
            if line.startswith(('[frame-time]', '[game-fps]', '[Vulkan perf]', '[realtime-speed]', '[camera-pose]', '[Vulkan raw model]')):
                events.write(json.dumps(dict(monotonic=time.monotonic(), line=line.rstrip()))+'\n')
            if line.startswith('[game-fps]'):
                log.flush()
                events.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=900)
    parser.add_argument('--vertex-census', action='store_true')
    parser.add_argument('--phases', action='store_true')
    # Uncapped boot currently starves the loading thread. The player can
    # enable it through F10 after loading; fixed-camera tests switch in gameplay.
    parser.add_argument('--fps', choices=['30', '60'], default='60',
                        help='Boot frame rate; use F10 for uncapped after loading.')
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    if games():
        raise SystemExit('An existing VCSNative is running; no second game started.')
    env = desktop_environment()
    report = root/'verification'/('live-profile-'+time.strftime('%Y%m%d-%H%M%S'))
    report.mkdir(parents=True)
    original = root/'userdata/VCSNative.ini'
    before = hashlib.sha256(original.read_bytes()).hexdigest()
    config = report/'settings.ini'
    config.write_text(original.read_text() + '\n' + '\n'.join([
        '[Display]', 'Enabled=true', 'Fullscreen=true', 'ShowFPS=true',
        '[Rendering]', 'Backend=Vulkan', 'InternalResolutionMode=Desktop',
        'TextureFilter=Bilinear', 'Mipmapping=On', 'MipmapFilter=Linear',
        'AnisotropicFiltering=16', '[Textures]', 'Enabled=true',
        'Directory='+str(root/'textures/hdtextures'), '[GraphicsDistance]',
        'World=3', 'LOD=3', '[Timing]', 'FrameRate='+args.fps,
        '[Audio]', 'Enabled=true', '[Diagnostics]',
        'LogFile='+str(report/'runtime.log'), 'FlushEveryLine=false', '']))
    env.update(PSPRECOMP_CONFIG=str(config), PSPRECOMP_WINDOW='1', PSPRECOMP_AUDIO='1',
               PSPRECOMP_FRAME_TIME_DIAG='1', PSPRECOMP_GPU_TIMING_DIAG='1',
               PSPRECOMP_VULKAN_STATS='1', PSPRECOMP_REALTIME_SPEED_DIAG='1',
               PSPRECOMP_REALTIME_SPEED_INTERVAL='120')
    if args.phases or args.vertex_census:
        env['PSPRECOMP_GE_PHASE_DIAG'] = '1'
    if args.vertex_census:
        env['PSPRECOMP_GE_VERTEX_CENSUS'] = '1'
        env['PSPRECOMP_TEST_CAPTURE_POSE'] = '1'
        control = report/'raw-model.txt'
        control.write_text('1\n')
        env['PSPRECOMP_GE_GPU_RAW_MODEL_CONTROL'] = str(control)
    metadata = dict(report=str(report), started=time.time(), duration_limit=args.seconds,
                    binary_sha256=hashlib.sha256((root/'run/VCSNative').read_bytes()).hexdigest(),
                    original_settings_sha256=before,
                    clock_ticks=os.sysconf('SC_CLK_TCK'), window=True, audible_audio=True,
                    automated_input=False, perf_frequency_hz=99, perf_stack_bytes=4096,
                    vertex_census=args.vertex_census, phases=args.phases or args.vertex_census,
                    frame_rate=args.fps, desktop=env.get('XDG_CURRENT_DESKTOP'),
                    perf_clock='CLOCK_MONOTONIC', perf_switch_seconds=30)
    (report/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
    perf = None
    with (report/'telemetry.jsonl').open('w') as samples, \
            (report/'perf.log').open('w') as perf_log:
        process = subprocess.Popen(['python3', str(root/'launch.py')], cwd=root, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, errors='replace')
        logger = threading.Thread(target=capture_log, args=(process.stdout, report), daemon=True)
        logger.start()
        metadata['pid'] = process.pid
        (report/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
        (root/'verification/current-live-profile.txt').write_text(str(report)+'\n')
        print(json.dumps(metadata), flush=True)
        start = time.monotonic()
        try:
            # Wait for launch.py to exec the one verified game binary.
            while process.poll() is None and time.monotonic()-start < 10:
                if read(Path('/proc')/str(process.pid)/'comm') == 'VCSNative':
                    break
                time.sleep(.1)
            if process.poll() is None:
                perf = subprocess.Popen(['perf', 'record', '-e', 'cycles:u', '-F', '99',
                                         '--clockid', 'mono', '--switch-output=30s',
                                         '--timestamp-filename',
                                         '--call-graph', 'dwarf,4096', '-p', str(process.pid),
                                         '-o', str(report/'perf.data')],
                                        stdout=perf_log, stderr=subprocess.STDOUT)
                metadata['perf_pid'] = perf.pid
                metadata['perf_started_monotonic'] = time.monotonic()
                (report/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
            while process.poll() is None:
                samples.write(json.dumps(telemetry(process.pid))+'\n')
                samples.flush()
                if perf is not None and perf.poll() is not None and 'perf_early_exit' not in metadata:
                    metadata['perf_early_exit'] = perf.returncode
                    (report/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
                    print('Profiler exited early; inspect perf.log. The player game remains open.', flush=True)
                if time.monotonic()-start >= args.seconds:
                    metadata['stop_reason'] = 'session time limit'
                    break
                if any(pid != process.pid for pid in games()):
                    metadata['stop_reason'] = 'another instance detected'
                    break
                time.sleep(1)
        finally:
            if perf is not None and perf.poll() is None:
                perf.send_signal(signal.SIGINT)
                try:
                    perf.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    perf.terminate()
                    perf.wait(timeout=5)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            logger.join(timeout=5)
            metadata.update(seconds=time.monotonic()-start, exit_code=process.returncode,
                            perf_exit_code=perf.returncode if perf else None,
                            original_settings_unchanged=before == hashlib.sha256(original.read_bytes()).hexdigest(),
                            remaining_games=games())
            (report/'result.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(json.dumps(metadata), flush=True)


if __name__ == '__main__':
    main()
