"""Five public host work reductions, actual bodies with authored inputs only."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('additional_stage', ROOT/'ios/runtime-port/prepare_runtime.py')
adapter = importlib.util.module_from_spec(spec); spec.loader.exec_module(adapter)


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def source(kind, prepared=None):
    names = {'ctab':'d3dx_shader.cpp', 'route':'xaudio2.c', 'surface':'d3dx_surface.cpp',
             'bc1':'d3d9_bcemu.cpp', 'uploads':'d3d9_bridge.cpp'}
    name = names[kind]
    original = (ROOT/'vendor/skydream/runtime'/name).read_text(encoding='utf-8')
    assert hashlib.sha256(original.encode()).hexdigest() == adapter.EXPECTED[name]
    text = original if prepared is None else prepared[name]
    if kind == 'ctab':
        body = section(text, 'struct Constant {', 'static uint32_t buffer_vt')
        body += section(text, 'static uint32_t make_table(', 'struct Includes {')
    elif kind == 'route':
        body = section(text, '#define PASS_FRAMES_MS', 'static HComClass')
        body += '\nstatic Voice *g_master;\n' + section(text, 'static void default_matrix(', 'static void mix_source(')
    elif kind == 'surface':
        bc = (ROOT/'vendor/skydream/runtime/d3d9_bcemu.cpp').read_text(encoding='utf-8')
        body = section(bc, 'void rgb565(', '// Decode rect `r`')
        body += '\nvoid native_d3dx_decode_block(D3DFORMAT f,const uint8_t*b,uint8_t*out,int pitch,int w,int h){decode_block(f,b,out,pitch,w,h);}\n'
        body += text.replace('#include "d3d9_bridge.h"', '')
    elif kind == 'bc1':
        body = section(text, 'void rgb565(', '// Decode rect `r`')
    else:
        body = '// Guest code cannot address host memory,' + text.split('// Guest code cannot address host memory,',1)[1].split('\nnamespace {\nbool is_block_format',1)[0]
    extension = 'c' if kind == 'route' else 'cpp'
    template = ROOT/f'tests/native/test_{kind}_work.{extension}'
    return template.read_text(encoding='utf-8').replace('/* ACTUAL_BODY */', body)


def compile_fixture(folder, kind, prepared=None):
    extension = 'c' if kind == 'route' else 'cpp'
    path=folder/f'{kind}.{extension}'; path.write_text(source(kind, prepared), encoding='utf-8')
    compiler, std = ('cc','c11') if extension == 'c' else ('c++','c++17')
    flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS')=='1' else []
    command=[compiler,f'-std={std}','-D_POSIX_C_SOURCE=200809L','-O2','-g','-fno-strict-aliasing',
             '-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-mismatched-new-delete','-pthread',*flags]
    if extension=='c': command.remove('-Wno-mismatched-new-delete')
    result=subprocess.run(command+[str(path),'-o',str(folder/kind)],capture_output=True,text=True,timeout=60)
    if result.returncode: raise AssertionError(result.stderr[-10000:])
    return folder/kind


@unittest.skipUnless(os.name=='posix' and shutil.which('c++'),'Existing POSIX C/C++ compilers required')
class AdditionalWorkTests(unittest.TestCase):
    def compare(self, kind):
        outputs=[]
        for prepared in (None, adapter.prepare(ROOT/'vendor/skydream')):
            with tempfile.TemporaryDirectory(prefix=f'{kind}-work-') as tmp:
                binary=compile_fixture(Path(tmp),kind,prepared)
                result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=60)
                self.assertEqual(result.returncode,0,result.stderr[-10000:]); outputs.append(json.loads(result.stdout))
        for key in ('cases','digest'):
            self.assertEqual(outputs[0][key],outputs[1][key],kind+' '+key)
        return outputs
    def test_ctab(self):
        old,new=self.compare('ctab'); self.assertLess(new['allocations'],old['allocations'])
    def test_route(self):
        old,new=self.compare('route'); self.assertEqual(old['nans'],new['nans'])
    def test_surface(self): self.compare('surface')
    def test_bc1(self): self.compare('bc1')
    def test_uploads(self):
        old,new=self.compare('uploads'); self.assertLess(new['allocations'],old['allocations'])
    def test_route_real_engine_pcm_callbacks_and_lifetime(self):
        from tests import test_audio_submix as audio
        original=(ROOT/'vendor/skydream/runtime/xaudio2.c').read_text(encoding='utf-8')
        baseline=adapter.patch_audio_submix(original)
        outputs=[]
        for text in (baseline,adapter.patch_audio_route(baseline)):
            with tempfile.TemporaryDirectory(prefix='route-engine-') as tmp:
                path=Path(tmp);source=path/'engine.c';source.write_text(audio.fixture(text),encoding='utf-8')
                flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS')=='1' else []
                compiled=subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-g','-pthread',*flags,str(source),'-o',str(path/'engine')],capture_output=True,text=True,timeout=60)
                self.assertEqual(compiled.returncode,0,compiled.stderr[-6000:])
                result=subprocess.run([str(path/'engine')],capture_output=True,text=True,timeout=40)
                self.assertEqual(result.returncode,0,result.stderr[-6000:]);outputs.append(result.stdout)
        self.assertEqual(outputs[0],outputs[1]);self.assertIn('pcm_graph_cases=132',outputs[1])

if __name__=='__main__': unittest.main()
