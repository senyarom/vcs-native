#!/usr/bin/env python3
"""Embed the already relocated game image as portable C++ (also when cross-building)."""
import hashlib
import json
from pathlib import Path
import sys

SOURCE_SHA = 'fee2e86c7fe457ab6da463d9fdc16f156a0900adcdaa2fd260206c11907c5d65'


def main():
    source, output = map(Path, sys.argv[1:])
    meta = json.loads((source / 'metadata.json').read_text())
    data = (source / 'initial-memory.bin').read_bytes()
    if (meta['format_version'] != 1 or meta['source_sha256'] != SOURCE_SHA
            or meta['data_size'] != len(data)
            or meta['image_sha256'] != hashlib.sha256(data).hexdigest()):
        raise SystemExit('Invalid game/bootstrap import; re-export the original ULUS10160 1.03 ELF')
    start, size = meta['load_address'], meta['image_size']
    end = start + size
    if not (0x08804000 == start < end <= 0x0A000000
            and 0 < len(data) <= size
            and start <= meta['entry'] < start + len(data)
            and start <= meta['gp'] < end
            and meta['arena_start'] == ((end + 255) & ~255)):
        raise SystemExit('Invalid game/bootstrap memory layout')
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('w') as f:
        f.write('// Generated from game/bootstrap; do not edit.\n#include "vcs_boot_image.hpp"\n'
                'namespace vcs { namespace {\n')
        # Separate objects stay below MSVC's string size limit. No relocation,
        # decompression or ELF parser is needed by the resulting executable.
        chunks = [data[i:i + 16384] for i in range(0, len(data), 16384)]
        for index, chunk in enumerate(chunks):
            f.write(f'const unsigned char chunk_{index}[] =\n')
            for line in range(0, len(chunk), 32):
                f.write('"' + ''.join(f'\\x{b:02x}' for b in chunk[line:line + 32]) + '"\n')
            f.write(';\n')
        f.write('const BootImageChunk chunks[]{\n')
        for index, chunk in enumerate(chunks):
            f.write(f'    {{chunk_{index}, {len(chunk)}u}},\n')
        f.write('};\nconst BootImage image{\n')
        for key in ('load_address', 'image_size', 'entry', 'gp', 'arena_start'):
            f.write(f'    {meta[key]}u,\n')
        f.write(f'    "{SOURCE_SHA}", chunks\n}};\n}}\n'
                'const BootImage &game_boot_image() noexcept { return image; }\n}\n')


if __name__ == '__main__':
    main()
