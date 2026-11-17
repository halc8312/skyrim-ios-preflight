"""Optional Mac Metal validation for the isolated DXVK graphics-flight experiment.

This runs the actual generated coordination methods with an authored Metal driver,
not a full DXVK/MoltenVK renderer. It never enables the production experiment.
Run this module on an authorized Mac to compile and execute both configurations.
SKYRIM_GRAPHICS_METAL_OUTPUT may name a fresh evidence directory; all compilation,
execution and platform logs are retained there, including on failure.
"""
import importlib.util
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("metal_graphics_fixture_source", ROOT / "tests/test_graphics_state_flights.py")
coordination = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coordination)

CASES = {
    "warm_ready": 1,
    "cold_owner": 2,
    "different_state": 3,
    "packed_variant": 10,
    "different_pass": 18,
    "independent_owner": 2,
    "exception_retry": 17,
    "publication_result": 31,
}
CHECKS = (
    "ready_hit", "independent_owner", "same_identity_single_generation",
    "full_state_identity", "render_pass_pointer_identity", "real_metal_cached_null",
    "invalid_state_absent", "exception_retry", "all_pso_references_released",
)


def actual_definitions(prototype):
    """Reuse the established generator, never a second implementation of flights."""
    generated = coordination.fixture(prototype)
    begin = generated.index("\nnamespace dxvk {\nstruct DxvkPipelineManager {")
    marker = "using Manager = dxvk::DxvkPipelineManager;"
    end = generated.index(marker, begin) + len(marker)
    definitions = generated[begin:end]
    anchor = "    DxvkGraphicsPipeline(Driver& d, DxvkPipelineManager& m) : m_pipeMgr(&m), driver(d) {}"
    if definitions.count(anchor) != 1:
        raise ValueError("Established fixture constructor changed")
    definitions = definitions.replace(anchor, anchor + "\n    ~DxvkGraphicsPipeline();")
    # Preserve the pinned destructor too: every cached handle reaches the Metal
    # driver's release path, rather than relying on process exit for cleanup.
    source = (coordination.SOURCE / "src/dxvk/dxvk_graphics.cpp").read_text(encoding="utf-8")
    destructor = coordination.section(source, "  DxvkGraphicsPipeline::~DxvkGraphicsPipeline() {",
                                      "  Rc<DxvkShader> DxvkGraphicsPipeline::getShader(")
    return definitions + "\nnamespace dxvk {\n" + destructor + "}\n"


def fixture(prototype):
    authored = (ROOT / "tests/native/graphics_state_flights_metal.mm").read_text(encoding="utf-8")
    if authored.count("// ACTUAL_DXVK_METHODS") != 1:
        raise ValueError("Metal fixture insertion anchor changed")
    return authored.replace("// ACTUAL_DXVK_METHODS", actual_definitions(prototype))


