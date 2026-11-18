"""Actual public waits with authored handles; no game or hardware performance claim."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('waiter_stage',ROOT/'ios/runtime-port/prepare_runtime.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)

class WaiterMembershipTests(unittest.TestCase):
    def test_adapter_only_adds_membership_and_empty_fast_path(self):
        old=(ROOT/'vendor/skydream/runtime/kernel32.c').read_text()
        self.assertEqual(hashlib.sha256(old.encode()).hexdigest(),adapter.EXPECTED['kernel32.c'])
        new=adapter.patch_waiter_membership(old)
        restored=new.replace('    uint32_t waiting_entries; /* protected by g_wlock */\n','',1)
        restored=restored.replace('    if (!w->waiting_entries) return;\n','',1)
        for op in ['++','--']:
            restored=restored.replace('        for (uint32_t k = 0; k < n; ++k)\n'
                f'            if (w[k]->waiting_entries != UINT32_MAX) {op}w[k]->waiting_entries;\n','',1)
        self.assertEqual(restored,old)
        with self.assertRaises(ValueError):adapter.patch_waiter_membership(new)
        # Both allocations zero the new field and neither object crosses the CPU ABI.
        self.assertIn('Waitable *w = calloc(1, sizeof *w);',new)
        self.assertIn('GThread *t = calloc(1, sizeof *t);',new)

    def test_actual_waits_and_signal_targets_preserve_results_without_unrelated_scans(self):
        cc=shutil.which(os.environ.get('CC','cc'))
        if not cc:self.skipTest('POSIX C compiler required; run on WSL/Mac')
        old=(ROOT/'vendor/skydream/runtime/kernel32.c').read_text()
        template=(ROOT/'tests/native/test_waiter_membership.c').read_text()
        with tempfile.TemporaryDirectory(prefix='waiter-membership-') as tmp:
            work=Path(tmp)
            for version,text in enumerate([old,adapter.patch_waiter_membership(old)]):
                body=text[text.index('struct HObj {'):text.index('/* ===================== threads / TEB')]
                self.assertEqual(body.count('if (t->objs[i] == w)'),1)
                body=body.replace('if (t->objs[i] == w)','if ((++comparisons, t->objs[i] == w))',1)
                source=work/f'wait{version}.c';source.write_text(template.replace('/* ACTUAL_HANDLES_AND_WAITS */',body))
                binary=work/f'wait{version}'
                flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS')=='1' else []
                compiled=subprocess.run([cc,'-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-Wall','-Wextra',
                    '-Werror','-pthread',*flags,f'-DFIXTURE_NEW={version}',str(source),'-o',str(binary)],
                    capture_output=True,text=True,timeout=60)
                self.assertEqual(compiled.returncode,0,compiled.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=25)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn('handshakes=1600',result.stdout)
                print(f'WAIT_MEMBERSHIP version={version}\n{result.stdout}',end='')

if __name__=='__main__':unittest.main()
