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
    "src/d3d9/d3d9_shader.cpp": "40e72c614732f25b99ba298fa3a3fd3dfec2cbbb771e05f4b6821b5478d166b4",
    "src/d3d9/d3d9_shader.h": "0817969cc9b551287a87a8cbe66e209535c15e39551528388a6a9f442fd9ad9d",
    "src/dxvk/dxvk_resource.h": "1dc5c873dc7207534ccd0201b2ab3509fcf0c3d71a86bbbfeb46cf700760b835",
    "src/dxvk/dxvk_resource.cpp": "bcb6bc7f6a37a32ae257e1d18cd1b33f8da00b02fe1c3e12b30a5386823a6bf4",
    "src/dxvk/dxvk_memory.cpp": "1432d9264ecd19159ef02af2de8767b31e19fa65d7e0653d16957ef2fd241631",
    "src/dxvk/dxvk_memory.h": "4aeee77488305734c8b559ec6c53309b048f47b197ad4532b36b5a7e3ccb57e4",
    "src/dxvk/dxvk_state_cache.cpp": "5b0942b71353c1731710f40a6f9573c8371c4cad65c661598aef06771b87026d",
    "src/dxvk/dxvk_graphics.cpp": "dc35eb99f52d3dc4ea3e033fbe55c4a06faae8201c3424bf552aefcbc4e509ca",
    "src/dxvk/dxvk_compute.cpp": "2515fefb8ae4acee0fe9ba6fd3a36eda8664fa9bcbb15784a83b39763c26ee0e",
    "src/dxvk/dxvk_graphics.h": "f2fb63084bd9757262c641c6e4e20a4a5ff223edacc4ef23eae2d486725ddf88",
    "src/dxvk/dxvk_compute.h": "41ae5eabc882bbd58918905838bfe7b5e0b9f7551fe482ddfa740e68529b4148",
    "src/dxvk/dxvk_shader.cpp": "8d6c02908f7b217039bf7d80e8ac5317bf8a827dfc54d29a88a5414103d05063",
    "src/dxvk/dxvk_pipecache.cpp": "a7d0a391aa19fa8f560487843b9a2d83e492c87a710914f76c4807640829e070",
    "src/dxvk/dxvk_pipecache.h": "b0dd5f8120da50e0426e1c84b13e9e169fc9088b50e1fc906f12c2ca0f72caeb",
    "src/dxvk/dxvk_pipemanager.cpp": "7f6a912d006624cf5236eebeea3ef211664e380c4595f69e6e6efeac49cd0c8a",
}


def once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise ValueError("DXVK adapter anchor changed")
    return text.replace(old, new, 1)


