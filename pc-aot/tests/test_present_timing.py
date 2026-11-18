"""Preserve real DXVK presentation calls, results, waits and recording schema."""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('present_dxvk',ROOT/'ios/dependencies/prepare_dxvk.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
SOURCE=ROOT/'vendor/skydream/third_party/dxvk'
FILES=['src/vulkan/vulkan_presenter.cpp','src/dxvk/dxvk_cmdlist.cpp','src/dxvk/dxvk_queue.cpp']


class PresentTimingTests(unittest.TestCase):
    def test_only_observation_changes_no_wait_timeout_or_error_policy(self):
        for name in FILES:
            old=(SOURCE/name).read_text()
            new=adapter.adapt(name,old)
            restored=new.removeprefix('#include "performance_metrics.h"\n')
            restored=re.sub(r'^ *const uint64_t ios\w+Started = skyrim_perf_now_ns\(\);\n','',restored,flags=re.M)
            restored=re.sub(r'^ *skyrim_perf_end\(SKYRIM_PERF_\w+, ios\w+Started, [^\n]+\);\n','',restored,flags=re.M)
            restored=restored.replace('const VkResult iosSubmitResult = m_vkd->vkQueueSubmit(queue, 1, &submitInfo, fence);\n    return iosSubmitResult;',
                                      'return m_vkd->vkQueueSubmit(queue, 1, &submitInfo, fence);')
            self.assertEqual(restored,old,name)

    def test_actual_acquire_and_present_success_suboptimal_failure_and_timeout(self):
        cxx=shutil.which(os.environ.get('CXX','c++'))
        if not cxx:self.skipTest('C++ compiler required; run on WSL/Mac')
        data=adapter.adapt(FILES[0],(SOURCE/FILES[0]).read_text())
        a=data.index('  VkResult Presenter::acquireNextImage(')
        b=data.index('  VkResult Presenter::recreateSwapChain(',a)
        methods=data[a:b]
        with tempfile.TemporaryDirectory(prefix='present-timing-') as tmp:
            path=Path(tmp);source=path/'test.cpp';binary=path/'test'
            source.write_text((ROOT/'tests/native/present_timing_fixture.cpp.in').read_text().replace('/* ACTUAL_METHODS */',methods))
            cmd=[cxx,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'ios/runtime-port'),str(source),'-o',str(binary)]
            if os.environ.get('SKYRIM_TEST_SANITIZERS')=='1':cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True,timeout=60)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=10)
            self.assertIn('PRESENT_TIMING exact_calls_results=PASS',result.stdout)


if __name__=='__main__':unittest.main()