def verify_report(result, prototype):
    model = "state_flights_prototype" if prototype else "production_baseline"
    if not isinstance(result, dict) or result.get("schema") != 1 or type(result.get("schema")) is not int \
            or result.get("status") != "passed" or result.get("model") != model \
            or result.get("scope") != "mac-metal-driver-fixture" \
            or result.get("pso_ownership_scope") != "fixture-retained-references" \
            or result.get("metal_execution_verified") is not True or result.get("public_only") is not True:
        raise ValueError("Incomplete Metal graphics-flight result")
    for key in ("full_dxvk_moltenvk_verified", "physical_iphone_verified", "game_rendering_verified",
                "performance_verified", "production_enabled"):
        if result.get(key) is not False:
            raise ValueError("Metal driver fixture cannot establish " + key)
    checks = result.get("checks", {})
    if not isinstance(checks, dict) or any(checks.get(name) is not True for name in CHECKS):
        raise ValueError("Missing required coordination or lifetime check")
    if result.get("distinct_state_progress") is not prototype \
            or result.get("publication_allocation_failure_exercised") is not prototype \
            or result.get("cleanup_kept_flight_owned") is not prototype:
        raise ValueError("Incorrect configuration-specific coordination result")
    counts = result.get("counts", {})
    expected_created = 10 if prototype else 9
    if not isinstance(counts, dict) or any(type(counts.get(name)) is not int for name in
            ("pso_created", "pso_released", "pso_live", "pso_rejected", "injected_exceptions", "publication_failures")) \
            or counts.get("pso_created") != expected_created or counts.get("pso_released") != expected_created \
            or counts.get("pso_live") != 0 or counts.get("pso_rejected") != 1 \
            or counts.get("injected_exceptions") != 1 or counts.get("publication_failures") != int(prototype):
        raise ValueError("Metal pipeline ownership or failure counts do not balance")
    cases = result.get("cases")
    if not isinstance(cases, list) or len(cases) != len(CASES):
        raise ValueError("Missing Metal render cases")
    seen = set()
    for case in cases:
        if not isinstance(case, dict):
            raise ValueError("Invalid Metal render case")
        name = case.get("case")
        if name not in CASES or name in seen or case.get("matches") is not True:
            raise ValueError("Unknown, repeated or failed Metal render case")
        seen.add(name)
        bits = case.get("bits")
        if not isinstance(bits, list) or len(bits) != 4 or any(type(word) is not int or not 0 <= word <= 0xffffffff for word in bits):
            raise ValueError("Invalid Metal readback bits")
        token = CASES[name]
        expected = [float(token), float(token + 1), float(token + 2), 1.0]
        actual = [struct.unpack("<f", struct.pack("<I", word))[0] for word in bits]
        if actual != expected:
            raise ValueError("Metal readback differs from independently authored expected colors")


