"""Deterministic production OFF/ON graphics-state coordination checks.

The native fixture uses the pinned packed state, instance class, Spinlock, and
generated lookup/publication methods. Only the GPU-facing operations are authored.
Fixture-only wait notifications establish ordering; deadlines detect deadlocks,
and are not latency measurements. No Vulkan device or private game input is used.
"""
import importlib.util
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/skydream/third_party/dxvk"
DEPENDENCIES = ROOT / "ios/dependencies"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


adapter = module("graphics_state_production", ROOT / "ios/dependencies/prepare_dxvk.py")


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def actual_instance(header):
    begin = header.index("  class DxvkGraphicsPipelineInstance {")
    return header[begin:header.index("\n  };", begin) + 5]


def fixture():
    base = "src/dxvk/dxvk_graphics"
    header = adapter.adapt(base + ".h", (SOURCE / (base + ".h")).read_text(encoding="utf-8"))
    code = adapter.adapt(base + ".cpp", (SOURCE / (base + ".cpp")).read_text(encoding="utf-8"))
    result_type = "DxvkGraphicsPipelineInstance*"
    field_start = header.index("    alignas(CACHE_LINE_SIZE) sync::Spinlock", header.index("  class DxvkGraphicsPipeline {"))
    fields = header[field_start:header.index("    " + result_type + " createInstance(", field_start)]
    # Only the vector's allocator changes: deterministic publication failure is
    # injected at the actual emplace_back, without changing instance semantics.
    vector = "std::vector<DxvkGraphicsPipelineInstance>"
    assert fields.count(vector) == 1
    fields = fields.replace(vector, "std::vector<DxvkGraphicsPipelineInstance, PublicationAllocator<DxvkGraphicsPipelineInstance>>")
    methods = section(code, "  VkPipeline DxvkGraphicsPipeline::getPipelineHandle(",
                      "  VkPipeline DxvkGraphicsPipeline::createPipeline(")
    anchor = "          flight.wait(flightLock);"
    assert methods.count(anchor) == 2
    methods = methods.replace(anchor,
        "          driver.note_wait(flight.state(), flight.pass());\n" + anchor)
    anchor = "      if (!compileLock.try_lock()) {"
    assert methods.count(anchor) == 2
    methods = methods.replace(anchor, anchor + "\n        driver.note_wait(state, renderPass);")
    definitions = f"""
namespace dxvk {{
struct DxvkPipelineManager {{
    explicit DxvkPipelineManager(bool enabled = false) : gate(enabled) {{}}
    explicit DxvkPipelineManager(skyrim::PrefetchGate::Hooks hooks) : gate(true, hooks) {{}}
    const bool exact = skyrim::exactGraphicsStateEnabled();
    skyrim::PrefetchGate gate;
    skyrim::PrefetchGate& iosPrefetchGate() {{ return gate; }}
    bool iosExactGraphicsState() const {{ return exact; }}
    std::atomic<unsigned> m_numGraphicsPipelines{{0}};
}};
{actual_instance(header)}
class DxvkGraphicsPipeline {{
public:
    DxvkGraphicsPipeline(Driver& d, DxvkPipelineManager& m) : m_pipeMgr(&m), driver(d) {{
        if (m_pipeMgr->iosExactGraphicsState()) m_compileFlights.emplace();
    }}
    VkPipeline getPipelineHandle(const DxvkGraphicsPipelineStateInfo&, const DxvkRenderPass*);
    void compilePipeline(const DxvkGraphicsPipelineStateInfo&, const DxvkRenderPass*);
    VkPipeline get(const State& state, const DxvkRenderPass* pass) {{ return getPipelineHandle(state, pass); }}
    void build(const State& state, const DxvkRenderPass* pass) {{ compilePipeline(state, pass); }}
    size_t size() {{ std::lock_guard<sync::Spinlock> lock(m_mutex); return m_pipelines.size(); }}
    size_t capacity() {{ std::lock_guard<sync::Spinlock> lock(m_mutex); return m_pipelines.capacity(); }}
    bool list_unlocked() {{ if (!m_mutex.try_lock()) return false; m_mutex.unlock(); return true; }}
    bool flight_idle(const State& state, const DxvkRenderPass* pass) {{
        if (m_compileFlights) {{
            skyrim::GraphicsFlights<State, DxvkRenderPass>::Ticket probe(*m_compileFlights, state, pass);
            auto lock = m_compileFlights->lock();
            return !probe.busy(lock);
        }}
        (void)state; (void)pass;
        if (!m_compileMutex.try_lock()) return false;
        m_compileMutex.unlock(); return true;
    }}
private:
    DxvkPipelineManager* m_pipeMgr;
    Driver& driver;
{fields}
    {result_type} createInstance(const State&, const DxvkRenderPass*);
    VkPipeline getPipelineHandleExact(const State&, const DxvkRenderPass*);
    void compilePipelineExact(const State&, const DxvkRenderPass*);
    std::optional<VkPipeline> createInstanceExact(const State&, const DxvkRenderPass*);
    DxvkGraphicsPipelineInstance* findInstance(const State&, const DxvkRenderPass*);
    bool validatePipelineState(const State& state) const {{ return driver.validate(state); }}
    VkPipeline createPipeline(const State& state, const DxvkRenderPass* pass) const {{ return driver.create(state, pass); }}
    void destroyPipeline(VkPipeline pipeline) const {{ driver.destroy(pipeline); }}
    void writePipelineStateToCache(const State& state, unsigned format) const {{ driver.write(state, format); }}
}};
{methods}
}}
using TestPipeline = dxvk::DxvkGraphicsPipeline;
using Manager = dxvk::DxvkPipelineManager;
"""
    authored = (ROOT / "tests/native/test_graphics_state_flights.cpp").read_text(encoding="utf-8")
    assert authored.count("// ACTUAL_DXVK_METHODS") == 1
    return authored.replace("// ACTUAL_DXVK_METHODS", definitions)


