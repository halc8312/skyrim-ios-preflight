"""Unlinked borrowed-payload experiment; not a guest integration test."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(os.name=='posix' and shutil.which('cc'),'POSIX C compiler required')
class DdsViewTests(unittest.TestCase):
    def test_authored_contiguous_and_borrowed_mips_are_identical(self):
        with tempfile.TemporaryDirectory(prefix='dds-view-') as tmp:
            binary=Path(tmp)/'view'
            flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS')=='1' else []
            subprocess.run(['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Werror',*flags,
                '-I',str(ROOT/'vendor/skydream/runtime'),'-I',str(ROOT/'experiments/dds-view'),
                str(ROOT/'tests/native/test_dds_view.c'),str(ROOT/'vendor/skydream/runtime/d3dx_image.c'),
                '-lm','-o',str(binary)],check=True,timeout=60)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('production_enabled=false',result.stdout)
            print(result.stdout.strip())

if __name__=='__main__':unittest.main()
