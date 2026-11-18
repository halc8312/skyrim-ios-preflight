"""Real mixer-thread loop, authored PCM sink/clock; no hardware audio claim."""
import hashlib
import importlib.util
import itertools
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('audio_queue_stage',ROOT/'ios/runtime-port/prepare_runtime.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)


class AudioBackpressureTests(unittest.TestCase):
    def test_only_queue_delay_and_negotiated_quantum_change(self):
        old=(ROOT/'vendor/skydream/runtime/xaudio2.c').read_text()
        self.assertEqual(hashlib.sha256(old.encode()).hexdigest(),adapter.EXPECTED['xaudio2.c'])
        new=adapter.patch_audio_backpressure(old)
        helper=(ROOT/'ios/runtime-port/audio_backpressure.c.in').read_text()
        restored=new.replace(helper+'\n','',1).replace(
            'usleep(ios_audio_backpressure_delay(q, frames, ios_audio_device_rate));','usleep(1000);',1).replace(
            '        ios_audio_device_samples = have.samples;\n        ios_audio_device_rate = have.freq;\n','',1)
        self.assertEqual(restored,old)
        self.assertLess(new.index('ios_audio_device_samples = have.samples;'),new.index('pthread_create(&t'))
        with self.assertRaises(ValueError):adapter.patch_audio_backpressure(new)

    def test_actual_engine_preserves_packets_reserve_pause_and_recovery_with_fewer_polls(self):
        cc=shutil.which(os.environ.get('CC','cc'))
        if not cc:self.skipTest('C compiler needed; run on WSL/Mac')
        old=(ROOT/'vendor/skydream/runtime/xaudio2.c').read_text()
        template=(ROOT/'tests/native/test_audio_backpressure.c').read_text()
        with tempfile.TemporaryDirectory(prefix='audio-backpressure-') as tmp:
            work=Path(tmp);binaries=[]
            for version,text in enumerate([old,adapter.patch_audio_backpressure(old)]):
                start=text.index('static void *engine_main(void *arg) {')
                end=text.index('static void engine_start_thread(void)',start)
                body=text[start:end]
                self.assertEqual(body.count('for (;;) {'),1)
                body=body.replace('for (;;) {','for (; mix_count < 300;) {',1)
                body=body.replace('    return NULL;','    free(out); return NULL;',1)
                if version:body=(ROOT/'ios/runtime-port/audio_backpressure.c.in').read_text()+body
                source=work/f'loop{version}.c';source.write_text(template.replace('/* ACTUAL_HELPER_AND_ENGINE */',body))
                binary=work/f'loop{version}';binaries.append(binary)
                flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS')=='1' else []
                subprocess.run([cc,'-std=c11','-D_DEFAULT_SOURCE','-O2','-Wall','-Wextra','-Werror',*flags,
                                f'-DFIXTURE_NEW={version}',str(source),'-o',str(binary)],check=True,timeout=60)
            cases=list(itertools.product([22050,44100,48000,96000],[256,512,1024],[0,3700,9900],[0],[100],[0,500,2000],[0]))
            cases += [(48000,1024,3700,scenario,100,500,interrupt) for scenario in [0,1,2,3] for interrupt in [0,7]]
            cases += [(48000,2048,3700,0,100,0,0)] # large negotiated quantum falls back unchanged
            cases += [(48000,1024,3700,0,4000,0,0)] # actual mixer work consumes reserve too
            summary=[]
            for args in cases:
                rows=[]
                for binary in binaries:
                    result=subprocess.run([str(binary),*map(str,args)],capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.returncode,0,(args,result.stderr[-3000:]))
                    rows.append({k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',result.stdout)})
                a,b=rows
                if args[3]!=1:
                    self.assertEqual(a['digest'],b['digest'],args)
                    self.assertEqual(a['packets'],b['packets'],args)
                    self.assertLessEqual(b['underflows'],a['underflows'],args)
                self.assertEqual(a['stall'],b['stall'],args);self.assertEqual(a['resume'],b['resume'],args)
                self.assertEqual(a['clears'],b['clears'],args);self.assertEqual(a['restarts'],b['restarts'],args)
                if args[1] > args[0]*10//1000*3:self.assertEqual(a,b,args)
                if args==(48000,1024,0,0,100,0,0):
                    self.assertEqual(a['underflows'],0);self.assertEqual(b['underflows'],0)
                    self.assertLess(b['sleeps'],a['sleeps']*.5)
                    summary=[a,b]
            print(f'AUDIO_QUEUE authored_cases={len(cases)} normal48k={summary} hardware_audio_unverified=1')


if __name__=='__main__':unittest.main()
