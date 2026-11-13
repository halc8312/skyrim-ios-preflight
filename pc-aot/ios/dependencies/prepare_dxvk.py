#!/usr/bin/env python3
"""Stage the pinned public DXVK tree with iOS host-only adapters.

Never edit vendor, CPU translation, or private inputs. Fingerprints use LF just
like prepare_runtime.py; unmodified files retain their original bytes.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path

EXPECTED = {
    "src/dxvk/dxvk_graphics.cpp": "dc35eb99f52d3dc4ea3e033fbe55c4a06faae8201c3424bf552aefcbc4e509ca",
    "src/dxvk/dxvk_compute.cpp": "2515fefb8ae4acee0fe9ba6fd3a36eda8664fa9bcbb15784a83b39763c26ee0e",
    "src/dxvk/dxvk_shader.cpp": "8d6c02908f7b217039bf7d80e8ac5317bf8a827dfc54d29a88a5414103d05063",
    "src/dxvk/dxvk_pipecache.cpp": "a7d0a391aa19fa8f560487843b9a2d83e492c87a710914f76c4807640829e070",
    "src/dxvk/dxvk_pipecache.h": "b0dd5f8120da50e0426e1c84b13e9e169fc9088b50e1fc906f12c2ca0f72caeb",
    "src/dxvk/dxvk_pipemanager.cpp": "7f6a912d006624cf5236eebeea3ef211664e380c4595f69e6e6efeac49cd0c8a",
}


def once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise ValueError("DXVK adapter anchor changed")
    return text.replace(old, new, 1)


def adapt(name: str, text: str) -> str:
    if name.endswith("dxvk_pipemanager.cpp"):
        return once(text, "new DxvkPipelineCache(device->vkd())",
            "new DxvkPipelineCache(device->vkd(), device->adapter()->deviceProperties())")
    if name.endswith("dxvk_pipecache.h"):
        text = '#include "pipeline_cache.h"\n#include <memory>\n' + text
        text = once(text, "DxvkPipelineCache(const Rc<vk::DeviceFn>& vkd);",
            "DxvkPipelineCache(const Rc<vk::DeviceFn>& vkd, const VkPhysicalDeviceProperties& properties);\n"
            "    void compiled() { m_disk->compiled(); }")
        return once(text, "VkPipelineCache         m_handle;",
            "VkPipelineCache         m_handle;\n    std::unique_ptr<skyrim::PipelineCache> m_disk;")
    if name.endswith("dxvk_pipecache.cpp"):
        return '''#include "dxvk_pipecache.h"
#include <cstdlib>
namespace dxvk {
  DxvkPipelineCache::DxvkPipelineCache(const Rc<vk::DeviceFn>& vkd,
      const VkPhysicalDeviceProperties& properties) : m_vkd(vkd) {
    m_disk = std::make_unique<skyrim::PipelineCache>(m_vkd->device(),
        m_vkd->vkCreatePipelineCache, m_vkd->vkGetPipelineCacheData,
        m_vkd->vkDestroyPipelineCache, properties,
        std::getenv("SKYRIM_VK_PIPELINE_CACHE_PATH"), SKYRIM_PIPELINE_CACHE_NAMESPACE);
    if (m_disk->result() != VK_SUCCESS)
      throw DxvkError("DxvkPipelineCache: Failed to create cache");
    m_handle = m_disk->handle();
  }
  DxvkPipelineCache::~DxvkPipelineCache() = default;
}
'''
    text = '#include "performance_metrics.h"\n' + text
    if name.endswith("dxvk_shader.cpp"):
        call = "m_vkd->vkCreateShaderModule(m_vkd->device(), &info, nullptr, &m_stage.module)"
        kind = "SKYRIM_PERF_SHADER_MODULE"
    else:
        graphics = name.endswith("dxvk_graphics.cpp")
        call = ("m_vkd->vkCreate" + ("Graphics" if graphics else "Compute") + "Pipelines(m_vkd->device(),\n"
                "          m_pipeMgr->m_cache->handle(), 1, &info, nullptr, &pipeline)")
        kind = "SKYRIM_PERF_GRAPHICS_PIPELINE" if graphics else "SKYRIM_PERF_COMPUTE_PIPELINE"
    dirty = "" if name.endswith("dxvk_shader.cpp") else "    if (iosResult == VK_SUCCESS) m_pipeMgr->m_cache->compiled();\n"
    return once(text, "    if (" + call + " != VK_SUCCESS)",
        "    const uint64_t iosStarted = skyrim_perf_now_ns();\n"
        "    const VkResult iosResult = " + call + ";\n"
        "    skyrim_perf_end(" + kind + ", iosStarted, iosResult != VK_SUCCESS);\n"
        + dirty +
        "    if (iosResult != VK_SUCCESS)")


def prepare(source: Path) -> dict[str, bytes]:
    replacements = {}
    for name, expected in EXPECTED.items():
        path = source / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Missing/linked DXVK input: " + name)
        text = path.read_text(encoding="utf-8")
        if hashlib.sha256(text.encode()).hexdigest() != expected:
            raise ValueError("Changed DXVK input: " + name)
        replacements[name] = adapt(name, text).encode()
    result = {}
    for path in sorted(source.rglob("*")):
        if ".git" in path.relative_to(source).parts:
            continue
        if path.is_symlink():
            raise ValueError("Linked public DXVK source")
        if path.is_file():
            name = path.relative_to(source).as_posix()
            result[name] = replacements.get(name, path.read_bytes())
    return result


def stage(source: Path, output: Path) -> None:
    source, output = source.resolve(strict=True), output.resolve()
    if source == output or source in output.parents or output in source.parents:
        raise ValueError("DXVK stage must be disjoint from source")
    contents = prepare(source)
    if output.exists():
        existing = {}
        for path in output.rglob("*"):
            if path.is_symlink():
                raise ValueError("Linked existing DXVK stage")
            if path.is_file():
                existing[path.relative_to(output).as_posix()] = path.read_bytes()
        if existing != contents:
            raise ValueError("Existing DXVK stage differs; use a new build directory")
        return
    output.mkdir(parents=True)
    for name, data in contents.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    stage(args.source, args.output)
