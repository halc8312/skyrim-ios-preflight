"""Real bounded lifecycle admission and pinned native submission boundaries."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('mvk_lifecycle',ROOT/'ios/dependencies/patch_mvk_lifecycle.py')
patch=importlib.util.module_from_spec(spec);spec.loader.exec_module(patch)


class GpuGateTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform=='darwin','Real Metal fixture requires the next approved Mac run')
    def test_real_metal_two_queues_schedule_and_preserve_bytes(self):
        with tempfile.TemporaryDirectory(prefix='gpu-metal-gate-') as tmp:
            binary=Path(tmp)/'gate';gate_obj=Path(tmp)/'gate.o'
            include=str(ROOT/'ios/runtime-port')
            subprocess.run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',include,
                '-c',str(ROOT/'ios/runtime-port/gpu_gate.c'),'-o',str(gate_obj)],check=True,timeout=60)
            subprocess.run(['clang','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fobjc-arc','-I',include,
                str(ROOT/'ios/runtime-port/gpu_gate_metal.m'),str(ROOT/'tests/native/test_gpu_gate_metal.m'),
                str(gate_obj),'-framework','Metal','-framework','Foundation','-framework','CoreFoundation',
                '-o',str(binary)],check=True,timeout=60)
            result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('two_queues_exact_bytes=PASS',result.stdout)
            print(result.stdout.strip())

    def test_real_gate_concurrent_pause_enqueue_capacity_lifetimes(self):
        cc=shutil.which(os.environ.get('CC','cc'))
        if not cc:self.skipTest('C compiler required; run on WSL/Mac')
        with tempfile.TemporaryDirectory(prefix='gpu-gate-') as tmp:
            binary=Path(tmp)/'gate'
            command=[cc,'-std=c11','-O2','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-pthread',
                '-I',str(ROOT/'ios/runtime-port'),str(ROOT/'ios/runtime-port/gpu_gate.c'),
                str(ROOT/'tests/native/test_gpu_gate.c'),'-o',str(binary)]
            if os.environ.get('SKYRIM_TEST_SANITIZERS')=='1':command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(command,check=True,timeout=60)
            result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('concurrent8000=PASS',result.stdout)
            print(result.stdout.strip())

    def test_all_native_commit_sites_and_early_enqueue_are_pinned(self):
        source=os.environ.get('SKYRIM_MVK_LIFECYCLE_SOURCE')
        if not source:
            source=ROOT/'local/lifecycle-20261114/MoltenVK-458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK'
        source=Path(source)
        if not source.is_dir():self.skipTest('Pinned source needed; mandatory path supplied in Mac dependency gate')
        with tempfile.TemporaryDirectory(prefix='mvk-gate-') as tmp:
            stage=Path(tmp)
            for name in patch.PINS:
                p=stage/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((source/name).read_bytes())
            patch.apply(stage,source)
            first={n:(stage/n).read_bytes() for n in patch.PINS}
            actual=os.environ.get('SKYRIM_MVK_LIFECYCLE_PATCHED_SOURCE')
            if actual:
                self.assertEqual(first,{n:(Path(actual)/n).read_bytes() for n in patch.PINS})
            patch.apply(stage,source)
            self.assertEqual(first,{n:(stage/n).read_bytes() for n in patch.PINS})
            for name,count in patch.COUNTS.items():
                raw=(source/name).read_bytes();new=first[name]
                self.assertEqual(new.count(b'skyrim_gpu_commit_metal((__bridge void *)'),count)
                # Removing just the hook and the scopes must recover every byte,
                # including all errors, semaphores, enqueue and completion paths.
                import re
                restored=new.removeprefix(patch.DECL).removeprefix(patch.SCOPE)
                restored=re.sub(rb'skyrim_gpu_commit_metal\(\(__bridge void \*\)([\w.]+)\);',lambda m:b'['+m[1]+b' commit];',restored)
                if name == 'GPUObjects/MVKQueue.mm':
                    # Keep exactly one consuming CPU wait per submission and
                    # preserve native-event encoding at its original position.
                    self.assertEqual(new.count(patch.QUEUE_PREFLIGHT), 1)
                    self.assertEqual(new.count(patch.PRESENT_PREFLIGHT), 1)
                    restored=restored.replace(patch.QUEUE_PREFLIGHT,b'')
                    restored=restored.replace(patch.PRESENT_PREFLIGHT,b'')
                    native=b'\t\tws.encodeWait(mtlCmdBuff);\t// Encoded semaphore waits'
                    self.assertEqual(restored.count(native),1)
                    restored=restored.replace(native,native+patch.PRESENT_INLINE_WAIT,1)
                self.assertEqual(restored,raw)
            target=stage/'GPUObjects/MVKQueue.mm';target.write_bytes(target.read_bytes()+b'\n// drift')
            with self.assertRaisesRegex(ValueError,'Unreviewed'):patch.apply(stage,source)


if __name__=='__main__':unittest.main()
