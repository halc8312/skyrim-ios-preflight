"""Authored compiler invocation/refusal fixtures; never invokes Apple's tools."""
import contextlib
import importlib.util
import io
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

spec=importlib.util.spec_from_file_location('public_frame_compiler',Path(__file__).with_name('compile_public_frame_shaders.py'))
compiler=importlib.util.module_from_spec(spec);spec.loader.exec_module(compiler)

def authored(root,sampled=False):
    src=root/'authored';src.mkdir()
    names=['pixel.metal','PublicSampledShaders.h'] if sampled else ['vertex.metal','pixel.metal','PublicFrameShaders.h']
    for name in names:(src/name).write_text('// authored\n')
    pins={'schema':1,'files':{name:{'bytes':(src/name).stat().st_size,'sha256':hashlib.sha256((src/name).read_bytes()).hexdigest()} for name in names}}
    (src/'source-pins.json').write_text(json.dumps(pins))
    return src

class CompilerTests(unittest.TestCase):
    def test_sampled_shader_set_compiles_one_pinned_library_and_requires_matching_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);src=authored(root,sampled=True);calls=[]
            def run(command,**kwargs):
                calls.append(command);self.assertTrue(kwargs['check'])
                Path(command[command.index('-o')+1]).write_bytes(b'AIR' if '-c' in command else b'MTLB'+bytes(1024))
            with mock.patch.object(compiler.subprocess,'check_output',return_value='/SDK'),mock.patch.object(compiler.subprocess,'run',side_effect=run),contextlib.redirect_stdout(io.StringIO()):
                compiler.compile('simulator',src,root/'fresh','sampled')
            self.assertEqual(len(calls),2);self.assertTrue((root/'fresh/PublicSampledPixel.metallib').is_file())
            (src/'PublicSampledShaders.h').write_text('// changed\n')
            with mock.patch.object(compiler.subprocess,'check_output') as read,self.assertRaises(ValueError):compiler.compile('device',src,root/'new','sampled')
            read.assert_not_called()

    def test_invalid_variant_never_creates_or_executes(self):
        with mock.patch.object(compiler.subprocess,'check_output') as read,self.assertRaises(ValueError):
            compiler.compile('macos','missing','missing-output')
        read.assert_not_called()

    def test_exact_sdk_targets_precise_math_two_separate_offline_libraries(self):
        for variant,sdk,target in [('device','iphoneos','air64-apple-ios26.0'),('simulator','iphonesimulator','air64-apple-ios26.0-simulator')]:
            with self.subTest(variant=variant),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);src=authored(root);dest=root/'fresh'
                calls=[]
                def run(command,**kwargs):
                    calls.append(command);self.assertTrue(kwargs['check']);self.assertEqual(kwargs['timeout'],120)
                    output=Path(command[command.index('-o')+1]);output.write_bytes(b'AIR' if '-c' in command else b'MTLB'+bytes(1024))
                with mock.patch.object(compiler.subprocess,'check_output',return_value='/SDK'),mock.patch.object(compiler.subprocess,'run',side_effect=run),contextlib.redirect_stdout(io.StringIO()):
                    compiler.compile(variant,src,dest)
                self.assertEqual(len(calls),4)
                for call in calls:self.assertEqual(call[:3],['xcrun','--sdk',sdk]);self.assertEqual(call[call.index('-target')+1],target)
                for call in calls[::2]:self.assertIn('-fno-fast-math',call);self.assertIn('-std=ios-metal2.4',call)
                self.assertTrue((dest/'PublicXenosVertex.metallib').is_file());self.assertTrue((dest/'PublicXenosPixel.metallib').is_file())
                with mock.patch.object(compiler.subprocess,'check_output') as read,self.assertRaises(ValueError):compiler.compile(variant,src,dest)
                read.assert_not_called()

    def test_compiler_failure_does_not_retry_or_publish_second_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);src=authored(root)
            with mock.patch.object(compiler.subprocess,'check_output',return_value='/SDK'),mock.patch.object(compiler.subprocess,'run',side_effect=subprocess.CalledProcessError(1,'metal')) as run,self.assertRaises(subprocess.CalledProcessError):compiler.compile('device',src,root/'fresh')
            self.assertEqual(run.call_count,1);self.assertFalse((root/'fresh/PublicXenosPixel.metallib').exists())

    def test_stale_header_or_shader_is_refused_before_apple_tools(self):
        for name in ['vertex.metal','pixel.metal','PublicFrameShaders.h']:
            with self.subTest(name=name),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);src=authored(root);(src/name).write_text('// changed\n')
                with mock.patch.object(compiler.subprocess,'check_output') as read,self.assertRaises(ValueError):compiler.compile('device',src,root/'fresh')
                read.assert_not_called();self.assertFalse((root/'fresh').exists())

if __name__=='__main__':unittest.main()