def flags():
    mode = os.environ.get("SKYRIM_TEST_SANITIZERS")
    if mode == "thread":
        return ["-fsanitize=thread", "-fno-omit-frame-pointer"]
    if mode == "1":
        return ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    return []


def includes(stage):
    result = ["-I", str(ROOT / "ios/runtime-port"), "-I", str(ROOT / "ios/dependencies"),
              "-I", str(DEPENDENCIES)]
    for path in ("src", "src/dxvk", "src/util", "include", "include/native",
                 "include/native/windows", "include/native/directx"):
        result += ["-I", str(stage / path)]
    return result


def native_environment(exact=None):
    result = dict(os.environ)
    if platform.system() == "Darwin":
        result.pop("IPHONEOS_DEPLOYMENT_TARGET", None)
        result.pop("SDKROOT", None)
    result.pop("SKYRIM_EXACT_GRAPHICS_STATE", None)
    if exact is not None:
        result["SKYRIM_EXACT_GRAPHICS_STATE"] = exact
    return result


def macos_flags():
    if platform.system() != "Darwin":
        return []
    result = subprocess.run(["xcrun", "--sdk", "macosx", "--show-sdk-path"],
                            env=native_environment(), check=True, capture_output=True, text=True, timeout=20)
    sdk = result.stdout.strip()
    if not sdk:
        raise RuntimeError("xcrun returned an empty macOS SDK path")
    return ["-isysroot", sdk, "-mmacosx-version-min=14.0"]


def serial_source(name):
    raw = (SOURCE / name).read_text(encoding="utf-8")
    text = adapter.adapt_pipeline_locking_baseline(name, raw)
    return adapter.adapt_timing(name, text) if name.endswith(".cpp") else text


class GraphicsStateFlightAdapterTests(unittest.TestCase):
    def test_identity_gpu_creation_and_remaining_methods_are_unchanged(self):
        path = "src/dxvk/dxvk_graphics.cpp"
        old = serial_source(path)
        new = adapter.adapt(path, (SOURCE / path).read_text(encoding="utf-8"))
        marker = "  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::findInstance("
        self.assertEqual(old[old.index(marker):], new[new.index(marker):])
        path = "src/dxvk/dxvk_graphics.h"
        old = serial_source(path)
        new = adapter.adapt(path, (SOURCE / path).read_text(encoding="utf-8"))
        self.assertEqual(actual_instance(old), actual_instance(new))
        self.assertIn("m_renderPass  == rp", actual_instance(new))
        self.assertIn("m_stateVector == state", actual_instance(new))

    def test_off_route_and_ready_hit_prefix_are_preserved(self):
        name = "src/dxvk/dxvk_graphics.cpp"
        old = serial_source(name)
        new = adapter.adapt(name, (SOURCE / name).read_text(encoding="utf-8"))
        start = "  VkPipeline DxvkGraphicsPipeline::getPipelineHandle("
        expected = section(old, start, "  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::findInstance(")
        actual = section(new, start, "  VkPipeline DxvkGraphicsPipeline::getPipelineHandleExact(")
        actual = actual.replace("    if (m_compileFlights) return this->getPipelineHandleExact(state, renderPass);\n\n", "")
        actual = actual.replace("    if (m_compileFlights) { this->compilePipelineExact(state, renderPass); return; }\n\n", "")
        self.assertEqual(actual, expected)
        self.assertEqual(section(old, start, "    SkyrimPerfContextScope iosRequest"),
                         section(new, start, "    if (m_compileFlights)"))
        exact = section(new, "  VkPipeline DxvkGraphicsPipeline::getPipelineHandleExact(",
                        "  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::findInstance(")
        self.assertNotIn("SKYRIM_PERF_GRAPHICS_COMPILE_WAIT", exact)
        self.assertEqual(exact.count("SKYRIM_PERF_GRAPHICS_STATE_WAIT"), 2)

    def test_policy_initializes_before_workers_and_off_table_stays_empty(self):
        name = "src/dxvk/dxvk_pipemanager.h"
        manager = adapter.adapt(name, (SOURCE / name).read_text(encoding="utf-8"))
        self.assertLess(manager.index("m_iosExactGraphicsState = skyrim::exactGraphicsStateEnabled()"), manager.index("m_stateCache;"))
        name = "src/dxvk/dxvk_graphics.cpp"
        code = adapter.adapt(name, (SOURCE / name).read_text(encoding="utf-8"))
        self.assertEqual(code.count("if (m_pipeMgr->iosExactGraphicsState()) m_compileFlights.emplace();"), 1)
        self.assertNotIn("getenv", code)
        self.assertNotIn("IOS_GRAPHICS_POLICY", code)
        name = "src/dxvk/dxvk_graphics.h"
        header = adapter.adapt(name, (SOURCE / name).read_text(encoding="utf-8"))
        self.assertIn("std::optional<skyrim::GraphicsFlights", header)
        self.assertIn("dxvk::mutex m_compileMutex;", header)

    def test_production_does_not_import_experiment_or_replace_compute_route(self):
        production = (DEPENDENCIES / "prepare_dxvk.py").read_text(encoding="utf-8")
        self.assertNotIn("dxvk-state-cold", production)
        for suffix in (".h", ".cpp"):
            name = "src/dxvk/dxvk_compute" + suffix
            self.assertEqual(adapter.adapt(name, (SOURCE / name).read_text(encoding="utf-8")), serial_source(name))
        generated = fixture()
        self.assertIn("getPipelineHandleExact", generated)
        self.assertIn("compileLock(m_compileMutex", generated)
        self.assertNotIn("#if PROTOTYPE", generated)


