#!/usr/bin/env python3
"""Verify a completed check_billboards.py run against actual guest RAM captures."""
from pathlib import Path
import json
import re
import statistics
import struct
import sys


def verify(report):
    report = Path(report)
    result = json.loads((report/'result.json').read_text())
    if result['exit_code'] != 4 or not result['user_files_unchanged'] or result['remaining_processes']:
        raise ValueError('Game did not close cleanly or player files changed')
    frames = []
    for vblank in (3400, 4000, 4600):
        memory = (report/f'ram_vblank_{vblank:06d}.bin').read_bytes()
        def read(fmt, address):
            return struct.unpack_from('<'+fmt, memory, (address & 0x1FFFFFFF)-0x08000000)
        def word(address):
            return read('I', address)[0]
        camera = list(read('3f', 0x08BC7E30+0x9B0))
        record = word(0x08E91200+580)
        root = word(record+32)
        metadata = word(root)-96
        if read('8s', metadata)[0] != b'VCSBILL1':
            raise ValueError('Current sector does not contain the extension descriptor')
        original, extended, current = read('9I', metadata+8), read('9I', metadata+44), read('9I', root+8)
        selection = 'extended' if vblank == 4000 else 'original'
        if current != (extended if selection == 'extended' else original):
            raise ValueError(f'{vblank}: actual render-list selection did not change as requested')
        if camera != result['pose'] or list(read('2I', record+8)) != [6, 17]:
            raise ValueError(f'{vblank}: unsuitable camera/sector for the billboard comparison')
        billboards = []
        for k in range(8):
            for address in range(current[k], current[k+1], 68):
                ident, resource = read('HH', address)
                if 4050 <= ident & 0x7FFF <= 4059:
                    billboards.append(dict(id=ident & 0x7FFF, resource=resource))
        if len(billboards) != (10 if selection == 'extended' else 0) or any(not b['resource'] for b in billboards):
            raise ValueError(f'{vblank}: billboard visibility does not match the requested LOD')
        mismatches = []
        if selection == 'extended':
            for k in range(8):
                extra = extended[k]
                for stock in range(original[k], original[k+1], 68):
                    ident = read('H', stock)[0] & 0x7FFF
                    while extra < extended[k+1] and read('H', extra)[0] & 0x7FFF < ident:
                        extra += 68
                    if extra >= extended[k+1] or read('H', extra)[0] & 0x7FFF != ident:
                        raise ValueError('An existing instance disappeared from the extended list')
                    if read('H', stock)[0] != read('H', extra)[0]:
                        mismatches.append(ident)
                    extra += 68
            if mismatches:
                raise ValueError(f'Existing visibility flags diverged: {mismatches[:10]}')
        lod = read('f', 0x08BC7E30+0x7A8)[0]
        if lod != (3 if selection == 'extended' else 1):
            raise ValueError('Camera LOD does not match the requested value')
        frames.append(dict(vblank=vblank, camera=camera, sector=[6,17], selection=selection,
                           billboards=billboards, camera_lod=lod, existing_flag_mismatches=mismatches))
    log = (report/'game.log').read_text()
    if any(f'[graphics-test] applied at vblank={n}' not in log for n in (3600,4200)):
        raise ValueError('Both live Apply transitions must complete')
    samples, after_pose = [], False
    for line in log.splitlines():
        if 'vblank=2881 ' in line:
            after_pose = True
        match = re.search(r'\[game-fps\] fps=([\d.]+)', line)
        if after_pose and match:
            samples.append(float(match[1]))
    result.update(snapshots=frames, steady_fps_samples=samples,
                  steady_fps_mean=statistics.mean(samples) if samples else None)
    (report/'verification.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    print(json.dumps(verify(sys.argv[1]), indent=2))
