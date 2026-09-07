#!/usr/bin/env python3
"""A packaged executable must never be published before its world cache."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_run

class PackagingTests(unittest.TestCase):
    def test_cache_readiness_and_failed_preparation_preserve_previous_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            build=root/'build'
            source=build/'bin/Release/VCSNative'
            source.parent.mkdir(parents=True);source.write_bytes(b'new game')
            (root/'config').mkdir();(root/'assets/game').mkdir(parents=True)
            (root/'assets/data').mkdir();(root/'assets/data/VCSProject2DFX_Lights.bin').write_bytes(b'lights')
            for name in ('VCSNative.sdl.ini','ProperShaders.sdl.ini'):
                (root/'config'/name).write_text('template')
            (root/'userdata').mkdir();(root/'userdata/VCSNative.ini').write_text('user preferences')
            target=root/('run/VCSNative.app/Contents/MacOS/VCSNative' if sys.platform=='darwin' else 'run/VCSNative')
            target.parent.mkdir(parents=True);target.write_bytes(b'old game')
            def fail(*args):
                self.assertEqual(target.read_bytes(),b'old game')
                raise RuntimeError('interrupted world-cache preparation')
            def ready(source_path,destination):
                self.assertEqual(target.read_bytes(),b'old game')
                destination.mkdir(parents=True)
                for name in ('MAINLA.wld','BEACH.wld'):(destination/name).write_bytes(b'cache')
                return destination
            original_replace=Path.replace
            def publish(path,destination):
                self.assertTrue((target.parent/'NativeWorld/MAINLA.wld').is_file())
                self.assertTrue((target.parent/'NativeWorld/BEACH.wld').is_file())
                self.assertTrue((target.parent/'PSP_DATA').is_dir())
                self.assertEqual((target.parent/'VCSNative.ini').read_text(),'user preferences')
                return original_replace(path,destination)
            with patch.object(prepare_run,'ROOT',root),patch.object(sys,'argv',['prepare_run.py',str(build)]):
                with patch.object(prepare_run,'prepare_world_catalog',side_effect=fail):
                    with self.assertRaisesRegex(RuntimeError,'interrupted'):prepare_run.main()
                self.assertEqual(target.read_bytes(),b'old game')
                with patch.object(prepare_run,'prepare_world_catalog',side_effect=ready),patch.object(Path,'replace',publish):
                    prepare_run.main()
                self.assertEqual(target.read_bytes(),b'new game')
                self.assertEqual((root/'userdata/VCSNative.ini').read_text(),'user preferences')

if __name__=='__main__':unittest.main()