def _run_logged(command, output, stem, env, timeout):
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        def decoded(value):
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""
        (output / (stem + ".log")).write_text(decoded(error.stdout) + decoded(error.stderr) + "\nTIMEOUT\n", encoding="utf-8")
        raise RuntimeError("Metal fixture timed out; evidence retained at " + str(output)) from error
    (output / (stem + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Metal fixture {stem} failed ({result.returncode}); evidence: {output}\n" + result.stderr[-6000:])
    return result


def run_metal(output):
    if platform.system() != "Darwin":
        raise RuntimeError("A Mac with Apple SDK and Metal device is required; no GPU verification performed")
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop("IPHONEOS_DEPLOYMENT_TARGET", None)
    env.pop("SDKROOT", None)
    objects = []
    for name in ("performance_metrics", "device_policy"):
        obj = output / (name + ".o")
        _run_logged(["xcrun", "--sdk", "macosx", "clang", "-std=c11", "-O2", "-mmacosx-version-min=14.0",
            "-D_POSIX_C_SOURCE=200809L", "-c", str(ROOT / "ios/runtime-port" / (name + ".c")), "-o", str(obj)],
            output, name + ".compile", env, 60)
        objects.append(str(obj))
    results = []
    for prototype in (False, True):
        model = "prototype" if prototype else "baseline"
        source = output / (model + ".mm")
        source.write_text(fixture(prototype), encoding="utf-8")
        binary = output / (model + "-metal")
        _run_logged(["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O2", "-g", "-pthread", "-fobjc-arc",
            "-mmacosx-version-min=14.0", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-deprecated-declarations",
            "-DDXVK_NATIVE", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", f"-DPROTOTYPE={int(prototype)}",
            *coordination.includes(coordination.SOURCE), str(source), *objects,
            "-framework", "Foundation", "-framework", "Metal", "-o", str(binary)],
            output, model + ".compile", env, 150)
        build = _run_logged(["xcrun", "vtool", "-show-build", str(binary)], output, model + ".platform", env, 20)
        if "platform MACOS" not in build.stdout:
            raise RuntimeError("Metal fixture was not built for macOS; evidence: " + str(output))
        run = _run_logged([str(binary)], output, model + ".run", env, 240)
        (output / (model + ".result.json")).write_text(run.stdout, encoding="utf-8")
        report = json.loads(run.stdout)
        verify_report(report, prototype)
        results.append(report)
    readbacks = [{case["case"]: case["bits"] for case in report["cases"]} for report in results]
    if readbacks[0] != readbacks[1]:
        raise RuntimeError("Baseline/prototype Metal readbacks differ; evidence: " + str(output))
    print("GRAPHICS_STATE_METAL: baseline/prototype 8 identical authored Mac GPU readbacks; "
          "coordination and PSO ownership passed; full DXVK/MoltenVK, iPhone and performance unverified", flush=True)
    print("Metal evidence: " + str(output), flush=True)
    return results


class GraphicsStateFlightMetalSourceTests(unittest.TestCase):
    def test_reuses_actual_generated_methods_instance_state_and_destructor(self):
        for prototype in (False, True):
            with self.subTest(prototype=prototype):
                generated = fixture(prototype)
                self.assertIn('#include "dxvk/dxvk_graphics_state.h"', generated)
                definitions = actual_definitions(prototype)
                self.assertIn(definitions, generated)
                marker = "  VkPipeline DxvkGraphicsPipeline::getPipelineHandle("
                end = "using TestPipeline = dxvk::DxvkGraphicsPipeline;"
                self.assertEqual(coordination.section(generated, marker, end),
                                 coordination.section(coordination.fixture(prototype), marker, end))
                self.assertNotIn("// ACTUAL_DXVK_METHODS", generated)
                self.assertIn("this->destroyPipeline(instance.pipeline());", definitions)
                self.assertIn("newRenderPipelineStateWithDescriptor", generated)
                self.assertIn("command.status == MTLCommandBufferStatusCompleted", generated)

    def test_report_validation_rejects_missing_readback_leaks_and_scope_inflation(self):
        # Synthetic parser input only; this is never emitted as GPU evidence.
        for prototype in (False, True):
            report = {
                "schema": 1, "status": "passed", "model": "state_flights_prototype" if prototype else "production_baseline",
                "scope": "mac-metal-driver-fixture", "pso_ownership_scope": "fixture-retained-references",
                "metal_execution_verified": True, "public_only": True,
                **{key: False for key in ("full_dxvk_moltenvk_verified", "physical_iphone_verified", "game_rendering_verified",
                                         "performance_verified", "production_enabled")},
                "checks": {name: True for name in CHECKS}, "distinct_state_progress": prototype,
                "publication_allocation_failure_exercised": prototype, "cleanup_kept_flight_owned": prototype,
                "counts": {"pso_created": 10 if prototype else 9, "pso_released": 10 if prototype else 9,
                           "pso_live": 0, "pso_rejected": 1, "injected_exceptions": 1, "publication_failures": int(prototype)},
                "cases": [{"case": name, "matches": True,
                           "bits": [struct.unpack("<I", struct.pack("<f", value))[0] for value in (token, token + 1, token + 2, 1)]}
                          for name, token in CASES.items()],
            }
            verify_report(report, prototype)
            for mutation in ("pixels", "missing", "duplicate", "leak", "scope", "unchecked", "wrong_model"):
                broken = json.loads(json.dumps(report))
                if mutation == "pixels": broken["cases"][0]["bits"][0] = 0
                if mutation == "missing": broken["cases"].pop()
                if mutation == "duplicate": broken["cases"][1] = broken["cases"][0]
                if mutation == "leak": broken["counts"]["pso_released"] -= 1
                if mutation == "scope": broken["physical_iphone_verified"] = True
                if mutation == "unchecked": broken["checks"].pop("real_metal_cached_null")
                if mutation == "wrong_model": broken["distinct_state_progress"] = not prototype
                with self.subTest(prototype=prototype, mutation=mutation), self.assertRaises(ValueError):
                    verify_report(broken, prototype)


@unittest.skipUnless(platform.system() == "Darwin", "Real Metal execution requires a Mac")
class GraphicsStateFlightMetalNativeTests(unittest.TestCase):
    def test_real_metal_pipeline_coordination_and_readback(self):
        configured = os.environ.get("SKYRIM_GRAPHICS_METAL_OUTPUT")
        # Retain evidence even on compilation/device failure. Missing Apple tools
        # or GPU on Darwin is a failure, not a successful skipped requirement.
        output = Path(configured) if configured else Path(tempfile.mkdtemp(prefix="graphics-metal-evidence-")) / "run"
        run_metal(output)


if __name__ == "__main__":
    unittest.main()
