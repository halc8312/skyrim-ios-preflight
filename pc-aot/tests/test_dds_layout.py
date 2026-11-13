"""Actual public DDS level calculations with authored metadata, no texture data."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dds_stage', ROOT/'ios/runtime-port/prepare_runtime.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def fixture(folder, measured=False):
    original=(ROOT/'vendor/skydream/runtime/d3dx_image.c').read_text(encoding='utf-8')
    assert hashlib.sha256(original.encode()).hexdigest()==adapter.EXPECTED['d3dx_image.c']
    functions=[]
    for label,text in [('old',original),('new',adapter.patch_dds_layout(original))]:
        body=text[text.index('int native_d3dx_dds_level('):text.index('uint8_t *native_d3dx_decode_rgba(')]
        body=body.replace('native_d3dx_dds_level',label+('_original_level' if label=='new' else '_level'))
        body=body.replace('skyrim_ios_dds_cube_level','new_level')
        if measured:body=body.replace('uint32_t w =',f'++{label}_steps; uint32_t w =')
        functions.append(body)
    source=(ROOT/'tests/native/test_dds_layout.c').read_text().replace('/* ACTUAL_DDS */','\n'.join(functions))
    path=folder/'dds.c';path.write_text(source)
    flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS')=='1' else []
    result=subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-g','-Wall','-Wextra','-Werror',*flags,
                           '-I',str(ROOT/'vendor/skydream/runtime'),str(path),'-o',str(folder/'dds')],
                          capture_output=True,text=True,timeout=60)
    if result.returncode:raise AssertionError(result.stderr[-8000:])
    return folder/'dds'


@unittest.skipUnless(os.name=='posix' and shutil.which('cc'),'Existing POSIX C compiler required')
class DdsLayoutTests(unittest.TestCase):
    def test_original_entry_and_2d_volume_callers_are_unchanged(self):
        original=(ROOT/'vendor/skydream/runtime/d3dx_image.c').read_text()
        expected=original[original.index('int native_d3dx_dds_level('):original.index('uint8_t *native_d3dx_decode_rgba(')]
        patched=adapter.patch_dds_layout(original)
        self.assertEqual(patched[patched.index('int native_d3dx_dds_level('):patched.index('int skyrim_ios_dds_cube_level(')],expected)
        native=(ROOT/'vendor/skydream/runtime/d3dx_native.cpp').read_text()
        staged=adapter.patch_native_textures(native)
        self.assertEqual(staged.count('skyrim_ios_dds_cube_level(&i, bytes, face, l,'),1)
        self.assertEqual(staged.count('native_d3dx_dds_level(&i, bytes, 0, l,'),2)

    def test_actual_offsets_truncation_failure_outputs_and_face_work(self):
        with tempfile.TemporaryDirectory(prefix='dds-layout-') as tmp:
            binary=fixture(Path(tmp),measured=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr[-8000:])
            self.assertIn('DDS_LAYOUT passed',result.stdout)


if __name__=='__main__':unittest.main()
