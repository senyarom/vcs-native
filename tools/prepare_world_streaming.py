#!/usr/bin/env python3
"""Build a reversible static-world extension from the user's original VCS dump.

No game bytes are stored in source control. Only MAINLA.LVZ/IMG are derived;
other game files are relative symlinks. The guest owns/fixes/frees sector data
normally. A 96-byte descriptor preceding each new overlay table lets the host
select the original or extended render lists without reloading the sector.
"""
from pathlib import Path
import hashlib
import json
import math
import os
import struct
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
RULES = ROOT / 'graphics/world_streaming/airport_billboards.json'
MAGIC = b'VCSBILL1'
META_SIZE = 96


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def origin(x, y):
    return (-2400 + 62.5 + 125*x - (y & 1)*62.5, -2000 + 54.125 + 108.25*y)


def align(data, remainder=0):
    while len(data) % 16 != remainder % 16:
        data.append(0)


class World:
    def __init__(self, lvz, img):
        self.lvz = zlib.decompress(lvz)
        self.img = img
        self.rows = [struct.unpack_from('<II', self.lvz, 36+8*i) for i in range(37)]

    def sector(self, x, y):
        start, x0 = self.rows[y]
        stop = self.rows[y+1][0]
        pos = start + (x-x0)*32
        if not start <= pos < stop:
            return None
        h = list(struct.unpack_from('<8I', self.lvz, pos))
        if h[0] != 0x57524C44 or h[2] < 32 or h[4]+h[5]*4 > h[2]:
            raise ValueError('Invalid WRLD chunk')
        raw = self.img[h[6]:h[6]+h[2]-32]
        if len(raw) != h[2]-32:
            raise ValueError('Truncated sector')
        passes = struct.unpack_from('<9I', raw, 8)
        rp, count = struct.unpack_from('<IH', raw)
        overlays = dict(struct.unpack_from('<II', raw, rp-32+8*i) for i in range(count))
        instances = []
        for k in range(8):
            if (passes[k+1]-passes[k]) % 68 or not 32 <= passes[k] <= passes[k+1] <= h[3]:
                raise ValueError('Invalid instance list')
            for p in range(passes[k]-32, passes[k+1]-32, 68):
                ident, resource = struct.unpack_from('<HH', raw, p)
                instances.append(dict(id=ident & 32767, resource=resource, pass_id=k,
                                      bounds=struct.unpack_from('<4e', raw, p+4), data=raw[p:p+68]))
        return dict(x=x, y=y, position=pos, header=h, raw=raw, passes=passes,
                    overlays=overlays, instances=instances,
                    relocs=struct.unpack_from('<%dI' % h[5], raw, h[4]-32))


def import_resource(raw, relocs, source, resource):
    start = source['overlays'][resource]
    end = min(p for p in [*source['overlays'].values(), *source['passes'], source['header'][3]] if p > start)
    # GE texture/CLUT addresses discard low alignment bits. Preserve alignment
    # of every embedded pointer, not just alignment of the resource header.
    align(raw, start-32)
    delta = len(raw)+32-start
    result = start+delta
    raw.extend(source['raw'][start-32:end-32])
    for field in source['relocs']:
        if start <= field < end:
            target = u32(source['raw'], field-32)
            if not start <= target < end:
                raise ValueError(f'Resource {resource} has an external pointer')
            struct.pack_into('<I', raw, field-32+delta, target+delta)
            relocs.append(field+delta)
    return result


def rebase_instance(instance, donor, sector):
    data = bytearray(instance['data'])
    old = origin(donor['x'], donor['y'])
    new = origin(sector['x'], sector['y'])
    for off, shift in zip((52, 56), (old[0]-new[0], old[1]-new[1])):
        word = u32(data, off)
        value = struct.unpack('<f', struct.pack('<I', (word & 0xFFFFFF) << 8))[0] + shift
        bits = struct.unpack('<I', struct.pack('<f', value))[0]
        struct.pack_into('<I', data, off, (word & 0xFF000000) | (bits >> 8))
    return data


