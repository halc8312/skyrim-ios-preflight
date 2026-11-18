"""Build14 #6 ON through real generated DXVK + patched MoltenVK cache methods.

One executable compares #1 OFF/ON with three authored prefetch requests. Events
establish lock ownership and progress, not a latency threshold. Packed DXVK
state/identity/publication, cache lookup and library publication are production
code; SPIRV-Cross, Metal, and final GPU handles are authored CPU boundaries.
No game input, Vulkan/Metal device, iPhone, or speedup claim is involved.

Requires the pinned SKYRIM_MVK_GPU_SOURCE and a POSIX C/C++ toolchain. Generated
sources and command stdout/stderr are retained under local/tests, including
failures. SKYRIM_TEST_SANITIZERS=1 or thread selects the existing sanitizer mode.
"""
import hashlib
import importlib.util
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "tests/native"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


graphics = load("graphics_integration_source", ROOT / "tests/test_graphics_state_flights.py")
mvk = load("mvk_integration_source", ROOT / "tests/test_mvk_module_cold.py")


def generated_graphics():
    existing = graphics.fixture()
    first = "namespace dxvk {\nstruct DxvkPipelineManager {"
    last = "using Manager = dxvk::DxvkPipelineManager;"
    begin = existing.index(first)
    definitions = existing[begin:existing.index(last, begin) + len(last)]
    # Reuse the pinned state, original instance class, real entry points and
    # observer-only flight hooks from the existing production fixture.
    authored = (NATIVE / "test_graphics_state_flights.cpp").read_text(encoding="utf-8")
    prefix = authored[:authored.index("struct Driver {")]
    source = (NATIVE / "test_graphics_mvk_cold_integration.cpp").read_text(encoding="utf-8")
    assert source.count("// REUSED_DXVK_FIXTURE_TYPES") == 1
    assert source.count("// ACTUAL_DXVK_METHODS") == 1
    return source.replace("// REUSED_DXVK_FIXTURE_TYPES", prefix).replace("// ACTUAL_DXVK_METHODS", definitions)


def generate_mvk(source, folder, patched):
    utility = source.parent / "Utility"
    base = (utility / "MVKBaseObject.h").read_bytes()
    assert hashlib.sha256(base).hexdigest() == "da0fabfafc78719f24de512c542bbe13605174bdd0f9115c924c8d117feeeb15"
    mixin = mvk.region(base.decode(), "class MVKConfigurableMixin {", "#pragma mark -")
    vendor = folder / "vendor"
    vendor.mkdir()
    for name, digest in mvk.VECTOR_PINS.items():
        data = (utility / name).read_bytes()
        assert hashlib.sha256(data).hexdigest() == digest
        (vendor / name).write_bytes(data)
    (vendor / "MVKFoundation.h").write_text("#pragma once\ntemplate<class T> class MVKArrayRef;\n", encoding="utf-8")
    header = mvk.fixture_header(mixin)
    anchor = "  void waitAt(int key,bool library) {"
    assert header.count(anchor) == 1
    header = header.replace(anchor, anchor + "\n    MvkIntegration::boundary(key, library);")
    (folder / "mvk_integration_fixture.h").write_text(header, encoding="utf-8")

    pipeline = patched["MVKPipeline.mm"].decode()
    shader = patched["MVKShaderModule.mm"].decode()
    sync = patched["MVKSync.mm"].decode()
    methods = (
        mvk.region(pipeline, "static bool mvkIOSModuleColdEnabled()", "MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(")
        + mvk.region(pipeline, "MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(", "#pragma mark Streaming pipeline")
        + mvk.region(pipeline, "void MVKPipelineCache::markDirty()", "VkResult MVKPipelineCache::mergePipelineCaches(")
        + mvk.region(shader, "MVKShaderLibrary* MVKShaderLibraryCache::getShaderLibrary(", "// Adds and returns a new shader library configured from contents")
        + mvk.region(shader, "bool MVKShaderModule::convert(", "void MVKShaderModule::setWorkgroupSize(")
        + mvk.region(sync, "void MVKMetalCompiler::handleError()", "// Returns whether the compilation came in late")
    )
    anchor = "    if (!compileLock.try_lock()) {"
    assert methods.count(anchor) == 1
    hook = "\n        MvkIntegration::coldWait();"
    observed = methods.replace(anchor, anchor + hook)
    assert observed.replace(hook, "") == methods
    authored = (NATIVE / "mvk_graphics_integration_backend.cpp").read_text(encoding="utf-8")
    assert authored.count("// ACTUAL_MVK_METHODS") == 1
    return authored.replace("// ACTUAL_MVK_METHODS", observed)


