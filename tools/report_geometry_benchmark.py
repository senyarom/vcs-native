#!/usr/bin/env python3
"""Summarize a completed geometry benchmark using its exact executable for perf."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess


def numbers(line):
    return {k: float(v) for k,v in re.findall(r'(\w+)=([0-9.]+)', line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    report = args.report.resolve()
    run = json.loads((report/'result.json').read_text())
    assert hashlib.sha256(args.binary.read_bytes()).hexdigest() == run['binary_sha256']
    # Our ELF has no build ID. Never resolve old samples against a newly
    # deployed binary at the same pathname: snapshot an explicit symbol root.
    package = report.parents[2]
    symfs = report/'symbols'
    executable = symfs / str(package/'run/VCSNative').lstrip('/')
    executable.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.binary, executable)
    for name in ['usr','lib','lib64']:
        if not (symfs/name).exists(): (symfs/name).symlink_to('/'+name)
    begin, end = run['perf_interval']
    output = subprocess.check_output([
        'perf','report','--stdio','--no-children','--call-graph','none','--percent-limit','0',
        '--show-total-period','-n','--sort','pid,dso,symbol','-t',';',
        '--symfs',str(symfs),'--time',f'{begin:.9f},{end:.9f}',
        '-i',str(report/'perf.data')],text=True,stderr=subprocess.STDOUT)
    (report/'perf-report.txt').write_text(output)
    rows = []
    totals = defaultdict(int)
    for line in output.splitlines():
        fields = [f.strip() for f in line.split(';')]
        if len(fields) < 6 or not fields[0].endswith('%'): continue
        try:
            samples, period = int(fields[1]),int(fields[2])
            tid = int(fields[3].split(':',1)[0])
        except ValueError: continue
        totals[tid] += period
        rows.append(dict(samples=samples,period=period,tid=tid,dso=fields[4],symbol=fields[5]))
    total = sum(totals.values())
    assert total, 'No perf samples'
    for row in rows:
        row['percent_thread'] = 100*row['period']/totals[row['tid']]
        row['percent_all'] = 100*row['period']/total
    rows.sort(key=lambda r: -r['period'])
    events = [json.loads(line) for line in (report/'events.jsonl').read_text().splitlines()]
    rates = [numbers(e['line']) for e in events if e['line'].startswith('[game-fps]')
             and begin + 2.1 <= e['monotonic'] <= end]
    frame_rows = [numbers(e['line']) for e in events if e['line'].startswith('[frame-time]')
                  and begin <= e['monotonic'] <= end]
    sensor_rows = [json.loads(line) for line in (report/'telemetry.jsonl').read_text().splitlines()]
    sensor_rows = [r for r in sensor_rows if begin <= r['monotonic'] <= end]
    sensors = {}
    if len(sensor_rows) > 1:
        a,b = sensor_rows[0],sensor_rows[-1]
        seconds = b['monotonic']-a['monotonic']
        pid = str(run['pid'])
        ticks = sum(b['threads'][pid][k]-a['threads'][pid][k] for k in ['user_ticks','system_ticks'])
        # Linux USER_HZ from the recorder's os.sysconf on Steam Deck is 100.
        sensors['main_cpu_percent'] = ticks/seconds
        engines = defaultdict(int)
        for client,counters in b['drm_clients'].items():
            old = a['drm_clients'].get(client,{})
            for key,value in counters.items():
                if key.startswith('drm-engine-') and key in old:
                    engines[key] += int(value.split()[0])-int(old[key].split()[0])
        sensors['gpu_engine_percent'] = {k:100*v/1e9/seconds for k,v in engines.items()}
        sensors['cpu_mhz_mean'] = statistics.mean(float(v)/1000 for r in sensor_rows for v in r['cpu_khz'].values() if v)
        sensors['temperature_c_mean'] = statistics.mean(float(g['temp1_input'])/1000 for r in sensor_rows for g in r['gpu'].values() if g.get('temp1_input'))
    draws = [numbers(line) for line in (report/'game.log').read_text().splitlines()
             if line.startswith('[gpu-time]')]
    vblank_begin,vblank_end = run['measured_vblanks']
    draws = [r for r in draws if vblank_begin <= r.get('vblank',0) < vblank_end]
    result = dict(binary_sha256=run['binary_sha256'], frame_ms=run['means']['frame_us']/1000,
                  ge_cpu_ms=run['means']['ge_us']/1000, present_ms=run['means']['present_us']/1000,
                  remaining_host_ms=run['means']['guest_cpu_us']/1000,
                  frame_throughput=1e6/run['means']['frame_us'],
                  game_fps=sum(r['frames'] for r in rates)/sum(r['seconds'] for r in rates),
                  game_fps_window_min=min(r['fps'] for r in rates),
                  game_fps_window_max=max(r['fps'] for r in rates),
                  frame_p95_ms=sorted(r['frame_us'] for r in frame_rows)[int(.95*len(frame_rows))-1]/1000,
                  measured_seconds=end-begin, measured_frames=len(frame_rows),
                  detailed_phase_timers=run.get('phases', False),
                  draws_per_frame=statistics.mean(r['game_draws'] for r in draws),
                  triangles_per_frame=statistics.mean(r['game_tris'] for r in draws),
                  sensors=sensors, samples=sum(r['samples'] for r in rows),
                  lost_samples=re.findall(r'Total Lost Samples:\s*(\d+)',output),
                  top_main=[r for r in rows if r['tid']==run['pid']][:25], top_all=rows[:25])
    (report/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if not k.startswith('top_')},indent=2))
    for r in result['top_main'][:10]: print(f"{r['percent_thread']:.2f}% {r['symbol'][:160]}")


if __name__ == '__main__':
    main()
