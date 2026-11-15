#!/usr/bin/env python3
"""Compose the pinned shader adapters with a default-OFF module-cold switch.

SKYRIM_MVK_MODULE_COLD=1 selects per-module cold serialization at runtime. The
process keeps that decision after first use; all other values retain the global
cold mutex. Ready hits, the cache format and Metal completion rules are unchanged.
"""
import argparse
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "ios/dependencies" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


hot = load("patch_mvk_hot_cache")
library = load("patch_mvk_library")
timing = load("patch_mvk_compile_timing")
NAMES = ("MVKPipeline.h", "MVKPipeline.mm", "MVKShaderModule.h", "MVKShaderModule.mm", "MVKSync.mm")


def once(raw, old, new):
    if raw.count(old) != 1:
        raise ValueError("Module-cold adapter anchor differs: " + old[:90].decode())
    return raw.replace(old, new)


POLICY_METHOD = b'''// Startup-only policy: set the environment before renderer creation. Keep one
// immutable process decision so different callers cannot use different locks.
static bool mvkIOSModuleColdEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SKYRIM_MVK_MODULE_COLD");
        const bool selected = value && std::strcmp(value, "1") == 0;
        std::fprintf(stderr, "IOS_SHADER_POLICY schema=1 module_cold=%u cold_scope=%u\\n",
                     unsigned(selected), unsigned(selected));
        return selected;
    }();
    return enabled;
}

'''


ERROR_METHOD = b'''\t// Only compiler failure publication takes this short catalog lock.
\t// Never retain it across compilation or a Metal completion wait.
\tvoid recordShaderCompilationResult(VkResult result) {
\t\tstd::lock_guard<std::mutex> lock(_shaderCacheLock);
\t\tsetConfigurationResult(result);
\t}
'''


def baseline(pristine):
    return {
        **{name: hot.patched(name, pristine[name]) for name in hot.PINS},
        **{name: library.patched(name, pristine[name]) for name in library.PINS},
        "MVKSync.mm": timing.patched(pristine["MVKSync.mm"]),
    }


def patched(pristine):
    result = baseline(pristine)
    result["MVKPipeline.h"] = once(result["MVKPipeline.h"],
        b"class MVKPipelineCache : public MVKVulkanAPIDeviceObject {\n\npublic:\n",
        b"class MVKPipelineCache : public MVKVulkanAPIDeviceObject {\n\npublic:\n\n" + ERROR_METHOD)
    result["MVKPipeline.mm"] = once(result["MVKPipeline.mm"],
        b"unique_lock<mutex> compileLock(_shaderCompileLock, defer_lock);",
        b"unique_lock<mutex> compileLock(mvkIOSModuleColdEnabled() ? slCache->_moduleColdLock : _shaderCompileLock, defer_lock);")
    result["MVKPipeline.mm"] = once(result["MVKPipeline.mm"],
        b"// Keep one cold compilation per cache, but let already prepared libraries and",
        b"// Keep cold work global by default, or per module when explicitly enabled; prepared libraries and")
    result["MVKPipeline.mm"] = once(result["MVKPipeline.mm"],
        b"#include <memory>", b"#include <memory>\n#include <cstdlib>\n#include <cstring>\n#include <cstdio>" )
    marker = b"MVKShaderLibrary* MVKPipelineCache::getShaderLibrary("
    result["MVKPipeline.mm"] = once(result["MVKPipeline.mm"], marker, POLICY_METHOD + marker)
    result["MVKShaderModule.h"] = once(result["MVKShaderModule.h"],
        b"\tMVKSmallVector<std::pair<mvk::SPIRVToMSLConversionConfiguration, MVKShaderLibrary*>> _shaderLibraries;",
        b"\tstd::mutex _moduleColdLock;\n\tMVKSmallVector<std::pair<mvk::SPIRVToMSLConversionConfiguration, MVKShaderLibrary*>> _shaderLibraries;")
    result["MVKShaderModule.h"] = once(result["MVKShaderModule.h"],
        b"\tmvk::SPIRVToMSLConverter _spvConverter;",
        b"\tstd::mutex _conversionLock;\n\tmvk::SPIRVToMSLConverter _spvConverter;")
    convert = result["MVKShaderModule.mm"].index(b"bool MVKShaderModule::convert(")
    config = b"\tconst auto& mvkCfg = getMVKConfig();\n"
    after_config = result["MVKShaderModule.mm"].index(config, convert) + len(config)
    result["MVKShaderModule.mm"] = (result["MVKShaderModule.mm"][:after_config]
        + b"\t// The pinned normal converter owns only immutable SPIR-V; CompilerMSL\n"
          b"\t// and the caller's config/results are per invocation. Keep optional\n"
          b"\t// diagnostics serialized for this module instance only. Equal-content\n"
          b"\t// module instances do not share this diagnostic guard.\n"
          b"\tstd::unique_lock<std::mutex> conversionGuard(_conversionLock, std::defer_lock);\n"
          b"\tif (mvkCfg.debugMode || (mvkCfg.shaderDumpDir && *mvkCfg.shaderDumpDir)) {\n"
          b"\t\tconversionGuard.lock();\n\t}\n"
        + result["MVKShaderModule.mm"][after_config:])
    sync = result["MVKSync.mm"]
    start = sync.index(b"void MVKMetalCompiler::handleError() {")
    end = sync.index(b"\n}\n", start) + 3
    method = sync[start:end]
    method = once(method, b"\t_owner->setConfigurationResult(reportError(", b"\tVkResult result = reportError(")
    method = once(method, b"_compileError.localizedDescription.UTF8String));",
        b"_compileError.localizedDescription.UTF8String);\n"
        b"\tif (_owner->getVkObjectType() == VK_OBJECT_TYPE_PIPELINE_CACHE) {\n"
        b"\t\tstatic_cast<MVKPipelineCache*>(_owner)->recordShaderCompilationResult(result);\n"
        b"\t} else {\n\t\t_owner->setConfigurationResult(result);\n\t}")
    result["MVKSync.mm"] = b'#include "MVKPipeline.h"\n' + sync[:start] + method + sync[end:]
    return result


def apply(source, pristine):
    if source.resolve() == pristine.resolve():
        raise ValueError("Use a separate stage; pristine input must remain unchanged")
    originals = {name: (pristine / name).read_bytes() for name in NAMES}
    base, candidate = baseline(originals), patched(originals)
    for name in NAMES:
        if (source / name).read_bytes() not in (originals[name], base[name], candidate[name]):
            raise ValueError("Unreviewed module-cold input: " + name)
    for name, content in candidate.items():
        (source / name).write_bytes(content)
    (source / "skyrim_performance_metrics.h").write_bytes((ROOT / "ios/runtime-port/performance_metrics.h").read_bytes())


def verify(source, pristine):
    """Verify the final composed stage, including the exact shared metric ABI."""
    originals = {name: (pristine / name).read_bytes() for name in NAMES}
    expected = patched(originals)
    expected["skyrim_performance_metrics.h"] = (ROOT / "ios/runtime-port/performance_metrics.h").read_bytes()
    for name, content in expected.items():
        if (source / name).read_bytes() != content:
            raise ValueError("Composed module-cold stage differs: " + name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--pristine", type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.pristine.resolve():
        parser.error("Use a separate build stage; never edit pristine input")
    apply(args.source, args.pristine)
    print("Pinned module-cold adapter present; runtime default is OFF")