class GraphicsMvkColdIntegrationTests(unittest.TestCase):
    def test_actual_graphics_entries_and_cache_global_vs_module(self):
        source = os.environ.get("SKYRIM_MVK_GPU_SOURCE")
        if (os.name != "posix" or not source or not shutil.which(os.environ.get("CXX", "c++"))
                or not shutil.which(os.environ.get("CC", "cc"))):
            self.skipTest("POSIX C/C++ toolchain and pinned SKYRIM_MVK_GPU_SOURCE required")
        # An explicitly supplied path is a requirement, not a reason to silently
        # skip corrupt/mismatched upstream sources. patched() checks their pins.
        originals = {name: (Path(source) / name).read_bytes() for name in mvk.adapter.NAMES}
        patched = mvk.adapter.patched(originals)
        if not graphics.SOURCE.is_dir():
            self.skipTest("Pinned DXVK dependency checkout required")
        output = ROOT / "local/tests"
        output.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="graphics-mvk-integration-", dir=output))
        print(f"Integration evidence: {folder}", flush=True)
        unit = folder / "graphics.cpp"
        unit.write_text(generated_graphics(), encoding="utf-8")
        backend = folder / "mvk.cpp"
        backend.write_text(generate_mvk(Path(source), folder, patched), encoding="utf-8")
        sequence = 0

        def run(command, env, timeout):
            nonlocal sequence
            sequence += 1
            log = folder / "execution.log"
            # One diagnostic transcript, without per-command receipt files or
            # the process environment. Preserve output even on a timeout.
            with log.open("a", encoding="utf-8") as stream:
                stream.write(f"\n[{sequence}] {shlex.join([str(value) for value in command])}\n"
                             f"exact_state={env.get('SKYRIM_EXACT_GRAPHICS_STATE')} "
                             f"module_cold={env.get('SKYRIM_MVK_MODULE_COLD')}\n")
                stream.flush()
                try:
                    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=timeout)
                except subprocess.TimeoutExpired as error:
                    for partial in (error.stdout, error.stderr):
                        if partial:
                            stream.write(partial.decode(errors="replace") if isinstance(partial, bytes) else partial)
                    stream.write(f"\nTIMEOUT after {timeout}s\n")
                    raise
                stream.write(result.stdout + result.stderr + f"\nexit_code={result.returncode}\n")
            self.assertEqual(result.returncode, 0, f"Evidence {log}\n{result.stdout}\n{result.stderr}")
            return result.stdout, result.stderr

        env = graphics.native_environment("1")
        env.pop("SKYRIM_MVK_MODULE_COLD", None)
        objects = []
        for name in ("performance_metrics", "device_policy"):
            obj = folder / f"{name}.o"
            run([os.environ.get("CC", "cc"), "-std=c11", "-O2", *graphics.flags(), *graphics.macos_flags(),
                 "-D_POSIX_C_SOURCE=200809L", "-c", str(ROOT / "ios/runtime-port" / f"{name}.c"), "-o", str(obj)], env, 45)
            objects.append(str(obj))
        executable = folder / "integration"
        run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-g", "-pthread",
             *graphics.flags(), *graphics.macos_flags(), "-Wall", "-Wextra", "-Werror",
             "-Wno-deprecated-declarations", "-Wno-unused-parameter", "-Wno-unknown-pragmas",
             "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", *graphics.includes(graphics.SOURCE),
             "-I", str(NATIVE), "-I", str(folder), "-isystem", str(folder / "vendor"),
             str(unit), str(backend), *objects, "-o", str(executable)], env, 90)
        command = [str(executable)]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "thread" and os.environ.get("SKYRIM_TSAN_DISABLE_ASLR") == "1":
            setarch = shutil.which("setarch")
            self.assertIsNotNone(setarch, "setarch required for explicitly requested process-local TSan ASLR control")
            command = [setarch, platform.machine(), "-R", *command]
        for mode in ("0", "1"):
            with self.subTest(module_cold=mode):
                env["SKYRIM_MVK_MODULE_COLD"] = mode
                stdout, stderr = run([*command, mode], env, 60)
                print(stdout.strip(), flush=True)
                self.assertIn(f"exact_state=1 module_cold={mode} prefetch_at_metal_before_release={1 if mode == '0' else 3} independent_draw_progress={mode}", stdout)
                for marker in ("ready_hits", "same_dxvk_state_single_generation", "same_mvk_key_single_generation", "same_module_serialization", "cache_lifetime"):
                    self.assertIn(marker + "=PASS", stdout)
                self.assertIn("gpu_or_game_performance_verified=0", stdout)
                self.assertEqual(stderr.count("IOS_GRAPHICS_POLICY "), 1)
                self.assertIn("exact_state=1 wait_scope=1", stderr)
                self.assertEqual(stderr.count("IOS_SHADER_POLICY "), 1)
                self.assertIn(f"module_cold={mode} cold_scope={mode}", stderr)


if __name__ == "__main__":
    unittest.main()