@unittest.skipUnless(os.name == "posix" and shutil.which(os.environ.get("CXX", "c++")),
                     "Native POSIX compiler required")
class GraphicsStateFlightNativeTests(unittest.TestCase):
    def test_actual_state_lookup_publication_admission_and_failures(self):
        with tempfile.TemporaryDirectory(prefix="graphics-flights-native-") as tmp:
            folder = Path(tmp)
            objects = []
            for name in ("performance_metrics", "device_policy"):
                obj = folder / (name + ".o")
                result = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", *flags(), *macos_flags(),
                    "-D_POSIX_C_SOURCE=200809L", "-c", str(ROOT / "ios/runtime-port" / (name + ".c")),
                    "-o", str(obj)], env=native_environment(), capture_output=True, text=True, timeout=40)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                objects.append(str(obj))
            path = folder / "fixture.cpp"
            path.write_text(fixture(), encoding="utf-8")
            executable = folder / "fixture"
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-g", "-pthread", *flags(), *macos_flags(),
                "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-Wno-unused-parameter",
                "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00",
                *includes(SOURCE), str(path), *objects, "-o", str(executable)]
            result = subprocess.run(command, env=native_environment(), capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            command = [str(executable)]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "thread" and os.environ.get("SKYRIM_TSAN_DISABLE_ASLR") == "1":
                setarch = shutil.which("setarch")
                self.assertIsNotNone(setarch, "setarch required for requested ThreadSanitizer ASLR control")
                command = [setarch, platform.machine(), "-R", *command]
            # One linked production binary; only the immutable startup setting differs.
            for exact in (None, "", "0", "true", "01", "1 ", "2", "1"):
                with self.subTest(policy=exact):
                    result = subprocess.run([*command, "--policy-only"], env=native_environment(exact), capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(f"policy_state={int(exact == '1')}", result.stdout)
                    self.assertEqual(result.stderr.count("IOS_GRAPHICS_POLICY "), 1)
                    self.assertIn(f"exact_state={int(exact == '1')} wait_scope={int(exact == '1')}", result.stderr)
            for enabled in (False, True):
                with self.subTest(exact=enabled):
                    result = subprocess.run(command, env=native_environment(str(int(enabled))), capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    common = ("ready_and_independent_owner", "exact_packed_state_and_pass_identity",
                              "same_identity_single_generation", "cached_null_invalid_absent",
                              "exception_wakes_retry", "reentrant_callback_growth", "eight_threads",
                              "startup_policy_latched", "hot_ready_clock_free", "wait_kind_split")
                    for marker in common:
                        self.assertIn(marker + "=PASS", result.stdout)
                    self.assertIn(f"different_state_progress={int(enabled)}", result.stdout)
                    if enabled:
                        for marker in ("publication_failure_cleanup_before_release", "parked_prefetch_owns_no_flight",
                                       "admitted_work_not_preempted", "shutdown_releases_admission"):
                            self.assertIn(marker + "=PASS", result.stdout)
                    print("production exact ON" if enabled else "production exact OFF", result.stdout.strip(), flush=True)

    def test_complete_graphics_and_manager_translation_units(self):
        with tempfile.TemporaryDirectory(prefix="graphics-flights-syntax-") as tmp:
            stage = Path(tmp) / "dxvk"
            adapter.stage(SOURCE, stage)
            command = [os.environ.get("CXX", "c++"), "-std=c++17", *macos_flags(), "-fsyntax-only", "-include", "cstdint", "-include", "cstring",
                       "-Wno-deprecated-declarations", "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", *includes(stage)]
            for name in ("dxvk_graphics.cpp", "dxvk_pipemanager.cpp"):
                with self.subTest(name=name):
                    result = subprocess.run([*command, str(stage / "src/dxvk" / name)], env=native_environment(),
                                            capture_output=True, text=True, timeout=90)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
