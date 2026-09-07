#!/usr/bin/env python3
"""Index the complete original static-world visibility sets and resource payloads.

The resulting private cache is consumed by the native world renderer. Original
WRLD/AREA chunks and PSP streaming ownership remain unchanged.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
from prepare_world_streaming import World, origin, u32, import_resource

MAGIC = b'VCSWLD02'
ROWS = 36


def cell(x, y):
    row = math.floor((y + 2000) / 108.25)
    return math.floor((x + 2400) / 125 + (row & 1) * .5), row


def position(data):
    return [struct.unpack('<f', struct.pack('<I', (u32(data, off) & 0xffffff) << 8))[0]
            for off in (52, 56, 60)]


def index_world(lvz, img):
    world = World(lvz, img)
    sources, variants = {}, {}
    area_count, area_table = struct.unpack_from('<II', world.lvz, 752)
    for i in range(area_count):
        _, _, offset, size, _ = struct.unpack_from('<hhIII', world.lvz, area_table + 16*i)
        data = world.img[offset:offset+size]
        h = struct.unpack_from('<8I', data)
        if h[0] != 0x41524541 or len(data) != h[2]:
            raise ValueError('Invalid AREA chunk')
        count, ptr = struct.unpack_from('<II', data, 32)
        entries = [struct.unpack_from('<HHI', data, ptr+8*j) for j in range(count)]
        source = dict(header=h, raw=data[32:], overlays={rid: p for rid, _, p in entries},
                      passes=(ptr,), relocs=struct.unpack_from('<%dI' % h[5], data, h[4]))
        for rid, _, p in entries:
            sources.setdefault(rid, source)
    sectors = {}
    for y in range(ROWS):
        start, x0 = world.rows[y]
        stop = world.rows[y+1][0]
        for x in range(x0, x0 + (stop-start)//32):
            if not 0 <= x < 64:
                raise ValueError('Sector column outside membership bitmap')
            sector = world.sector(x, y)
            sectors[x, y] = sector
            for rid in sector['overlays']:
                sources.setdefault(rid, dict(sector, passes=(*sector['passes'], u32(sector['raw'], 0), u32(sector['raw'], 48))))
            for entry in sector['instances']:
                key = entry['pass_id'], entry['id'], entry['resource']
                if key not in variants:
                    center = position(entry['data'])
                    center[0] += origin(x, y)[0]
                    center[1] += origin(x, y)[1]
                    variants[key] = dict(**entry, center=center, donor=sector, rows=[0]*ROWS)
                variants[key]['rows'][y] |= 1 << x
    return world, sources, list(variants.values()), sectors


def build(lvz, img):
    world, sources, variants, sectors = index_world(lvz, img)
    count = u32(world.lvz, 332)
    # The dummy 32-byte prefix matches the relocation convention of original
    # chunks; all cache pointers are offsets from the start of the payload.
    blob = bytearray(32)
    relocs, pointers = [], [0]*count
    for rid, source in sorted(sources.items()):
        if not 0 <= rid < count:
            raise ValueError('Resource ID out of range')
    # import_resource expects a body without the virtual 32-byte header.
    # Keep one body throughout to avoid copying the entire island per item.
    body = bytearray()
    for rid, source in sorted(sources.items()):
        pointers[rid] = import_resource(body, relocs, source, rid)
    blob.extend(body)
    if len(blob) > 24*1024*1024:
        raise ValueError('World payload exceeds reserved native resource arena')
    resource_table = u32(world.lvz, 32)
    geometry = {v['resource'] for v in variants if v['resource']}
    missing, dependencies = [], {}
    for rid in geometry:
        if pointers[rid]:
            data, off = blob, pointers[rid]
        elif (off := u32(world.lvz, resource_table+12*rid)):
            data = world.lvz
        else:
            missing.append(rid)
            continue
        n = u32(data, off)
        if n >= 4096 or off+8+24*n > len(data):
            raise ValueError(f'Invalid PSP geometry resource {rid}')
        deps = {struct.unpack_from('<H', data, off+8+24*i)[0] for i in range(n)} - {0}
        if any(d >= count or not (pointers[d] or u32(world.lvz, resource_table+12*d)) for d in deps):
            raise ValueError(f'Unresolved geometry texture dependency {rid}: {deps}')
        dependencies[rid] = sorted(deps)
    # [header: 8s, resource count, blob bytes, reloc count, variant count]
    output = bytearray(struct.pack('<8s4I', MAGIC, count, len(blob), len(relocs), len(variants)))
    output.extend(struct.pack('<%dI' % count, *pointers))
    output.extend(struct.pack('<%dI' % len(relocs), *relocs))
    output.extend(blob)
    for v in sorted(variants, key=lambda v: (v['pass_id'], v['id'], v['resource'])):
        # 384 bytes: pass, donor x/y, xyz, original 68-byte record, 36 uint64 masks.
        output.extend(struct.pack('<Iii3f', v['pass_id'], v['donor']['x'], v['donor']['y'], *v['center']))
        output.extend(v['data'])
        output.extend(struct.pack('<36Q', *v['rows']))
        output.extend(bytes(4))
    report = dict(resources=count, copied_resources=len(sources), resource_bytes=len(blob),
                  variants=len(variants), instance_ids=len({v['id'] for v in variants}),
                  sectors=len(sectors), missing_geometry=sorted(missing),
                  dependencies=sum(map(len, dependencies.values())), cache_bytes=len(output))
    return output, report


def prepare(source, destination):
    source, destination = Path(source).resolve(), Path(destination).absolute()
    destination.mkdir(parents=True, exist_ok=True)
    rundata = source/'PSP_GAME/USRDIR/RUNDATA'
    builder_hash = hashlib.sha256(Path(__file__).read_bytes() +
        Path(__file__).with_name('prepare_world_streaming.py').read_bytes()).hexdigest()
    manifest = destination/'manifest.json'
    old = json.loads(manifest.read_text()) if manifest.is_file() else {}
    reports = {}
    for name in ('MAINLA', 'BEACH'):
        inputs = [(rundata/(name+suffix)).read_bytes() for suffix in ('.LVZ', '.IMG')]
        target = destination/(name+'.wld')
        hashes = [hashlib.sha256(b).hexdigest() for b in inputs]
        previous = old.get(name, {})
        if (previous.get('source_sha256') == hashes and previous.get('builder_sha256') == builder_hash
                and target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == previous.get('sha256')):
            reports[name] = previous
            continue
        data, report = build(*inputs)
        temp = target.with_suffix('.new')
        temp.write_bytes(data)
        temp.replace(target)
        report['source_sha256'] = hashes
        report['builder_sha256'] = builder_hash
        report['sha256'] = hashlib.sha256(data).hexdigest()
        reports[name] = report
        print(name, json.dumps(report))
    (destination/'manifest.json').write_text(json.dumps(reports, indent=2)+'\n')
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    prepare(args.source, args.destination)
