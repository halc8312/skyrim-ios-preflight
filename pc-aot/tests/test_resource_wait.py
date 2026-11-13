"""Actual public DXVK resource counters/waits; authored GPU completion events."""
import importlib.util
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'vendor/skydream/third_party/dxvk'
spec=importlib.util.spec_from_file_location('resource_adapter',ROOT/'ios/dependencies/prepare_dxvk.py')
adapter=importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def fixture(folder, patched, hooks=True):
    for filename in ('dxvk_resource.h','dxvk_resource.cpp'):
        relative='src/dxvk/'+filename
        text=(SOURCE/relative).read_text(encoding='utf-8')
        assert hashlib.sha256(text.encode()).hexdigest()==adapter.EXPECTED[relative]
        if patched:
            text=adapter.adapt(relative,text)
            if hooks and filename.endswith('.cpp'):
                text=adapter.once(text,'      ResourceWaitRegistration registration(bucket);',
                                  '      ::fixture_wait_hook(0);\n      ResourceWaitRegistration registration(bucket);')
                text=adapter.once(text,'[this, access] { return !isInUse(access); }',
                                  '[this, access] { const bool idle=!isInUse(access); ::fixture_wait_hook(1); return idle; }')
        if filename.endswith('.h'):
            # Keep the entire actual class. Only narrow its umbrella include;
            # Vulkan loaders/logging are unrelated to these resource counters.
            # Complete production units are syntax-checked separately.
            text=adapter.once(text,'#include "dxvk_include.h"',
                              '#include "util/rc/util_rc.h"\n#include "util/util_flags.h"\n#include "util/sync/sync_spinlock.h"')
        (folder/filename).write_text(text,encoding='utf-8')
    text=(ROOT/'tests/native/test_resource_wait.cpp').read_text(encoding='utf-8')
    path=folder/'fixture.cpp'
    path.write_text(text.replace('// ACTUAL_RESOURCE','#include "dxvk_resource.cpp"'),encoding='utf-8')
    return path


def compile_fixture(folder, patched, hooks=True):
    path=fixture(folder,patched,hooks)
    mode=os.getenv('SKYRIM_TEST_SANITIZERS')
    flags=['-fsanitize='+('thread' if mode=='thread' else 'address,undefined'),'-fno-omit-frame-pointer'] if mode in ('1','thread') else []
    command=['c++','-std=c++17','-O2','-g','-pthread','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
             '-include','cstdint','-include','cstring','-DDXVK_NATIVE','-DNOMINMAX',f'-DPATCHED={int(patched)}',*flags]
    for include in ('src','src/dxvk','include','include/native','include/native/windows','include/native/directx'):
        command+=['-isystem',str(SOURCE/include)]
    metrics=folder/'metrics.o'
    compiled=subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2',*flags,'-c',
                             str(ROOT/'ios/runtime-port/performance_metrics.c'),'-o',str(metrics)],
                            capture_output=True,text=True,timeout=45)
    if compiled.returncode:raise AssertionError(compiled.stderr[-8000:])
    result=subprocess.run([*command,'-I',str(ROOT/'ios/runtime-port'),str(path),str(metrics),'-o',str(folder/'fixture')],capture_output=True,text=True,timeout=60)
    if result.returncode:raise AssertionError(result.stderr[-8000:])
    return folder/'fixture'


@unittest.skipUnless(os.name=='posix' and shutil.which('c++'),'Existing POSIX C++ compiler required')
class ResourceWaitTests(unittest.TestCase):
    def test_actual_access_visibility_notification_boundaries_and_address_reuse(self):
        results=[]
        for patched in (False,True):
            with tempfile.TemporaryDirectory(prefix='resource-wait-') as tmp:
                binary=compile_fixture(Path(tmp),patched)
                result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stderr[-8000:])
                self.assertIn('access=PASS visibility=PASS reuse=128',result.stdout)
                self.assertIn(f'boundaries={int(patched)}',result.stdout)
                results.append(result.stdout.split('resource_bytes=')[1])
        self.assertEqual(results[0],results[1])


if __name__=='__main__':unittest.main()