def adapt_memory(name: str, text: str) -> str:
    if name.endswith('.h'):
        text = once(text, '    ~DxvkMemoryChunk();', '''    ~DxvkMemoryChunk();

    bool iosEmpty() const {
      return m_freeList.size() == 1 && m_freeList[0].offset == 0
          && m_freeList[0].length == m_memory.memSize;
    }
    VkDeviceSize iosSize() const { return m_memory.memSize; }
    bool iosSamePool(const DxvkMemoryChunk& other) const {
      return m_memory.memFlags == other.m_memory.memFlags
          && m_memory.priority == other.m_memory.priority;
    }''')
        return once(text, '    DxvkMemory tryAlloc(', '''    uint64_t m_iosBudgetPressureUntil = 0;
    uint64_t m_iosNextWarningTrim = 0;
    void iosTrimEmptyChunks(bool all);
    VkDeviceSize iosPoolSize(DxvkMemoryType* type, VkDeviceSize minimum);

    DxvkMemory tryAlloc(''')
    text = '#include "device_policy.h"\n#include "performance_metrics.h"\n' + text
    text = once(text, '    // Try to allocate from a memory type which supports the given flags exactly',
        '''    const uint64_t iosNow = skyrim_perf_now_ns();
    if (skyrim_device_memory_pressure(iosNow) && iosNow >= m_iosNextWarningTrim) {
      iosTrimEmptyChunks(true);
      m_iosNextWarningTrim = iosNow <= UINT64_MAX - 100000000ull ? iosNow + 100000000ull : UINT64_MAX;
    }

    // Try to allocate from a memory type which supports the given flags exactly''')
    text = once(text, '''    // This call is technically not thread-safe, but it
    // doesn't need to be since we don't free chunks''',
        '''    // Pool retirement holds the allocator mutex. Allocator destruction
    // occurs only after all resource slices have ended their lifetimes.''')
    text = once(text, '    if (size >= type->chunkSize || dedAllocInfo) {',
        '    if (size >= type->chunkSize || align > type->chunkSize || dedAllocInfo) {')
    text = once(text, '''        for (uint32_t i = 0; i < 6 && (type->chunkSize >> i) >= size && !devMem.memHandle; i++)
          devMem = tryAllocDeviceMemory(type, flags, type->chunkSize >> i, priority, nullptr);''',
        '''        const VkDeviceSize minimum = dxvk::align(size, align);
        const VkDeviceSize poolSize = iosPoolSize(type, minimum);
        for (uint32_t i = 0; i < 6 && (poolSize >> i) >= minimum && !devMem.memHandle; i++)
          devMem = tryAllocDeviceMemory(type, flags, poolSize >> i, priority, nullptr);''')
    text = once(text, '    chunk->free(offset, length);', '''    chunk->free(offset, length);
    // Memory slices are released by DXVK after their resource lifetime ends.
    // A whole empty block has no live slice; never free partly occupied blocks.
    if (chunk->iosEmpty()) {
      const uint64_t now = skyrim_perf_now_ns();
      iosTrimEmptyChunks(skyrim_device_memory_pressure(now)
          || (m_iosBudgetPressureUntil && now < m_iosBudgetPressureUntil));
    }''')
    marker = '  VkDeviceSize DxvkMemoryAllocator::pickChunkSize(uint32_t memTypeId) const {'
    methods = '''  void DxvkMemoryAllocator::iosTrimEmptyChunks(bool all) {
    // Caller holds m_mutex. A chunk destructor updates the same heap statistics.
    for (uint32_t t = 0; t < m_memProps.memoryTypeCount; ++t) {
      auto& chunks = m_memTypes[t].chunks;
      for (size_t i = 0; i < chunks.size();) {
        if (!chunks[i]->iosEmpty()) { ++i; continue; }
        bool duplicate = false;
        if (!all) {
          for (size_t j = 0; j < chunks.size(); ++j)
            if (j != i && chunks[j]->iosEmpty() && chunks[j]->iosSamePool(*chunks[i])
                && (chunks[j]->iosSize() > chunks[i]->iosSize()
                    || (chunks[j]->iosSize() == chunks[i]->iosSize() && j < i))) {
              duplicate = true; break;
            }
        }
        if (all || duplicate) {
          const VkDeviceSize bytes = chunks[i]->iosSize();
          chunks.erase(chunks.begin() + i);
          skyrim_device_record_reclaimed(bytes);
        } else {
          ++i; // Retain the largest warm block for each flags/priority bucket.
        }
      }
    }
  }

  VkDeviceSize DxvkMemoryAllocator::iosPoolSize(DxvkMemoryType* type, VkDeviceSize minimum) {
    const bool hasBudget = bool(m_device->extensions().extMemoryBudget);
    VkDeviceSize budget = 0, usage = 0;
    if (hasBudget) {
      const auto heaps = m_device->adapter()->getMemoryHeapInfo();
      budget = heaps.heaps[type->heapId].memoryBudget;
      usage = heaps.heaps[type->heapId].memoryAllocated;
    }
    uint64_t available = 0;
    const bool known = skyrim_device_platform_headroom(&available) != 0;
    const uint64_t now = skyrim_perf_now_ns();
    const bool warning = skyrim_device_memory_pressure(now) != 0;
    VkDeviceSize size = skyrim_device_pool_size(type->chunkSize, minimum,
        budget, usage, hasBudget, available, known, warning);
    if (size < type->chunkSize || warning) {
      m_iosBudgetPressureUntil = now <= UINT64_MAX - 2000000000ull ? now + 2000000000ull : UINT64_MAX;
      iosTrimEmptyChunks(true);
    } else {
      m_iosBudgetPressureUntil = 0;
    }
    skyrim_device_record_pool(type->chunkSize, size);
    // The driver still decides required allocation success. A stale/advisory
    // estimate cannot discard a draw or create a new hard allocation failure.
    return size;
  }


'''
    return once(text, marker, methods + marker)


