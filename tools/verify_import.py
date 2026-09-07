#!/usr/bin/env python3
"""Verify imported game/code bytes and all replacement-image assignments."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]


def main():
    manifest = json.loads(
        (ROOT / 'tools/import_data/ulus10160-1.03-assets.json').read_text())
    checked = 0
    for entry in manifest['files']:
        relative = entry['path']
        path = ROOT / 'assets/game' / relative
        if not path.exists():
            raise SystemExit(f'Runtime game asset is missing: {relative}')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != entry['sha256']:
            raise SystemExit(f'Game asset changed: {relative}')
        checked += 1
    pack = ROOT / 'textures/hdtextures'
    for line in (pack / 'checksums.sha256').read_text().splitlines():
        digest, relative = line.split('  ', 1)
        if hashlib.sha256((pack / relative).read_bytes()).hexdigest() != digest:
            raise SystemExit(f'Texture checksum mismatch: {relative}')
    section = ''
    keys = set()
    for line in (pack / 'textures.ini').read_text().splitlines():
        line = line.split('#', 1)[0].split(';', 1)[0].strip()
        if line.startswith('['):
            section = line
        elif section == '[hashes]' and '=' in line:
            key, relative = map(str.strip, line.split('=', 1))
            if key in keys or not (pack / relative).is_file():
                raise SystemExit(f'Duplicate or missing texture assignment: {line}')
            keys.add(key)
    print(f'Verified {checked} game assets, {len(keys)} texture assignments, '
          f'{len(list(pack.rglob("*.png")))} PNG files.')


if __name__ == '__main__':
    main()
