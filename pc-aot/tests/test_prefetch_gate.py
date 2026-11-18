"""Bounded optional admission, authored clock/thermal inputs and a real waiter."""
import os
import platform
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(os.name=='posix' and shutil.which('c++'), 'POSIX C++ fixture required')
class PrefetchGateTests(unittest.TestCase):
    def test_admission_clock_thermal_stop_and_required_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='prefetch-gate-') as tmp:
            folder=Path(tmp)
            mode=os.getenv('SKYRIM_TEST_SANITIZERS')
            flags=['-fsanitize=thread','-fno-omit-frame-pointer'] if mode=='thread' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='1' else []
            objects=[]
            for name in ('performance_metrics','device_policy'):
                obj=folder/(name+'.o'); objects.append(str(obj))
                subprocess.run(['cc','-std=c11','-O2','-D_POSIX_C_SOURCE=200809L',*flags,
                    '-c',str(ROOT/'ios/runtime-port'/(name+'.c')),'-o',str(obj)],check=True,timeout=45)
            exe=folder/'gate'
            subprocess.run(['c++','-std=c++17','-O2','-g','-Wall','-Wextra','-Werror','-pthread',*flags,
                '-I',str(ROOT/'ios/runtime-port'),'-I',str(ROOT/'ios/dependencies'),
                str(ROOT/'tests/native/test_prefetch_gate.cpp'),*objects,'-o',str(exe)],check=True,timeout=45)
            command=[str(exe)]
            if mode=='thread' and os.getenv('SKYRIM_TSAN_DISABLE_ASLR')=='1':
                setarch=shutil.which('setarch'); self.assertIsNotNone(setarch)
                command=[setarch,platform.machine(),'-R',*command]
            result=subprocess.run(command,capture_output=True,text=True,check=True,timeout=15)
            self.assertIn('prefetch_admission_off_priority_deadline_thermal_stop=PASS',result.stdout)
            self.assertIn('expired=3',result.stderr)
            self.assertIn('disabled=1',result.stderr)
            off=subprocess.run([*command,'off'],capture_output=True,text=True,check=True,timeout=15)
            self.assertEqual(off.stderr,'')
            logged=subprocess.run([*command,'logging'],capture_output=True,text=True,check=True,timeout=15)
            self.assertEqual(logged.stderr.count('IOS_PREFETCH_GATE event=0 '),2)
            self.assertEqual(logged.stderr.count('IOS_PREFETCH_GATE event=1 '),1)
            self.assertEqual(logged.stderr.count('IOS_PREFETCH_GATE event=2 '),1)
            concurrent=subprocess.run([*command,'concurrent-expiry'],capture_output=True,text=True,check=True,timeout=15)
            self.assertIn('concurrent_expirations=8 report_bound=3 PASS',concurrent.stdout)
            self.assertEqual(concurrent.stderr.count('IOS_PREFETCH_GATE event=0 '),0)
            self.assertEqual(concurrent.stderr.count('IOS_PREFETCH_GATE event=1 '),3)
            self.assertEqual(concurrent.stderr.count('IOS_PREFETCH_GATE event=2 '),1)

if __name__=='__main__':unittest.main()
