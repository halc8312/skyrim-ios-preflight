"""Deterministic, isolated per-state graphics-flight experiment checks.

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
EXPERIMENT = ROOT / "experiments/dxvk-state-cold"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


adapter = module("graphics_state_production", ROOT / "ios/dependencies/prepare_dxvk.py")
experiment = module("graphics_state_experiment", EXPERIMENT / "patch.py")


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def actual_instance(header):
    begin = header.index("  class DxvkGraphicsPipelineInstance {")
    return header[begin:header.index("\n  };", begin) + 5]


def fixture(prototype):
    base = "src/dxvk/dxvk_graphics"
    header = adapter.adapt(base + ".h", (SOURCE / (base + ".h")).read_text(encoding="utf-8"))
    code = adapter.adapt(base + ".cpp", (SOURCE / (base + ".cpp")).read_text(encoding="utf-8"))
    if prototype:
        header = experiment.patched(base + ".h", header)
        code = experiment.patched(base + ".cpp", code)
    result_type = "std::optional<VkPipeline>" if prototype else "DxvkGraphicsPipelineInstance*"
    field_start = header.index("    alignas(CACHE_LINE_SIZE) sync::Spinlock", header.index("  class DxvkGraphicsPipeline {"))
    fields = header[field_start:header.index("    " + result_type + " createInstance(", field_start)]
    # Only the vector's allocator changes: deterministic publication failure is
    # injected at the actual emplace_back, without changing instance semantics.
    vector = "std::vector<DxvkGraphicsPipelineInstance>"
    assert fields.count(vector) == 1
    fields = fields.replace(vector, "std::vector<DxvkGraphicsPipelineInstance, PublicationAllocator<DxvkGraphicsPipelineInstance>>")
    methods = section(code, "  VkPipeline DxvkGraphicsPipeline::getPipelineHandle(",
                      "  VkPipeline DxvkGraphicsPipeline::createPipeline(")
    if prototype:
        anchor = "          flight.wait(flightLock);"
        assert methods.count(anchor) == 2
        methods = methods.replace(anchor,
            "          driver.note_wait(flight.state(), flight.pass());\n" + anchor)
    else:
        anchor = "      if (!compileLock.try_lock()) {"
        assert methods.count(anchor) == 2
        methods = methods.replace(anchor, anchor + "\n        driver.note_wait(state, renderPass);")
    definitions = f"""
namespace dxvk {{
struct DxvkPipelineManager {{
    explicit DxvkPipelineManager(bool enabled = false) : gate(enabled) {{}}
    explicit DxvkPipelineManager(skyrim::PrefetchGate::Hooks hooks) : gate(true, hooks) {{}}
    skyrim::PrefetchGate gate;
    skyrim::PrefetchGate& iosPrefetchGate() {{ return gate; }}
    std::atomic<unsigned> m_numGraphicsPipelines{{0}};
}};
{actual_instance(header)}
class DxvkGraphicsPipeline {{
public:
    DxvkGraphicsPipeline(Driver& d, DxvkPipelineManager& m) : m_pipeMgr(&m), driver(d) {{}}
    VkPipeline getPipelineHandle(const DxvkGraphicsPipelineStateInfo&, const DxvkRenderPass*);
    void compilePipeline(const DxvkGraphicsPipelineStateInfo&, const DxvkRenderPass*);
    VkPipeline get(const State& state, const DxvkRenderPass* pass) {{ return getPipelineHandle(state, pass); }}
    void build(const State& state, const DxvkRenderPass* pass) {{ compilePipeline(state, pass); }}
    size_t size() {{ std::lock_guard<sync::Spinlock> lock(m_mutex); return m_pipelines.size(); }}
    size_t capacity() {{ std::lock_guard<sync::Spinlock> lock(m_mutex); return m_pipelines.capacity(); }}
    bool list_unlocked() {{ if (!m_mutex.try_lock()) return false; m_mutex.unlock(); return true; }}
    bool flight_idle(const State& state, const DxvkRenderPass* pass) {{
#if PROTOTYPE
        skyrim::GraphicsFlights<State, DxvkRenderPass>::Ticket probe(m_compileFlights, state, pass);
        auto lock = m_compileFlights.lock();
        return !probe.busy(lock);
#else
        (void)state; (void)pass;
        if (!m_compileMutex.try_lock()) return false;
        m_compileMutex.unlock(); return true;
#endif
    }}
private:
    DxvkPipelineManager* m_pipeMgr;
    Driver& driver;
{fields}
    {result_type} createInstance(const State&, const DxvkRenderPass*);
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
              "-I", str(EXPERIMENT)]
    for path in ("src", "src/dxvk", "src/util", "include", "include/native",
                 "include/native/windows", "include/native/directx"):
        result += ["-I", str(stage / path)]
    return result


class GraphicsStateFlightAdapterTests(unittest.TestCase):
    def test_identity_gpu_creation_and_remaining_methods_are_unchanged(self):
        path = "src/dxvk/dxvk_graphics.cpp"
        old = adapter.adapt(path, (SOURCE / path).read_text(encoding="utf-8"))
        new = experiment.patched(path, old)
        marker = "  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::findInstance("
        self.assertEqual(old[old.index(marker):], new[new.index(marker):])
        path = "src/dxvk/dxvk_graphics.h"
        old = adapter.adapt(path, (SOURCE / path).read_text(encoding="utf-8"))
        new = experiment.patched(path, old)
        self.assertEqual(actual_instance(old), actual_instance(new))
        self.assertIn("m_renderPass  == rp", actual_instance(new))
        self.assertIn("m_stateVector == state", actual_instance(new))

