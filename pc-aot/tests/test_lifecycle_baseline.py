"""The real-Metal failure is retained; the rejected candidate cannot ship."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


baseline = load('lifecycle_baseline', 'ios/dependencies/verify_lifecycle_baseline.py')
rejected = load('rejected_lifecycle', 'ios/dependencies/patch_mvk_lifecycle.py')


class LifecycleBaselineTests(unittest.TestCase):
    def source(self):
        path = Path(os.environ.get('SKYRIM_MVK_LIFECYCLE_SOURCE', str(
            ROOT / 'local/lifecycle-20261114/MoltenVK-458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK')))
        if not path.is_dir():
            self.skipTest('Pinned public source required; supplied by the Mac route')
        return path

    def test_all_native_submission_files_remain_original(self):
        source = self.source()
        self.assertEqual(baseline.PINS, rejected.PINS)
        baseline.verify(source)
        staged = os.environ.get('SKYRIM_MVK_LIFECYCLE_PATCHED_SOURCE')
        if staged:
            baseline.verify(Path(staged))
        print('LIFECYCLE_BASELINE original_submission_files=4 rejected_gate_shipped=0 background_fix_verified=0')

    def test_previously_gated_and_incomplete_stages_are_rejected(self):
        source = self.source()
        with tempfile.TemporaryDirectory(prefix='lifecycle-baseline-') as tmp:
            stage = Path(tmp)
            with self.assertRaises(ValueError):
                baseline.verify(stage)
            for name in baseline.PINS:
                target = stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((source / name).read_bytes())
            baseline.verify(stage)
            rejected.apply(stage, source)
            with self.assertRaisesRegex(ValueError, 'Rejected or unreviewed'):
                baseline.verify(stage)

    def test_product_does_not_link_or_call_the_rejected_gate(self):
        build = (ROOT / 'ios/dependencies/build-dependencies.sh').read_text()
        cmake = (ROOT / 'ios/dependencies/CMakeLists.txt').read_text()
        self.assertNotIn('patch_mvk_lifecycle.py', build + cmake)
        self.assertNotIn('gpu_gate.c', cmake)
        self.assertNotIn('gpu_gate_metal.m', cmake)
        for name in ('device_platform.m', 'ios_runtime.c'):
            text = (ROOT / 'ios/runtime-port' / name).read_text()
            self.assertNotIn('skyrim_gpu_', text)
            self.assertNotIn('gpu_gate.h', text)
        self.assertIn('verify_lifecycle_baseline.py', build)
        self.assertIn("grep -q '_skyrim_gpu_'", (ROOT / 'scripts/build-title-host.sh').read_text())
        self.assertTrue((ROOT / 'experiments/gpu_lifecycle_gate_test.py').is_file())


if __name__ == '__main__':
    unittest.main()
