"""Optional CPU-only real SPIRV-Cross/MoltenVK converter sharing experiment."""
import concurrent.futures
import hashlib
import importlib.util
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


mvk_patch=load('converter_config_patch',ROOT/'ios/dependencies/patch_moltenvk.py')
fixture=load('converter_graphics_input',ROOT/'tests/graphics_alias_fixture.py')
PINS={
    'SPIRVToMSLConverter.cpp':'02645fd056c4446be7c5ceb2332ba5b0a79d875e883f658121b11e1774eb8cdc',
    'SPIRVToMSLConverter.h':'a36806cf4caf7e3d72ffc04885f58fa6a07af07a0f37416e7ceb1cc63a02b6be',
    'SPIRVSupport.cpp':'97a3a26757070e7e98934d0ac13dac1fa6e0549fbdd012097224fe3e47d9028c',
    'SPIRVSupport.h':'3d2bc15d28283adddbf5fea60e18ad9fc4e2b993fd3226fedaee5b7c062887d7',
    'FileSupport.h':'25ec06f81b76767b948581827bcd4fd8bf51c7c9ad55beef033a8805ef184343',
}
COMMON={
    'MVKCommonEnvironment.h':'ab90042d78a6e50949e512acb95a292292ed903971a3bf2295e131893c4ffc2c',
    'MVKStrings.h':'67010ed795e170dec8081bcbb2b5d9617f60ef09936374d94346e8a1cac31023',
}
CROSS={
    'spirv_cross.cpp':'39bb65692d0483950709247b52f3a0e0947b3ae2a86227d0251089b5c9243d67',
    'spirv_parser.cpp':'537b50f6bab0a4359e6dec3db23e4bf61ed9167f16b2f8070406b574d3e5d06b',
    'spirv_cross_parsed_ir.cpp':'fd7dafbe3634426bab1ce3371ecc90a38179c4b117e17fe7898c9bf52fe41e40',
    'spirv_cfg.cpp':'248526af6c0b3c74c318f5e085cf3966a2d79683849a3de264b95e73903c279c',
    'spirv_glsl.cpp':'8760139e5fef7debe9be0e465f2c72ac5bc16ed23eb550f3f82b20b4b2507d17',
    'spirv_msl.cpp':'6a016ed8fa40bd364be62b8250a4c4a91766dcc959e31c71d898beb62cfb139c',
}


class RealConverterTests(unittest.TestCase):
    def test_shared_converter_matches_serialized_results(self):
        converter_path=os.environ.get('SKYRIM_MVK_CONVERTER_SOURCE')
        cross_path=os.environ.get('SKYRIM_SPIRV_CROSS_SOURCE')
        cxx=shutil.which(os.environ.get('CXX','g++'))
        if not(converter_path and cross_path and cxx):
            self.skipTest('Pinned public converter and patched SPIRV-Cross sources plus C++ compiler required')
        converter=Path(converter_path);cross=Path(cross_path)
        common=converter.parents[1]/'Common'
        with tempfile.TemporaryDirectory(prefix='mvk-real-converter-') as directory:
            stage=Path(directory)
            for source,pins in ((converter,PINS),(common,COMMON)):
                for name,digest in pins.items():
                    data=(source/name).read_bytes()
                    self.assertEqual(hashlib.sha256(data).hexdigest(),digest,name)
                    (stage/name).write_bytes(mvk_patch.patched(data) if name=='SPIRVToMSLConverter.cpp' else data)
            for name,digest in CROSS.items():
                self.assertEqual(hashlib.sha256((cross/name).read_bytes()).hexdigest(),digest,name)
            # Only platform definitions and a numeric byte-swap adapter are
            # supplied. These do not claim an Apple SDK or Metal implementation.
            (stage/'TargetConditionals.h').write_text('#pragma once\n'+''.join(
                '#define '+name+' 0\n' for name in ('TARGET_OS_OSX','TARGET_OS_IOS','TARGET_OS_MACCATALYST','TARGET_OS_TV','TARGET_OS_WATCH','TARGET_OS_XR')))
            (stage/'CoreFoundation').mkdir()
            (stage/'CoreFoundation/CFByteOrder.h').write_text('#pragma once\n#include <stdint.h>\ninline uint32_t CFSwapInt32(uint32_t value){return __builtin_bswap32(value);}\n')
            definitions=[]
            for i,options in enumerate(({}, {'reverse':True}, {'alias':False})):
                data=fixture.fixture(**options)
                words=struct.unpack('<'+'I'*(len(data)//4),data)
                definitions.append('static const std::vector<uint32_t> input'+str(i)+'={'+','.join(hex(word) for word in words)+'};\n')
            (stage/'converter_inputs.h').write_text('#include <cstdint>\n#include <vector>\n'+''.join(definitions))
            flags=['-std=c++17','-O1','-g1','-pthread','-include','cstdint','-DMVK_EXCLUDE_SPIRV_TOOLS=1','-Wno-deprecated',
                   '-I',str(stage),'-I',str(cross)]
            if os.environ.get('SKYRIM_TEST_SANITIZERS')=='thread':
                flags+=['-fsanitize=thread','-fno-omit-frame-pointer']
            sources=[*(cross/name for name in CROSS),stage/'SPIRVToMSLConverter.cpp',stage/'SPIRVSupport.cpp',ROOT/'tests/native/test_mvk_converter_parallel.cpp']
            objects=[stage/(source.stem+'.o') for source in sources]
            def compile_unit(item):
                source,obj=item
                try:
                    completed=subprocess.run([cxx,*flags,'-c',str(source),'-o',str(obj)],capture_output=True,text=True,timeout=180)
                    return source.name+'\n'+completed.stdout+completed.stderr if completed.returncode else ''
                except subprocess.TimeoutExpired:
                    return source.name+': compile exceeded 180 seconds'
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                failures=[failure for failure in pool.map(compile_unit,zip(sources,objects)) if failure]
            self.assertEqual(failures,[], '\n'.join(failures))
            binary=stage/'converter-test'
            subprocess.run([cxx,*flags,*(str(obj) for obj in objects),'-o',str(binary)],check=True,timeout=45)
            command=[str(binary)]
            if os.environ.get('SKYRIM_TEST_SANITIZERS')=='thread' and os.environ.get('SKYRIM_TSAN_DISABLE_ASLR')=='1':
                setarch=shutil.which('setarch')
                self.assertIsNotNone(setarch, 'Explicit process-local TSan layout option requires setarch')
                command=[setarch,platform.machine(),'-R',*command]
                print('TSan address randomization disabled for the test child only; no system setting changed')
            completed=subprocess.run(command,capture_output=True,text=True,timeout=90)
            self.assertEqual(completed.returncode,0,completed.stdout+completed.stderr)
            self.assertEqual(completed.stdout.count('msl_and_metadata_mismatches=0 input_mutations=0'),2)
            self.assertIn('known_pinned_converter_error returned_success=1 empty_msl=1 error_logged=1 production_fix=0', completed.stdout)
            print(completed.stdout.strip())


if __name__=='__main__':unittest.main()