    def test_exact_baseline_required_and_patch_is_idempotent(self):
        for name in experiment.NAMES:
            with self.subTest(name=name):
                raw = (SOURCE / name).read_text(encoding="utf-8")
                old = adapter.adapt(name, raw)
                new = experiment.patched(name, old)
                self.assertEqual(experiment.patched(name, new), new)
                for changed in (raw, old + "\n", old.replace("m_pipelines", "changed_pipelines")):
                    with self.assertRaises(ValueError):
                        experiment.patched(name, changed)
        with self.assertRaises(ValueError):
            experiment.patched("src/dxvk/dxvk_compute.cpp", "")

    def test_apply_changes_only_two_staged_files_and_helper(self):
        with tempfile.TemporaryDirectory(prefix="graphics-flights-stage-") as tmp:
            stage = Path(tmp) / "dxvk"
            adapter.stage(SOURCE, stage)
            before = {p.relative_to(stage).as_posix(): p.read_bytes() for p in stage.rglob("*") if p.is_file()}
            experiment.apply(stage)
            after = {p.relative_to(stage).as_posix(): p.read_bytes() for p in stage.rglob("*") if p.is_file()}
            changed = {name for name in before.keys() | after.keys() if before.get(name) != after.get(name)}
            self.assertEqual(changed, {*experiment.NAMES, "src/dxvk/graphics_flights.h"})
            experiment.apply(stage)
            self.assertEqual(after, {p.relative_to(stage).as_posix(): p.read_bytes() for p in stage.rglob("*") if p.is_file()})

    def test_drift_is_rejected_before_any_staged_write(self):
        with tempfile.TemporaryDirectory(prefix="graphics-flights-drift-") as tmp:
            stage = Path(tmp)
            for name in experiment.NAMES:
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(experiment.baseline(name), encoding="utf-8")
            cpp = stage / "src/dxvk/dxvk_graphics.cpp"
            cpp.write_text(cpp.read_text(encoding="utf-8") + "\n// drift\n", encoding="utf-8")
            before = {name: (stage / name).read_bytes() for name in experiment.NAMES}
            with self.assertRaises(ValueError):
                experiment.apply(stage)
            self.assertEqual(before, {name: (stage / name).read_bytes() for name in experiment.NAMES})
            self.assertFalse((stage / "src/dxvk/graphics_flights.h").exists())

    def test_source_trees_are_refused_and_production_adapter_is_independent(self):
        for path in (ROOT, ROOT / "vendor", SOURCE):
            with self.subTest(path=path), self.assertRaises(ValueError):
                experiment.apply(path)
        production = (ROOT / "ios/dependencies/prepare_dxvk.py").read_text(encoding="utf-8")
        for marker in ("graphics_flights", "dxvk-state-cold", "GraphicsFlights"):
            self.assertNotIn(marker, production)


@unittest.skipUnless(os.name == "posix" and shutil.which(os.environ.get("CXX", "c++")),
                     "Native POSIX compiler required")
class GraphicsStateFlightNativeTests(unittest.TestCase):
    def test_actual_state_lookup_publication_admission_and_failures(self):
        with tempfile.TemporaryDirectory(prefix="graphics-flights-native-") as tmp:
            folder = Path(tmp)
            objects = []
            for name in ("performance_metrics", "device_policy"):
                obj = folder / (name + ".o")
                result = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", *flags(),
                    "-D_POSIX_C_SOURCE=200809L", "-c", str(ROOT / "ios/runtime-port" / (name + ".c")),
                    "-o", str(obj)], capture_output=True, text=True, timeout=40)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                objects.append(str(obj))
            for prototype in (False, True):
                with self.subTest(prototype=prototype):
                    path = folder / "fixture.cpp"
                    path.write_text(fixture(prototype), encoding="utf-8")
                    executable = folder / "fixture"
                    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-g", "-pthread", *flags(),
                        "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-Wno-unused-parameter",
                        "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", f"-DPROTOTYPE={int(prototype)}",
                        *includes(SOURCE), str(path), *objects, "-o", str(executable)]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    command = [str(executable)]
                    if os.environ.get("SKYRIM_TEST_SANITIZERS") == "thread" and os.environ.get("SKYRIM_TSAN_DISABLE_ASLR") == "1":
                        setarch = shutil.which("setarch")
                        self.assertIsNotNone(setarch, "setarch required for requested ThreadSanitizer ASLR control")
                        command = [setarch, platform.machine(), "-R", *command]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    common = ("ready_and_independent_owner", "exact_packed_state_and_pass_identity",
                              "same_identity_single_generation", "cached_null_invalid_absent",
                              "exception_wakes_retry", "reentrant_callback_growth", "eight_threads")
                    for marker in common:
                        self.assertIn(marker + "=PASS", result.stdout)
                    self.assertIn(f"different_state_progress={int(prototype)}", result.stdout)
                    if prototype:
                        for marker in ("publication_failure_cleanup_before_release", "parked_prefetch_owns_no_flight",
                                       "admitted_work_not_preempted", "shutdown_releases_admission"):
                            self.assertIn(marker + "=PASS", result.stdout)
                    print("prototype" if prototype else "production baseline", result.stdout.strip(), flush=True)

    def test_complete_graphics_and_manager_translation_units(self):
        with tempfile.TemporaryDirectory(prefix="graphics-flights-syntax-") as tmp:
            stage = Path(tmp) / "dxvk"
            adapter.stage(SOURCE, stage)
            experiment.apply(stage)
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only", "-include", "cstdint", "-include", "cstring",
                       "-Wno-deprecated-declarations", "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", *includes(stage)]
            for name in ("dxvk_graphics.cpp", "dxvk_pipemanager.cpp"):
                with self.subTest(name=name):
                    result = subprocess.run([*command, str(stage / "src/dxvk" / name)],
                                            capture_output=True, text=True, timeout=90)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
