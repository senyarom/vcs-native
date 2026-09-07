#!/usr/bin/env python3
"""Verify the native visibility index against every original exterior sector."""
from pathlib import Path
import math
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_world_catalog import index_world, position, cell, origin

RUNDATA = Path(__file__).resolve().parents[1]/'assets/game/PSP_GAME/USRDIR/RUNDATA'

class WorldCatalogTests(unittest.TestCase):
    @unittest.skipUnless((RUNDATA/'MAINLA.IMG').is_file(), 'requires private original game data')
    def test_all_sectors_keep_membership_and_transforms(self):
        for name in ('MAINLA', 'BEACH'):
            _, _, variants, sectors = index_world((RUNDATA/(name+'.LVZ')).read_bytes(),
                                                   (RUNDATA/(name+'.IMG')).read_bytes())
            keyed = {(v['pass_id'], v['id'], v['resource']): v for v in variants}
            for (x,y), sector in sectors.items():
                # At 1x, the indexed visibility must equal the source, including
                # different mesh components sharing the same instance ID/pass.
                original = {(v['pass_id'],v['id'],v['resource']) for v in sector['instances']}
                selected = {k for k,v in keyed.items() if v['rows'][y] & (1<<x)}
                self.assertEqual(original, selected, (name,x,y))
                for v in sector['instances']:
                    center = position(v['data'])
                    ox,oy = origin(x,y)
                    center[0] += ox;center[1] += oy
                    donor = keyed[v['pass_id'],v['id'],v['resource']]
                    for a,b in zip(center, donor['center']):
                        self.assertAlmostEqual(a,b,delta=.08) # GE float24 quantization
            if name == 'MAINLA':
                def select(lod):
                    result = set()
                    for v in variants:
                        cx,cy,_ = v['center']
                        x,y = cell(cx+(-1700-cx)/lod,cy+(-130-cy)/lod)
                        if 0<=y<36 and 0<=x<64 and v['rows'][y] & (1<<x):
                            result.add(v['id'])
                    return result
                coarse, fine = select(1), select(3)
                for old,new in [(5835,4080),(5827,4088),(5859,3969)]:
                    self.assertIn(old,coarse);self.assertNotIn(new,coarse)
                    self.assertNotIn(old,fine);self.assertIn(new,fine)

if __name__ == '__main__':unittest.main()