def extend_sector(world, sector, donor, additions, rules):
    h = sector['header'].copy()
    raw = bytearray(sector['raw'][:h[3]-32])
    relocs = list(sector['relocs'])
    overlays = dict(sector['overlays'])
    for key, xy in rules['resources'].items():
        rid = int(key)
        if rid not in overlays:
            overlays[rid] = import_resource(raw, relocs, world.sector(*xy), rid)
    align(raw)
    metadata = len(raw)+32
    raw.extend(bytes(META_SIZE))
    struct.pack_into('<IH', raw, 0, len(raw)+32, len(overlays))
    for rid, ptr in sorted(overlays.items()):
        relocs.append(len(raw)+36)
        raw.extend(struct.pack('<II', rid, ptr))
    align(raw)
    passes, added_records = [], []
    for k in range(8):
        passes.append(len(raw)+32)
        entries = [(i['id'], i['data'], None) for i in sector['instances'] if i['pass_id'] == k]
        entries += [(i['id'], rebase_instance(i, donor, sector), i) for i in additions if i['pass_id'] == k]
        for _, data, extra in sorted(entries, key=lambda entry: entry[0]):
            if extra:
                added_records.append((len(raw)+32, extra['resource'], *extra['bounds'][:3]))
            raw.extend(data)
    passes.append(len(raw)+32)
    # Boot with the original lists; host enables additions on the game thread.
    # Both immutable sets keep PSP merge-by-ID and all original swap flags intact.
    added_table = len(raw)+32
    for ptr, resource, x, y, z in added_records:
        relocs.append(len(raw)+32)
        raw.extend(struct.pack('<IIfff', ptr, resource, x, y, z))
    struct.pack_into('<8s9I9IIIff', raw, metadata-32, MAGIC, *sector['passes'], *passes,
                     len(added_records), added_table, rules['base_range'], rules['maximum_lod'])
    relocs += [metadata+8+4*i for i in range(18)] + [metadata+84]
    align(raw)
    h[3] = h[4] = len(raw)+32
    h[5] = len(relocs)
    raw.extend(struct.pack('<%dI' % len(relocs), *relocs))
    h[2] = len(raw)+32
    validate_chunk(h, raw)
    return h, raw


def validate_chunk(header, raw):
    if len(raw)+32 != header[2] or header[4]+4*header[5] != header[2]:
        raise ValueError('Inconsistent rebuilt chunk size')
    relocs = struct.unpack_from('<%dI' % header[5], raw, header[4]-32)
    if len(set(relocs)) != len(relocs):
        raise ValueError('A relocation was added twice')
    for field in relocs:
        if not 32 <= field <= header[3]-4 or not 32 <= u32(raw, field-32) <= header[3]:
            raise ValueError('Relocation escapes rebuilt chunk')


