"""Production default and actual compiler output; no game shader inputs."""
import hashlib
import copy
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'vendor/skydream/third_party/dxvk'
spec=importlib.util.spec_from_file_location('float_dxvk',ROOT/'ios/dependencies/prepare_dxvk.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)

class FloatCompatibilityTests(unittest.TestCase):
    def test_metal_report_requires_both_modes_and_independent_pixel_values(self):
        spec=importlib.util.spec_from_file_location('metal_readback',ROOT/'tests/dxso-float/run_metal_check.py')
        reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
        expected={'rcp':((0x7f7fffff,)*4,(0x3f000000,)*4),
            'rsq':((0x7f7fffff,)*4,(0x3f000000,)*4),
            'nrm':((0,)*4,(0x3f19999a,0x3f4ccccd,0,0)),
            'log':((0xff7fffff,)*4,(0x40000000,)*4),
            'pow':((0x3f800000,)*4,(0x41000000,)*4)}
        report={'schema':1,'status':'passed','case_count':20,'public_only':True,'runtime_inputs':True,
            'metal_execution_verified':True,'physical_iphone_verified':False,
            'game_rendering_verified':False,'cases':[]}
        for op,pairs in expected.items():
            for i,values in enumerate(pairs):
                for mode in ('safe','fast'):
                    report['cases'].append({'case':'float-'+op+('-normal' if i else '-zero'),
                        'math_mode':mode,'matches':True,
                        'lanes':[{'bits':b,'finite':True,'matches':True} for b in values]})
        reader.verify_report(report)
        for mutant in (0,0x7f800000,0x7fc00000,0xff7fffff,True,-1,1<<32):
            bad=copy.deepcopy(report);bad['cases'][0]['lanes'][0]['bits']=mutant
            with self.assertRaises(ValueError):reader.verify_report(bad)
        bad=copy.deepcopy(report);bad['cases'][-1]=copy.deepcopy(bad['cases'][0])
        with self.assertRaises(ValueError):reader.verify_report(bad)
        for key,val in [('status','unavailable'),('case_count',True),('schema',True),
                ('physical_iphone_verified',True),('game_rendering_verified',True),('metal_execution_verified',False),('runtime_inputs',False)]:
            bad=copy.deepcopy(report);bad[key]=val
            with self.assertRaises(ValueError):reader.verify_report(bad)

    def test_staged_default_preserves_explicit_override_and_real_compiler_path(self):
        key='src/d3d9/d3d9_config.h'
        old=(SOURCE/key).read_text()
        self.assertEqual(hashlib.sha256(old.encode()).hexdigest(),adapter.EXPECTED[key])
        new=adapter.adapt(key,old)
        self.assertEqual(new,old.replace('FloatEmulationByDefault = false','FloatEmulationByDefault = true',1))
        for key,digest in {
            'src/d3d9/d3d9_options.cpp':'b19573357f8f611d03381c4e892cb95ce8489286cb734b0a06df8f5e71994e3a',
            'src/dxso/dxso_options.cpp':'263b22a161b06e905790f2830b024ee15f19319a558b1247c4ad5f121f3c87ec',
            'src/dxso/dxso_compiler.cpp':'1f3e5184a6dc0b5a5114574dae6e160431d05a33223a500dd4cb6278f112bb6b',
        }.items():
            self.assertEqual(hashlib.sha256((SOURCE/key).read_text().encode()).hexdigest(),digest)
        options=(SOURCE/'src/d3d9/d3d9_options.cpp').read_text()
        self.assertLess(options.index('this->d3d9FloatEmulation = config::FloatEmulationByDefault'),
                        options.index('applyTristate(this->d3d9FloatEmulation'))
        self.assertIn('prepare_dxvk.py',(ROOT/'ios/dependencies/CMakeLists.txt').read_text())

    def test_actual_public_translator_fixture(self):
        binary=os.environ.get('SKYRIM_DXSO_FLOAT_FIXTURE')
        if not binary:self.skipTest('Run after building tests/dxso-float, or in the next authorized Metal fixture gate')
        output=os.environ.get('SKYRIM_DXSO_FLOAT_OUTPUT')
        if output:
            Path(output).mkdir(parents=True,exist_ok=False)
            self.check_fixture(binary,output)
            return
        with tempfile.TemporaryDirectory(prefix='dxso-float-') as tmp:
            self.check_fixture(binary,tmp)

    def check_fixture(self,binary,output):
        result=subprocess.run([str(Path(binary).resolve()),str(Path(output).resolve())],cwd=output,
                              text=True,capture_output=True,check=True,timeout=45)
        self.assertIn('actual_compiler_cases=30 finite_boundary_emission=PASS',result.stdout)
        self.assertEqual(len(list(Path(output).glob('*.spv'))),30)

if __name__=='__main__':unittest.main()
