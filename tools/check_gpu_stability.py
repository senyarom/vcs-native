#!/usr/bin/env python3
"""One visible, silent eight-minute native-HD run with automatic shutdown."""
from pathlib import Path
import hashlib
import json
import os
import re
import statistics
import subprocess
import time
from check_billboards import ROOT, games, player_files


def main():
    if games():
        raise SystemExit('VCSNative is already running; no test game was started.')
    report = ROOT/'work/gpu-fallback'/('live-'+time.strftime('%Y%m%d-%H%M%S'))
    report.mkdir(parents=True)
    config = report/'active.ini'
    config.write_text((ROOT/'userdata/VCSNative.ini').read_text()+
        '\n[Textures]\nDirectory='+str(ROOT/'textures/hdtextures')+
        '\n[GraphicsDistance]\nWorld=3\nLOD=3\n')
    env = os.environ.copy()
    env.pop('SDL_VIDEODRIVER', None)
    env.update(PSPRECOMP_CONFIG=str(config), PSPRECOMP_WINDOW='1', PSPRECOMP_AUDIO='1',
               SDL_AUDIODRIVER='dummy', PSPRECOMP_STOP_VBLANK='28801', PSPRECOMP_VULKAN_STATS='1',
               PSPRECOMP_GE_GPU_COLOR_PREVIEW='1', PSPRECOMP_GE_GPU_DUMP_VBLANK='3300',
               PSPRECOMP_GE_GPU_DUMP_COUNT='3', PSPRECOMP_GE_GPU_DUMP_INTERVAL='12000',
               PSPRECOMP_GE_GPU_DUMP_PATH=str(report/'frame.ppm'),
               PSPRECOMP_TEST_WORLD_POSE='-1700,-130,16', PSPRECOMP_RAM_DUMP_DIR=str(report),
               PSPRECOMP_RAM_DUMP_START_VBLANK='3400', PSPRECOMP_RAM_DUMP_END_VBLANK='27400',
               PSPRECOMP_RAM_DUMP_INTERVAL='12000')
    for i, start in enumerate([300,700,1200,1800,2100,2400,2600]):
        key='PSPRECOMP_CTRL_PULSE'+(str(i+1) if i else '')
        env.update({key+'_BUTTONS':'16384',key+'_START_VBLANK':str(start),key+'_END_VBLANK':str(start+15)})
    before=player_files()
    binary=ROOT/('run/VCSNative.app/Contents/MacOS/VCSNative' if os.uname().sysname=='Darwin' else 'build/bin/Release/VCSNative')
    binary_hash=hashlib.sha256(binary.read_bytes()).hexdigest()
    game=(ROOT/'run/world-game').resolve()
    start=time.monotonic()
    print(report, flush=True)
    with (report/'game.log').open('w') as log:
        process=subprocess.Popen([str(binary),'--game-root',str(game)],
                                 cwd=report,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                if any(pid!=process.pid for pid in games()):
                    raise RuntimeError('Another VCSNative appeared; closing only the test process.')
                if time.monotonic()-start>550:
                    raise RuntimeError('Stability test exceeded 550 seconds.')
                time.sleep(1)
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait()
    log=(report/'game.log').read_text()
    perf=[dict((k,float(v)) for k,v in re.findall(r'(\w+)=([\d.]+)',line))
          for line in log.splitlines() if line.startswith('[Vulkan perf]')]
    fps=[float(n) for n in re.findall(r'\[game-fps\] fps=([\d.]+)',log)]
    errors=[line for line in log.splitlines() if
            ('[presentation] GPU ->' in line and 'gpu_active=0' in line) or
            'failed:' in line or 'arena exhausted' in line or 'Validation Error' in line]
    result=dict(exit_code=process.returncode,seconds=round(time.monotonic()-start,2),
                user_files_unchanged=before==player_files(),remaining_processes=games(),
                binary_sha256=binary_hash,window=True,audible_audio=False,errors=errors,
                perf_samples=len(perf),late_game_fps_mean=statistics.mean(fps[-60:]) if fps else None,
                max_texture_bytes=max((p.get('texture_bytes',0) for p in perf),default=0),
                max_target_bytes=max((p.get('target_bytes',0) for p in perf),default=0),
                max_targets=max((p.get('targets',0) for p in perf),default=0),last_perf=perf[-1] if perf else {})
    (report/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)
    if process.returncode!=4 or not result['user_files_unchanged'] or errors or len(perf)<100:
        raise SystemExit('Stability check failed; inspect game.log and result.json.')


if __name__=='__main__':main()