def rebuild(lvz, img, rules):
    world = World(lvz, img)
    donor = world.sector(*rules['instance_sector'])
    candidates = [i for i in donor['instances'] if i['id'] in rules['instances']]
    if len(candidates) != len(rules['instances']):
        raise ValueError('Missing source billboard instance')
    output_img = bytearray(img)
    output_lvz = bytearray(world.lvz)
    report = []
    maximum = rules['base_range']*rules['maximum_lod']
    for y in range(36):
        start, x0 = world.rows[y]
        stop = world.rows[y+1][0]
        for x in range(x0, x0+(stop-start)//32):
            ox, oy = origin(x, y)
            # Conservative cell margin covers the staggered sector transitions.
            if all(math.hypot(i['bounds'][0]-ox, i['bounds'][1]-oy) > maximum+160 for i in candidates):
                continue
            sector = world.sector(x, y)
            present = {i['id'] for i in sector['instances']}
            additions = [i for i in candidates if i['id'] not in present]
            if not additions:
                continue
            h, raw = extend_sector(world, sector, donor, additions, rules)
            while len(output_img) % 2048:
                output_img.append(0)
            h[6] = len(output_img)
            output_img.extend(raw)
            struct.pack_into('<8I', output_lvz, sector['position'], *h)
            report.append(dict(x=x, y=y, added=[i['id'] for i in additions],
                               original_bytes=sector['header'][2], bytes=h[2]))
    return zlib.compress(output_lvz, 9), output_img, report


def digest(data):
    return hashlib.sha256(data).hexdigest()


def publish_alias(destination, alias):
    # Each game resolves its root to an immutable generation at startup. A new
    # build can therefore publish both LVZ and IMG without mixing generations
    # in an already-running game's file handles.
    if alias.is_dir() and not alias.is_symlink():
        alias.rename(destination.parent / ('legacy-'+str(time.time_ns())))
    temporary = alias.with_name(alias.name+'.new')
    if temporary.is_symlink():
        temporary.unlink()
    temporary.symlink_to(os.path.relpath(destination, alias.parent), target_is_directory=True)
    temporary.replace(alias)


def prepare(source, destination):
    source, alias = Path(source).resolve(), Path(destination).absolute()
    if source == alias or source in alias.parents:
        raise ValueError('Derived world cache must be outside the original game dump')
    rules = json.loads(RULES.read_text())
    rundata = source / 'PSP_GAME/USRDIR/RUNDATA'
    inputs = {name: (rundata/name).read_bytes() for name in rules['source_sha256']}
    for name, data in inputs.items():
        if digest(data) != rules['source_sha256'][name]:
            raise ValueError(f'{name}: unsupported or modified game data; original files were not changed')
    signature = digest(Path(__file__).read_bytes()+RULES.read_bytes()+str(source).encode())
    destination = alias.parent/'world-cache'/signature[:24]
    manifest = destination / 'world-streaming.json'
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        if previous.get('signature') == signature and previous.get('source') == str(source):
            valid = all((destination/'PSP_GAME/USRDIR/RUNDATA'/n).is_file() and
                        digest((destination/'PSP_GAME/USRDIR/RUNDATA'/n).read_bytes()) == h
                        for n, h in previous['output_sha256'].items())
            if valid:
                publish_alias(destination, alias)
                return destination
    lvz, img, report = rebuild(inputs['MAINLA.LVZ'], inputs['MAINLA.IMG'], rules)
    destination.mkdir(parents=True, exist_ok=True)
    for directory, dirs, files in os.walk(source):
        target = destination / Path(directory).relative_to(source)
        target.mkdir(parents=True, exist_ok=True)
        for name in files:
            original, link = Path(directory)/name, target/name
            if original.parent == rundata and name in inputs:
                continue
            if not link.exists() and not link.is_symlink():
                link.symlink_to(os.path.relpath(original, target))
    out = destination/'PSP_GAME/USRDIR/RUNDATA'
    outputs = {'MAINLA.LVZ': lvz, 'MAINLA.IMG': img}
    for name, data in outputs.items():
        with tempfile.NamedTemporaryFile(dir=out, delete=False) as f:
            f.write(data)
            temp = Path(f.name)
        temp.replace(out/name)
    result = dict(signature=signature, source=str(source), source_sha256=rules['source_sha256'],
                  output_sha256={n: digest(d) for n, d in outputs.items()}, sectors=report)
    temporary = manifest.with_suffix('.new')
    temporary.write_text(json.dumps(result, indent=2)+'\n')
    temporary.replace(manifest)
    publish_alias(destination, alias)
    print(f'World streaming: {len(report)} sectors, {len(img)-len(inputs["MAINLA.IMG"]):,} cache bytes added')
    return destination


if __name__ == '__main__':
    print(prepare(ROOT/'assets/game', ROOT/'run/world-game'))
