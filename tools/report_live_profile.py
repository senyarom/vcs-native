#!/usr/bin/env python3
"""Extract perf functions for an actual slow-FPS interval, while recording continues."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import subprocess


def perf(*args):
    return subprocess.check_output(['nice', '-n', '19', 'perf', *args], text=True, stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--context', type=float, default=2)
    parser.add_argument('--below', type=float, default=25)
    parser.add_argument('--label', default='slow-interval')
    parser.add_argument('--from-time', type=float, default=0,
                        help='Only consider FPS intervals ending after this monotonic timestamp.')
    parser.add_argument('--to-time', type=float, default=float('inf'),
                        help='Only consider FPS intervals ending before this monotonic timestamp.')
    args = parser.parse_args()
    root = args.report.resolve()
    metadata = json.loads((root/'metadata.json').read_text())
    events = []
    for line in (root/'events.jsonl').read_text().splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue  # Recording can be appending the final line.
        if event['line'].startswith('[game-fps]'):
            fields = {key: float(value) for key, value in re.findall(r'(\w+)=([\d.]+)', event['line'])}
            if fields['fps'] < args.below and args.from_time <= event['monotonic'] <= args.to_time:
                events.append(dict(time=event['monotonic'], **fields))
    if not events:
        raise SystemExit('No completed game FPS interval below the requested threshold yet.')
    target = min(events, key=lambda event: event['fps'])
    begin = target['time'] - target['seconds'] - args.context
    end = target['time'] + args.context
    chunks = []
    for path in sorted(root.glob('perf.data.*')):
        if not re.fullmatch(r'perf\.data\.\d+', path.name):
            continue
        header = perf('report', '--header-only', '--stdio', '-i', str(path))
        first = re.search(r'time of first sample\s*:\s*([\d.]+)', header)
        last = re.search(r'time of last sample\s*:\s*([\d.]+)', header)
        first_time = float(first[1]) if first else 0
        last_time = float(last[1]) if last else 0
        if last_time == 0:
            # perf 6.15 switched-output headers leave sample-time bounds zero.
            # Read actual event timestamps and cache only the completed file.
            cache = path.with_name(path.name+'.range.json')
            if cache.exists():
                first_time, last_time = json.loads(cache.read_text())
            else:
                times = perf('script', '--ns', '-G', '-F', 'time', '-i', str(path))
                values = [float(value) for value in re.findall(r'^\s*([\d]+\.[\d]+):?\s*$', times, re.MULTILINE)]
                if values:
                    first_time, last_time = min(values), max(values)
                    cache.write_text(json.dumps([first_time, last_time])+'\n')
        if last_time:
            chunks.append((path, first_time, last_time))
    if not chunks or max(last for _, _, last in chunks) < end:
        raise SystemExit('The selected slow interval is still in the active perf file; wait for its 30-second rotation.')
    rows = defaultdict(lambda: [0, 0])
    used = []
    lost = 0
    for path, first, last in chunks:
        if last < begin or first > end:
            continue
        output = perf('report', '--stdio', '--no-children', '--call-graph', 'none',
                      '--percent-limit', '0', '--show-total-period', '-n',
                      '--sort', 'pid,dso,symbol', '-t', ';', '--time', f'{begin:.9f},{end:.9f}',
                      '-i', str(path))
        loss = re.search(r'Total Lost Samples:\s*(\d+)', output)
        if loss:
            lost += int(loss[1])
        used.append(dict(file=path.name, first_sample=first, last_sample=last))
        for line in output.splitlines():
            fields = [field.strip() for field in line.split(';')]
            if len(fields) < 6 or not fields[0].endswith('%'):
                continue
            try:
                samples, period = int(fields[1]), int(fields[2])
                tid = int(fields[3].split(':', 1)[0])
            except ValueError:
                continue
            key = (tid, fields[4], fields[5])
            rows[key][0] += samples
            rows[key][1] += period
    totals = defaultdict(int)
    for (tid, _, _), (_, period) in rows.items():
        totals[tid] += period
    total = sum(totals.values())
    if total == 0:
        raise SystemExit('No perf samples in selected interval.')
    ranked = []
    for (tid, dso, symbol), (samples, period) in sorted(rows.items(), key=lambda item: -item[1][1]):
        ranked.append(dict(tid=tid, dso=dso, symbol=symbol, samples=samples, period=period,
                           percent_all=100*period/total, percent_thread=100*period/totals[tid]))
    telemetry = []
    for line in (root/'telemetry.jsonl').read_text().splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if begin <= item['monotonic'] <= end:
            telemetry.append(item)
    sensors = {}
    if len(telemetry) >= 2:
        a, b = telemetry[0], telemetry[-1]
        duration = b['monotonic']-a['monotonic']
        pid = str(metadata['pid'])
        ta, tb = a['threads'][pid], b['threads'][pid]
        sensors['seconds'] = duration
        sensors['main_cpu_percent'] = 100*(tb['user_ticks']+tb['system_ticks']-ta['user_ticks']-ta['system_ticks'])/metadata['clock_ticks']/duration
        engines = defaultdict(int)
        for client, counters in b['drm_clients'].items():
            old = a['drm_clients'].get(client, {})
            for key, value in counters.items():
                if key.startswith('drm-engine-') and key in old:
                    engines[key] += int(value.split()[0])-int(old[key].split()[0])
        sensors['gpu_engines_percent'] = {key: 100*value/1e9/duration for key, value in engines.items()}
    result = dict(label=args.label, fps_interval=target, start=begin, end=end,
                  clock='CLOCK_MONOTONIC; event times are log receipt times',
                  context_seconds=args.context, chunks=used, lost_samples_in_chunks=lost,
                  samples=sum(samples for samples, _ in rows.values()), sensors=sensors,
                  threads=[dict(tid=tid, percent_all=100*period/total) for tid, period in sorted(totals.items(), key=lambda item: -item[1])],
                  top_all=ranked[:30], top_main=[row for row in ranked if row['tid']==metadata['pid']][:30])
    output_path = root/('interval-'+re.sub(r'[^a-zA-Z0-9_-]', '-', args.label)+'.json')
    output_path.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({key: result[key] for key in ['fps_interval', 'start', 'end', 'samples', 'sensors']}, indent=2))
    print('Main thread self samples:')
    for row in result['top_main'][:12]:
        print(f"{row['percent_thread']:6.2f}% {row['symbol'][:220]}")
    print(output_path)


if __name__ == '__main__':
    main()