def adapt_workers(text: str) -> str:
    text = '#include "device_policy.h"\n' + text
    text = once(text, 'uint32_t numCpuCores = dxvk::thread::hardware_concurrency();',
        'uint32_t numCpuCores = skyrim_device_compiler_cpus(dxvk::thread::hardware_concurrency());')
    text = once(text, '    env::setThreadName("dxvk-shader");',
        '    env::setThreadName("dxvk-shader");\n    skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER);')
    text = once(text, '      compilePipelines(item);',
        '      skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER);\n      compilePipelines(item);')
    text = once(text, '    env::setThreadName("dxvk-writer");',
        '    env::setThreadName("dxvk-writer");\n    skyrim_device_platform_work_role(SKYRIM_WORK_CACHE_WRITER);')
    return text


def adapt_pipeline_locking(name: str, text: str) -> str:
    """Keep ready lookups independent of compilation of another state variant.

    Writers take compileMutex before the short instance-list lock. Readers only
    take the latter. Pipeline construction remains serialized per shader set;
    no missing draw is skipped and no extra compiler worker is introduced.
    """
    graphics = "dxvk_graphics." in name
    kind = "Graphics" if graphics else "Compute"
    if name.endswith(".h"):
        anchor = f"    std::vector<Dxvk{kind}PipelineInstance> m_pipelines;"
        return once(text, anchor, anchor + "\n"
            "    // Slow compiler calls never hold the instance-list spinlock.\n"
            "    dxvk::mutex m_compileMutex;")
    cls = f"Dxvk{kind}Pipeline"
    state = f"const Dxvk{kind}PipelineStateInfo& state"
    args = "state"
    if graphics:
        state += ",\n    const DxvkRenderPass*                renderPass"
        args += ", renderPass"
    # The exact source hash is checked before this adapter is applied. Restrict
    # replacement to these three methods; findInstance and GPU creation stay intact.
    begin = text.index(f"  VkPipeline {cls}::getPipelineHandle(")
    end = text.index(f"  {cls}Instance* {cls}::findInstance(", begin)
    wait_kind = "SKYRIM_PERF_" + kind.upper() + "_COMPILE_WAIT"
    compile_lock = f"""    std::unique_lock<dxvk::mutex> compileLock(m_compileMutex, std::defer_lock);
    if (!compileLock.try_lock()) {{
      const uint64_t waitStarted = skyrim_perf_now_ns();
      compileLock.lock();
      skyrim_perf_end({wait_kind}, waitStarted, 0);
    }}
"""
    validate = ""
    if graphics:
        validate = """    if (!this->validatePipelineState(state))
      return nullptr;

"""
    cache_args = "state, renderPass->format()" if graphics else "state"
    methods = f"""  VkPipeline {cls}::getPipelineHandle(
    {state}) {{
    {{ std::lock_guard<sync::Spinlock> lock(m_mutex);
      if (auto* instance = this->findInstance({args}))
        return instance->pipeline();
    }}

    VkPipeline pipeline = VK_NULL_HANDLE;
    {{
{compile_lock}
      {{ std::lock_guard<sync::Spinlock> lock(m_mutex);
        if (auto* instance = this->findInstance({args}))
          return instance->pipeline();
      }}
      auto* instance = this->createInstance({args});
      if (!instance)
        return VK_NULL_HANDLE;
      // compileLock prevents vector growth until this handle has been copied.
      // The state-cache callback below may itself overlap another insertion.
      pipeline = instance->pipeline();
    }}
    this->writePipelineStateToCache({cache_args});
    return pipeline;
  }}


  void {cls}::compilePipeline(
    {state}) {{
    {{ std::lock_guard<sync::Spinlock> lock(m_mutex);
      if (this->findInstance({args}))
        return;
    }}
{compile_lock}
    {{ std::lock_guard<sync::Spinlock> lock(m_mutex);
      if (this->findInstance({args}))
        return;
    }}
    this->createInstance({args});
  }}


  {cls}Instance* {cls}::createInstance(
    {state}) {{
    // Caller owns compileMutex; only publication takes the instance-list lock.
{validate}    VkPipeline newPipelineHandle = this->createPipeline({args});

    std::lock_guard<sync::Spinlock> lock(m_mutex);
    m_pipeMgr->m_num{kind}Pipelines += 1;
    return &m_pipelines.emplace_back({args}, newPipelineHandle);
  }}


"""
    return text[:begin] + methods + text[end:]


