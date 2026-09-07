#!/usr/bin/env python3
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_world_streaming import import_resource, rebase_instance, origin, validate_chunk

class WorldDataTests(unittest.TestCase):
    def test_texture_pointer_alignment_survives_relocation(self):
        # Header at 4 mod 16, pixel/CLUT address aligned to 16. Rounding the
        # destination header up to 16 instead corrupts GE texture addressing.
        source = dict(raw=bytearray(256), overlays={42:68,43:132}, passes=(200,),
                      header=[0,0,288,256], relocs=(72,))
        struct.pack_into('<I', source['raw'],40,80)
        source['raw'][48:100]=bytes(range(52))
        raw=bytearray(77);relocs=[]
        pointer=import_resource(raw,relocs,source,42)
        pixels=struct.unpack_from('<I',raw,pointer-28)[0]
        self.assertEqual(pixels%16,0)
        self.assertEqual(pointer%16,68%16)
        self.assertEqual(raw[pixels-32:pixels-32+52],bytes(range(52)))
        self.assertEqual(relocs,[pointer+4])
        struct.pack_into('<I',source['raw'],40,220)
        with self.assertRaises(ValueError):import_resource(bytearray(),[],source,42)

    def test_sector_rebase_keeps_world_position_and_command_bits(self):
        data=bytearray(68)
        for off,value in [(52,-47.425),(56,-29.15)]:
            bits=struct.unpack('<I',struct.pack('<f',value))[0]
            struct.pack_into('<I',data,off,0x3C000000|(bits>>8))
        before=bytes(data)
        donor=dict(x=6,y=15);dest=dict(x=5,y=17)
        result=rebase_instance(dict(data=data),donor,dest)
        for axis,off in enumerate((52,56)):
            def value(record):
                word=struct.unpack_from('<I',record,off)[0]
                return struct.unpack('<f',struct.pack('<I',(word&0xffffff)<<8))[0]
            self.assertAlmostEqual(value(data)+origin(6,15)[axis],value(result)+origin(5,17)[axis],delta=.01)
            self.assertEqual(result[off+3],data[off+3])
        self.assertEqual(bytes(data),before)
        self.assertEqual(result[:52],data[:52])

    def test_duplicate_relocation_rejected(self):
        raw=bytearray(40)
        struct.pack_into('<I',raw,0,40)
        struct.pack_into('<II',raw,32,32,32)
        with self.assertRaises(ValueError):validate_chunk([0,0,72,64,64,2],raw)

if __name__=='__main__':unittest.main()
