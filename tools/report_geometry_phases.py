#!/usr/bin/env python3
"""Compare completed-game FPS within each mode of a geometry benchmark."""
import argparse
import json
from pathlib import Path
import statistics

from report_geometry_benchmark import numbers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    report = args.report.resolve()
    run = json.loads((report/'result.json').read_text())
    assert run.get('valid', True), 'Benchmark did not reach gameplay or failed isolation checks'
    events = [json.loads(line) for line in (report/'events.jsonl').read_text().splitlines()]
    frames = [(e['monotonic'], numbers(e['line'])) for e in events if e['line'].startswith('[frame-time]')]
    rates = [(e['monotonic'], numbers(e['line'])) for e in events if e['line'].startswith('[game-fps]')]
    speeds = [(e['monotonic'], numbers(e['line'])) for e in events if e['line'].startswith('[realtime-speed]')]
    draws = [numbers(line) for line in (report/'game.log').read_text().splitlines() if line.startswith('[gpu-time]')]
    transitions = [(0, '60'), *(tuple(phase) for phase in run.get('fps_phases', []))]
    if run['fps'] != '60' and not any(at == 3300 for at, _ in transitions):
        transitions.append((3300, run['fps']))
    transitions.sort()
    minimum, maximum = run['measured_vblanks']
    phases = []
    for index, (at, mode) in enumerate(transitions):
        begin = max(minimum, at + (120 if at else 0))
        end = min(maximum, transitions[index+1][0] if index+1 < len(transitions) else maximum)
        selected = [(t, f) for t, f in frames if begin <= f['vblank'] < end]
        if len(selected) < 100:
            continue
        first, last = selected[0][0], selected[-1][0]
        complete = [r for t, r in rates if t-r['seconds'] >= first and t <= last]
        assert complete, 'No completed gameplay intervals in phase'
        graphics = [d for d in draws if begin <= d['vblank'] < end]
        clock = [s for t, s in speeds if first+4.1 <= t <= last]
        means = {k: statistics.mean(f.get(k, 0) for _, f in selected)/1000 for k in
                 ('frame_us', 'ge_us', 'present_us', 'pacing_us', 'scheduler_idle_us', 'io_us')}
        phases.append(dict(mode=mode, vblanks=[begin,end], seconds=last-first,
            game_fps=sum(r['frames'] for r in complete)/sum(r['seconds'] for r in complete),
            game_fps_min=min(r['fps'] for r in complete), game_fps_max=max(r['fps'] for r in complete),
            frame_p95_ms=sorted(f['frame_us'] for _, f in selected)[int(.95*len(selected))-1]/1000,
            timings_ms={k.removesuffix('_us'):v for k,v in means.items()},
            draws=statistics.mean(d['game_draws'] for d in graphics),
            triangles=statistics.mean(d['game_tris'] for d in graphics),
            clock_speed_percent=statistics.mean(s['emulation_speed_percent'] for s in clock) if clock else None))
    result = dict(binary_sha256=run['binary_sha256'], phases=phases,
                  limitation='Fixed camera and settings; NPC/traffic state continues and is not a deterministic replay.')
    (report/'phases-summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