def adapt_timing(name: str, text: str) -> str:
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


def adapt_resource_wait(name: str, text: str) -> str:
    if name.endswith('.cpp'):
        return text + '\n' + (Path(__file__).parent / 'resource_wait.cpp.in').read_text(encoding='utf-8')
    text = once(text, '  enum class DxvkAccess {', '''  class DxvkResource;
  extern std::atomic<uint32_t> g_iosResourceWaiters;
  void iosNotifyResourceWaiters(const DxvkResource *resource);

  enum class DxvkAccess {''')
    text = once(text, '          : m_useCountW) -= 1;', '''          : m_useCountW) -= 1;
        if (g_iosResourceWaiters.load() && !isInUse(access)) iosNotifyResourceWaiters(this);''')
    text = once(text, '''      sync::spin(50000, [this, access] {
        return !isInUse(access);
      });''', '''      if (isInUse(access)) iosWaitIdle(access);''')
    return once(text, '    std::atomic<uint32_t> m_useCountR = { 0u };',
                '    void iosWaitIdle(DxvkAccess access) const;\n\n    std::atomic<uint32_t> m_useCountR = { 0u };')


def adapt_shader_inflight(name: str, text: str) -> str:
    if name.endswith('.h'):
        text = once(text, '#include <array>',
            '#include <array>\n#include <memory>\n#include <thread>\n#include <new>')
        return once(text, '      DxvkHash, DxvkEq> m_modules;', '''      DxvkHash, DxvkEq> m_modules;

    struct IosShaderFlight {
      dxvk::condition_variable ready;
      std::thread::id owner = std::this_thread::get_id();
      bool complete = false; // all access under m_mutex
    };
    std::unordered_map<DxvkShaderKey, std::shared_ptr<IosShaderFlight>,
      DxvkHash, DxvkEq> m_iosCompiling;''')
    start = text.index('    // Use the shader\'s unique key for the lookup')
    end = text.index('\n  }\n\n}', start)
    replacement = (Path(__file__).parent / 'shader_inflight.cpp.in').read_text(encoding='utf-8')
    return '#include "performance_metrics.h"\n' + text[:start] + replacement.rstrip('\n') + text[end:]


def adapt(name: str, text: str) -> str:
    if name.endswith(('d3d9_shader.cpp', 'd3d9_shader.h')):
        return adapt_shader_inflight(name, text)
    if name.endswith(('dxvk_resource.h', 'dxvk_resource.cpp')):
        return adapt_resource_wait(name, text)
    if name.endswith(("dxvk_memory.cpp", "dxvk_memory.h")):
        return adapt_memory(name, text)
    if name.endswith("dxvk_state_cache.cpp"):
        return adapt_workers(text)
    if name.endswith(("dxvk_graphics.h", "dxvk_compute.h")):
        return adapt_pipeline_locking(name, text)
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
    if name.endswith(("dxvk_graphics.cpp", "dxvk_compute.cpp")):
        text = adapt_pipeline_locking(name, text)
    return adapt_timing(name, text)


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
