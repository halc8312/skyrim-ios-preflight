"""Compile the complete production allocator with an authored Vulkan driver."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT / "vendor/skydream/third_party/dxvk"
spec=importlib.util.spec_from_file_location("device_dxvk",ROOT / "ios/dependencies/prepare_dxvk.py")
adapter=importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class DeviceMemoryTests(unittest.TestCase):
    def test_actual_allocator_lifetime_pressure_alignment_failure_and_concurrency(self):
        cc=shutil.which(os.environ.get("CC","cc"))
        cxx=shutil.which(os.environ.get("CXX","c++"))
        if not cc or not cxx:
            self.skipTest("C/C++ compiler required; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="device-memory-") as tmp:
            folder=Path(tmp)
            for name in ("dxvk_memory.h","dxvk_memory.cpp"):
                key="src/dxvk/"+name
                original=(SOURCE/key).read_text()
                self.assertEqual(hashlib.sha256(original.encode()).hexdigest(),adapter.EXPECTED[key])
                (folder/name).write_text(adapter.adapt(key,original))
            # The allocator itself is unabridged; only external device/adapter calls
            # are supplied by the authored provider. Real Rc and alignment code.
            for name in ("dxvk_device.h","dxvk_adapter.h"):
                (folder/name).write_text('#include "device_memory_fixture.h"\n')
            flags=["-O2","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS")=="1":
                flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]
            runtime=ROOT/"ios/runtime-port"
            policy=folder/"policy.o"
            subprocess.run([cc,"-std=c11",*flags,"-c",str(runtime/"device_policy.c"),"-I",str(runtime),"-o",str(policy)],check=True,timeout=60)
            binary=folder/"allocator"
            subprocess.run([cxx,"-std=c++17",*flags,str(folder/"dxvk_memory.cpp"),
                str(ROOT/"tests/native/test_device_memory.cpp"),str(policy),
                "-I",str(folder),"-I",str(ROOT/"tests/native"),"-I",str(runtime),
                "-I",str(SOURCE/"src"),"-I",str(SOURCE/"include"),"-o",str(binary)],check=True,timeout=90)
            result=subprocess.run([str(binary)],check=True,text=True,capture_output=True,timeout=20)
            self.assertIn("actual_allocator=PASS",result.stdout)
            self.assertIn("concurrent=PASS",result.stdout)

    def test_worker_adapter_preserves_queue_order_and_explicit_count(self):
        key="src/dxvk/dxvk_state_cache.cpp"
        old=(SOURCE/key).read_text()
        self.assertEqual(hashlib.sha256(old.encode()).hexdigest(),adapter.EXPECTED[key])
        changed=adapter.adapt(key,old)
        restored=changed.removeprefix('#include "device_policy.h"\n')
        restored=restored.replace("skyrim_device_compiler_cpus(dxvk::thread::hardware_concurrency())","dxvk::thread::hardware_concurrency()")
        # The optional admission gate must be stopped before upstream worker
        # shutdown. Validate that exact hook, then undo it for the complete
        # source comparison below; queue ordering/count checks remain intact.
        shutdown = '  void DxvkStateCache::stopWorkerThreads() {'
        gated_shutdown = shutdown + '\n    m_pipeManager->iosPrefetchGate().stop();'
        self.assertEqual(restored.count(gated_shutdown), 1)
        restored = restored.replace(gated_shutdown, shutdown)
        for line in ("    skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER);\n",
                     "      skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER);\n",
                     "      SkyrimPerfPrefetchScope iosPrefetchScope(item.iosPrefetch);\n",
                     "    skyrim_device_platform_work_role(SKYRIM_WORK_CACHE_WRITER);\n"):
            # Preserve exact indentation when removing injected calls.
            restored="\n".join(x for x in restored.split("\n") if x+"\n" != line)
        metadata = [
            ('    const bool iosInserted = m_shaderMap.insert({ key, shader }).second;\n'
             '    // While entryLock is held, a newly inserted final prerequisite makes all\n'
             '    // its now-complete tuples eligible. Capture once before any worker-lock\n'
             '    // wait; duplicate registrations cannot reconstruct the original time.\n'
             '    const uint64_t iosReadyNs = iosInserted ? skyrim_perf_now_ns() : 0;',
             '    m_shaderMap.insert({ key, shader });'),
            ('      item.iosPrefetch.ready_ns = iosReadyNs;\n'
             '      item.iosPrefetch.queued_ns = skyrim_perf_now_ns();\n', ''),
            ('        item.iosPrefetch.worker_ns = skyrim_perf_now_ns();\n', ''),
        ]
        for added, original in metadata:
            self.assertEqual(restored.count(added), 1)
            restored = restored.replace(added, original)
        # Also reverse the independently exercised shader-tuple index change.
        # Keep the whole-file equality check: no other queue/worker edits hide here.
        indexing = [
            ('    size_t numShaderTuples = 0;\n', ''),
            ('        const bool firstShaderTuple = m_entryMap.find(entry.shaders) == m_entryMap.end();\n', ''),
            ('        if (firstShaderTuple) {\n          ++numShaderTuples;\n'
             '          mapShaderToPipeline(entry.shaders.vs,  entry.shaders);\n'
             '          mapShaderToPipeline(entry.shaders.tcs, entry.shaders);\n'
             '          mapShaderToPipeline(entry.shaders.tes, entry.shaders);\n'
             '          mapShaderToPipeline(entry.shaders.gs,  entry.shaders);\n'
             '          mapShaderToPipeline(entry.shaders.fs,  entry.shaders);\n'
             '          mapShaderToPipeline(entry.shaders.cs,  entry.shaders);\n'
             '        }',
             '        mapShaderToPipeline(entry.shaders.vs,  entry.shaders);\n'
             '        mapShaderToPipeline(entry.shaders.tcs, entry.shaders);\n'
             '        mapShaderToPipeline(entry.shaders.tes, entry.shaders);\n'
             '        mapShaderToPipeline(entry.shaders.gs,  entry.shaders);\n'
             '        mapShaderToPipeline(entry.shaders.fs,  entry.shaders);\n'
             '        mapShaderToPipeline(entry.shaders.cs,  entry.shaders);'),
            ('    Logger::info(str::format("IOS_STATE_CACHE entries=", m_entries.size(),\n'
             '      " shader_tuples=", numShaderTuples, " wakeup_links=", m_pipelineMap.size()));\n', ''),
        ]
        for added, original in indexing:
            self.assertEqual(restored.count(added), 1)
            restored = restored.replace(added, original)
        self.assertEqual(restored,old)


if __name__=="__main__":
    unittest.main()
