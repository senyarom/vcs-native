#!/usr/bin/env python3
"""Summarize the eight stationary views of benchmark_geometry.py --orbit."""
import argparse
import json
from pathlib import Path
import re
import statistics


def numbers(line):
    return {key: float(value) for key, value in re.findall(r'(\w+)=([0-9.]+)', line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    root = args.report.expanduser().resolve()
    run = json.loads((root/'result.json').read_text())
    assert run.get('orbit'), 'This report is not an orbit benchmark'
    assert run['userdata_unchanged'] and not run['remaining_games'], 'Unclean test completion'
    records = {kind: [] for kind in ('frame-time', 'ge-phase', 'gpu-time')}
    for line in (root/'game.log').read_text().splitlines():
        for kind in records:
            if line.startswith('['+kind+']'):
                records[kind].append(numbers(line))
                break
    frames, rates = [], []
    for line in (root/'events.jsonl').read_text().splitlines():
        event = json.loads(line)
        row = dict(monotonic=event['monotonic'], **numbers(event['line']))
        if event['line'].startswith('[frame-time]'):
            frames.append(row)
        elif event['line'].startswith('[game-fps]'):
            rates.append(row)
    views = []
    for sector in range(8):
        # First 120 frames warm newly visible assets; remaining 480 are measured.
        begin, end = 3320+sector*600, 3800+sector*600
        select = lambda rows: [r for r in rows if begin <= r.get('vblank', 0) < end]
        f, g, gpu = (select(records[k]) for k in records)
        timed = select(frames)
        assert len(f) == len(g) == len(gpu) == len(timed) == 480, f'Incomplete sector {sector}'
        t0, t1 = timed[0]['monotonic'], timed[-1]['monotonic']
        rate = [r for r in rates if t0 <= r['monotonic']-r['seconds'] and r['monotonic'] <= t1]
        mean = lambda rows, key: statistics.mean(r[key] for r in rows)
        views.append(dict(sector=sector, angle_degrees=sector*45, frames=len(f),
            vblanks=[begin, end], perf_interval=[t0, t1],
            game_fps=sum(r['frames'] for r in rate)/sum(r['seconds'] for r in rate) if rate else None,
            throughput_fps=1e6/mean(f, 'frame_us'), frame_ms=mean(f, 'frame_us')/1000,
            frame_p95_ms=sorted(r['frame_us'] for r in f)[int(.95*len(f))-1]/1000,
            ge_ms=mean(f, 'ge_us')/1000, present_ms=mean(f, 'present_us')/1000,
            remaining_host_ms=mean(f, 'guest_cpu_us')/1000,
            vertex_decode_ms=mean(g, 'vdecode_us')/1000, list_ms=mean(g, 'list_us')/1000,
            input_vertices=mean(g, 'verts'), gpu_draws=mean(gpu, 'game_draws'),
            triangles=mean(gpu, 'game_tris')))
    result = dict(binary_sha256=run['binary_sha256'], pose=run['pose'],
        raw_model=run['raw_model'], matrix_stream=run['matrix_stream'], views=views,
        userdata_unchanged=run['userdata_unchanged'], remaining_games=run['remaining_games'])
    (root/'orbit-summary.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
