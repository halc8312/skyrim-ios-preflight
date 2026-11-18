"""Run actual pinned DXVK lookup/publication methods with an authored compiler.

No Vulkan device or game input is used. A blocked compiler exposes head-of-line
blocking deterministically; real instance classes and the real Spinlock are used.
Full DXVK translation units are compiled separately with their production headers.
"""
import importlib.util
import os
import platform
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/skydream/third_party/dxvk"
spec = importlib.util.spec_from_file_location("pipeline_stage", ROOT / "ios/dependencies/prepare_dxvk.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def fixture(kind, patched):
    base = "src/dxvk/dxvk_" + kind.lower()
    header = (SOURCE / (base + ".h")).read_text()
    code = (SOURCE / (base + ".cpp")).read_text()
    if patched:
        # Historical serialized implementation remains an independent oracle.
        # The full-state production fixture separately executes runtime OFF/ON.
        header = adapter.adapt_pipeline_locking_baseline(base + ".h", header)
        code = adapter.adapt_timing(base + ".cpp", adapter.adapt_pipeline_locking_baseline(base + ".cpp", code))
    cls = f"Dxvk{kind}Pipeline"
    # Closing class marker is unambiguous before the next class documentation.
    begin = header.index(f"  class {cls}Instance {{")
    instance = header[begin:header.index("\n  };", begin) + 5]
    field_begin = header.index("sync::Spinlock", header.index(f"  class {cls} {{"))
    fields = header[field_begin:header.index(f"    {cls}Instance* createInstance(", field_begin)]
    methods = section(code, f"  VkPipeline {cls}::getPipelineHandle(", f"  VkPipeline {cls}::createPipeline(")
    graphic = kind == "Graphics"
    state = f"const Dxvk{kind}PipelineStateInfo& state"
    args = "state"
    if graphic:
        state += ", const DxvkRenderPass* renderPass"
        args += ", renderPass"
    definitions = f"""
namespace dxvk {{
using VkPipeline = uint64_t;
#define VK_NULL_HANDLE 0
struct TestState {{ unsigned id; bool operator==(const TestState& o) const {{ return id==o.id; }} }};
using Dxvk{kind}PipelineStateInfo = TestState;
struct DxvkRenderPass {{ unsigned id; unsigned format() const {{ return id; }} }};
struct DxvkPipelineManager {{
  skyrim::PrefetchGate gate;
  skyrim::PrefetchGate& iosPrefetchGate() {{ return gate; }}
  std::atomic<unsigned> m_num{kind}Pipelines{{0}};
}};
{instance}
class {cls} {{
public:
    {cls}(Driver& d, DxvkPipelineManager& m): m_pipeMgr(&m), driver(d) {{}}
    VkPipeline getPipelineHandle({state});
    void compilePipeline({state});
    VkPipeline get(unsigned id, unsigned rp=0) {{ (void)rp; TestState state{{id}}; return getPipelineHandle({('state, &passes[rp]' if graphic else 'state')}); }}
    void build(unsigned id, unsigned rp=0) {{ (void)rp; TestState state{{id}}; compilePipeline({('state, &passes[rp]' if graphic else 'state')}); }}
private:
    DxvkPipelineManager* m_pipeMgr;
    Driver& driver;
    {('DxvkRenderPass passes[2]{{0},{1}};' if graphic else '')}
    {fields}
    {cls}Instance* createInstance({state});
    {cls}Instance* findInstance({state});
    bool validatePipelineState(const TestState& state) const {{ return state.id != 9999; }}
    VkPipeline createPipeline({state}) const {{ return driver.create(state.id, {('renderPass->id' if graphic else '0')}); }}
    void writePipelineStateToCache(const TestState& state{(', unsigned format' if graphic else '')}) const {{ driver.write(state.id, {('format' if graphic else '0')}); }}
}};
{methods}
}}
using TestPipeline = dxvk::{cls};
"""
    authored = (ROOT / "tests/native/test_pipeline_locking.cpp").read_text()
    return authored.replace("// ACTUAL_DXVK_METHODS", definitions)


@unittest.skipUnless(os.name == "posix" and shutil.which("c++"), "Native POSIX C++ compiler required")
class PipelineLockingTests(unittest.TestCase):
    def test_actual_lookup_publication_and_concurrent_misses(self):
        for kind in ("Graphics", "Compute"):
            for patched in (False, True):
                with self.subTest(kind=kind, patched=patched), tempfile.TemporaryDirectory(prefix="pipeline-lock-") as tmp:
                    folder = Path(tmp)
                    path = folder / "fixture.cpp"
                    path.write_text(fixture(kind, patched))
                    mode=os.environ.get("SKYRIM_TEST_SANITIZERS")
                    flags = ["-fsanitize=thread", "-fno-omit-frame-pointer"] if mode=="thread" else ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if mode=="1" else []
                    metrics = folder / "metrics.o"
                    policy = folder / "policy.o"
                    subprocess.run(["cc", "-std=c11", "-O2", *flags, "-D_POSIX_C_SOURCE=200809L", "-c",
                        str(ROOT / "ios/runtime-port/performance_metrics.c"), "-o", str(metrics)], check=True, timeout=40)
                    subprocess.run(["cc", "-std=c11", "-O2", *flags, "-D_POSIX_C_SOURCE=200809L", "-c",
                        str(ROOT / "ios/runtime-port/device_policy.c"), "-o", str(policy)], check=True, timeout=40)
                    command = ["c++", "-std=c++17", "-O2", "-g", "-pthread", *flags, "-DDXVK_NATIVE",
                        "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                        f"-DPATCHED={int(patched)}", f"-DGRAPHICS={int(kind=='Graphics')}",
                        "-I", str(SOURCE / "src"), "-I", str(SOURCE / "include/native"),
                        "-I", str(SOURCE / "include/native/windows"), "-I", str(SOURCE / "include/native/directx"),
                        "-I", str(ROOT / "ios/runtime-port"), "-I", str(ROOT / "ios/dependencies"),
                        str(path), str(metrics), str(policy), "-o", str(folder / "fixture")]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    command=[str(folder/"fixture")]
                    if mode=="thread" and os.environ.get("SKYRIM_TSAN_DISABLE_ASLR")=="1":
                        setarch=shutil.which("setarch"); self.assertIsNotNone(setarch)
                        command=[setarch,platform.machine(),"-R",*command]
                    for gate in (("0", "1") if patched else ("0",)):
                        result = subprocess.run(command, capture_output=True, text=True, timeout=25,
                            env={**os.environ, 'SKYRIM_PREFETCH_GATE': gate})
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn(f"ready_while_compiler_blocked={int(patched)}", result.stdout)
                        self.assertIn("semantics=PASS", result.stdout)
                        print(kind, "patched" if patched else "baseline", "gate="+gate, result.stdout.strip())

    def test_gpu_creation_and_instance_matching_are_unchanged(self):
        for kind in ("graphics", "compute"):
            name = f"src/dxvk/dxvk_{kind}.cpp"
            old = (SOURCE / name).read_text()
            new = adapter.adapt_pipeline_locking_baseline(name, old)
            cls = f"Dxvk{kind.title()}Pipeline"
            marker = f"  {cls}Instance* {cls}::findInstance("
            self.assertEqual(old[old.index(marker):], new[new.index(marker):])
            if kind == "compute":
                self.assertEqual(adapter.adapt(name, old), adapter.adapt_timing(name, new))
            # New mutex is the only change to each actual class declaration.
            name = f"src/dxvk/dxvk_{kind}.h"
            old = (SOURCE / name).read_text()
            new = adapter.adapt_pipeline_locking_baseline(name, old)
            self.assertEqual(new.replace("\n    // Slow compiler calls never hold the instance-list spinlock.\n    dxvk::mutex m_compileMutex;", ""), old)


if __name__ == "__main__":
    unittest.main()
