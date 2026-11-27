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
    'src/d3d9/d3d9_multithread.h': '78c954cad28730b7be132a6b4034ad1b4aecf38d8c26f507630527b7bbeffc04',
    'src/d3d9/d3d9_device.h': '56b14e8a8303c13ea85a55f5cd3ddd0e35e310cb96d2d4c4e4ea5078e9fd1728',
    'src/d3d9/d3d9_swapchain.cpp': '04caf3a5097461d5b2fd6e82bb12749f74ccca8f617ca2ac1b5466a365c61f29',
    'src/d3d9/d3d9_device.cpp': 'ae0e2e6c24cd0fc0ffef265ddeefaec8355d8950deb2d7f64c16175f7c03bca2',
    'src/vulkan/vulkan_presenter.cpp': 'fc91ecc01c03ce46620843acc18450d2a99e152467c778a1b3ab5e444ca51663',
    'src/dxvk/dxvk_cmdlist.cpp': 'a4150f38f9c6139d4a4066c4a335f87880ba62bda6aaa2a2bd38d7b16c6f8e19',
    'src/dxvk/dxvk_queue.cpp': '5750907e13a8294f3f27199e329a03220b39fbb122e64b832c8a2236221ae126',

    "src/d3d9/d3d9_config.h": "e75608dc8754e9b81b88b9fd88880bdf6bf0067b2659a649c7ff621330c4e2e7",
    "src/dxvk/dxvk_descriptor.h": "071b047b53e76d19b07bd0c16b5203c1a41f313d789bd8a724d09700d8f2db55",
    "src/dxvk/dxvk_descriptor.cpp": "aa5de1ddb614506d880619b2ec9906453f6dd359616880d0d8ce98878b22033c",
    "src/dxvk/dxvk_pipelayout.h": "a666ca205342a74c861c1db8f352e9cf697019c4aad664dcdcfbec613f013796",
    "src/dxvk/dxvk_pipelayout.cpp": "0711068ff173c082df22ef0ba12fe385af25be0ef1159a71f015cbea34a49733",
    "src/dxvk/dxvk_context.h": "6a40624cafcc09f654015273feddef05c824dde42361e1f9914683a134a9fc36",
    "src/dxvk/dxvk_context.cpp": "c4614edda7f4cc4e6e221e9a9ce2f9ef4900ffb412a8333342ff627344a56213",

    "src/d3d9/d3d9_shader.cpp": "40e72c614732f25b99ba298fa3a3fd3dfec2cbbb771e05f4b6821b5478d166b4",
    "src/d3d9/d3d9_shader.h": "0817969cc9b551287a87a8cbe66e209535c15e39551528388a6a9f442fd9ad9d",
    "src/dxvk/dxvk_resource.h": "1dc5c873dc7207534ccd0201b2ab3509fcf0c3d71a86bbbfeb46cf700760b835",
    "src/dxvk/dxvk_resource.cpp": "bcb6bc7f6a37a32ae257e1d18cd1b33f8da00b02fe1c3e12b30a5386823a6bf4",
    "src/dxvk/dxvk_memory.cpp": "1432d9264ecd19159ef02af2de8767b31e19fa65d7e0653d16957ef2fd241631",
    "src/dxvk/dxvk_memory.h": "4aeee77488305734c8b559ec6c53309b048f47b197ad4532b36b5a7e3ccb57e4",
    "src/dxvk/dxvk_state_cache.cpp": "5b0942b71353c1731710f40a6f9573c8371c4cad65c661598aef06771b87026d",
    "src/dxvk/dxvk_state_cache.h": "2b9ba65a9e5d3e349085edea4bed794f0fcc3092e08ea75adb74a21148402a18",
    "src/dxvk/dxvk_graphics.cpp": "dc35eb99f52d3dc4ea3e033fbe55c4a06faae8201c3424bf552aefcbc4e509ca",
    "src/dxvk/dxvk_compute.cpp": "2515fefb8ae4acee0fe9ba6fd3a36eda8664fa9bcbb15784a83b39763c26ee0e",
    "src/dxvk/dxvk_graphics.h": "f2fb63084bd9757262c641c6e4e20a4a5ff223edacc4ef23eae2d486725ddf88",
    "src/dxvk/dxvk_compute.h": "41ae5eabc882bbd58918905838bfe7b5e0b9f7551fe482ddfa740e68529b4148",
    "src/dxvk/dxvk_shader.cpp": "8d6c02908f7b217039bf7d80e8ac5317bf8a827dfc54d29a88a5414103d05063",
    "src/dxvk/dxvk_pipecache.cpp": "a7d0a391aa19fa8f560487843b9a2d83e492c87a710914f76c4807640829e070",
    "src/dxvk/dxvk_pipecache.h": "b0dd5f8120da50e0426e1c84b13e9e169fc9088b50e1fc906f12c2ca0f72caeb",
    "src/dxvk/dxvk_pipemanager.cpp": "7f6a912d006624cf5236eebeea3ef211664e380c4595f69e6e6efeac49cd0c8a",
    "src/dxvk/dxvk_pipemanager.h": "b4d6d7148be411ad6b4a321652da25e20a646a52eba907f86e567a43ee7befd5",
}


# These fixed internal layouts use no dynamic UBOs and at most two combined
# image samplers. A changed helper must be reviewed before this bound is used.
META_DESCRIPTOR_BOUNDS = {
    "src/dxvk/dxvk_meta_blit.cpp": "10074c278f5971bd4b2bc98da12df53736bfaf84eb910cd13ed24ef036ef95c1",
    "src/dxvk/dxvk_meta_clear.cpp": "07b8107a2cfc1734d26632bc0814f45f97b847319261efa1fd55d2910ac6e077",
    "src/dxvk/dxvk_meta_copy.cpp": "ab7cea34783677b7726590fa2974b4a62040a2f3eaa61961527bebceadd6c2fc",
    "src/dxvk/dxvk_meta_mipgen.cpp": "98fa69e81a5673d8577b631063bb035f64f3e9acc532fb4dbb9e4d758f609324",
    "src/dxvk/dxvk_meta_pack.cpp": "e2e8e47e809a58488113eb092b608ad3cc3c8d572b769a82156d4b5090ea4c8f",
    "src/dxvk/dxvk_meta_resolve.cpp": "62274f749116a92d251dfb9a29173fbbf18cfabbe802a1ee6ace4a21fec6a806",
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


def adapt_worker_header(text: str) -> str:
    text = '#include "performance_metrics.h"\n' + text
    return once(text, '''    struct WorkerItem {
      DxvkGraphicsPipelineShaders gp;
      DxvkComputePipelineShaders  cp;
    };''', '''    struct WorkerItem {
      DxvkGraphicsPipelineShaders gp;
      DxvkComputePipelineShaders  cp;
      SkyrimPerfPrefetch iosPrefetch = {};
    };''')


def adapt_workers(text: str) -> str:
    text = '#include "device_policy.h"\n' + text
    text = once(text, '    m_shaderMap.insert({ key, shader });', '''    const bool iosInserted = m_shaderMap.insert({ key, shader }).second;
    // While entryLock is held, a newly inserted final prerequisite makes all
    // its now-complete tuples eligible. Capture once before any worker-lock
    // wait; duplicate registrations cannot reconstruct the original time.
    const uint64_t iosReadyNs = iosInserted ? skyrim_perf_now_ns() : 0;''')
    text = once(text, '      m_workerQueue.push(item);', '''      item.iosPrefetch.ready_ns = iosReadyNs;
      item.iosPrefetch.queued_ns = skyrim_perf_now_ns();
      m_workerQueue.push(item);''')
    text = once(text, '''        item = m_workerQueue.front();
        m_workerQueue.pop();''', '''        item = m_workerQueue.front();
        item.iosPrefetch.worker_ns = skyrim_perf_now_ns();
        m_workerQueue.pop();''')
    text = once(text, '  void DxvkStateCache::stopWorkerThreads() {',
        '  void DxvkStateCache::stopWorkerThreads() {\n    m_pipeManager->iosPrefetchGate().stop();')
    text = once(text, 'uint32_t numCpuCores = dxvk::thread::hardware_concurrency();',
        'uint32_t numCpuCores = skyrim_device_compiler_cpus(dxvk::thread::hardware_concurrency());')
    text = once(text, '    env::setThreadName("dxvk-shader");',
        '    env::setThreadName("dxvk-shader");\n    skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER);')
    text = once(text, '      compilePipelines(item);',
        '      skyrim_device_platform_work_role(SKYRIM_WORK_COMPILER);\n'
        '      SkyrimPerfPrefetchScope iosPrefetchScope(item.iosPrefetch);\n'
        '      compilePipelines(item);')
    text = once(text, '    env::setThreadName("dxvk-writer");',
        '    env::setThreadName("dxvk-writer");\n    skyrim_device_platform_work_role(SKYRIM_WORK_CACHE_WRITER);')
    # A worker item already visits every state entry for its shader tuple.
    # Registering the tuple once per entry makes N variants queue N jobs,
    # each revisiting all N variants. Preserve every entry and its format;
    # only the shader-to-tuple wakeup index needs one record per tuple.
    text = once(text, '''        mapPipelineToEntry(entry.shaders, entryId);

        mapShaderToPipeline(entry.shaders.vs,  entry.shaders);
        mapShaderToPipeline(entry.shaders.tcs, entry.shaders);
        mapShaderToPipeline(entry.shaders.tes, entry.shaders);
        mapShaderToPipeline(entry.shaders.gs,  entry.shaders);
        mapShaderToPipeline(entry.shaders.fs,  entry.shaders);
        mapShaderToPipeline(entry.shaders.cs,  entry.shaders);''', '''        const bool firstShaderTuple = m_entryMap.find(entry.shaders) == m_entryMap.end();
        mapPipelineToEntry(entry.shaders, entryId);

        if (firstShaderTuple) {
          ++numShaderTuples;
          mapShaderToPipeline(entry.shaders.vs,  entry.shaders);
          mapShaderToPipeline(entry.shaders.tcs, entry.shaders);
          mapShaderToPipeline(entry.shaders.tes, entry.shaders);
          mapShaderToPipeline(entry.shaders.gs,  entry.shaders);
          mapShaderToPipeline(entry.shaders.fs,  entry.shaders);
          mapShaderToPipeline(entry.shaders.cs,  entry.shaders);
        }''')
    text = once(text, '    uint32_t numInvalidEntries = 0;',
        '    uint32_t numInvalidEntries = 0;\n    size_t numShaderTuples = 0;')
    text = once(text, '      " valid state cache entries"));',
        '      " valid state cache entries"));\n'
        '    Logger::info(str::format("IOS_STATE_CACHE entries=", m_entries.size(),\n'
        '      " shader_tuples=", numShaderTuples, " wakeup_links=", m_pipelineMap.size()));')
    return text


def adapt_pipeline_locking_baseline(name: str, text: str) -> str:
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
    compile_lock = f"""    uint64_t waitStartedNs = 0, waitEndedNs = 0;
    std::unique_lock<dxvk::mutex> compileLock(m_compileMutex, std::defer_lock);
    auto acquireCompileLock = [&] {{
      waitStartedNs = waitEndedNs = 0;
      if (!compileLock.try_lock()) {{
        const uint64_t waitStarted = skyrim_perf_now_ns();
        compileLock.lock();
        waitStartedNs = waitStarted;
        waitEndedNs = skyrim_perf_now_ns();
      }}
    }};
    auto recordWait = [&](unsigned result) {{
      if (!waitStartedNs) return;
      SkyrimPerfContext context = skyrim_perf_context_get();
      context.result = result;
      skyrim_perf_interval_context({wait_kind}, waitStartedNs, waitEndedNs, 0, 0, context);
      waitStartedNs = 0;
    }};
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

    SkyrimPerfContextScope iosRequest(SKYRIM_PERF_ROLE_DRAW, SKYRIM_PERF_CONTEXT_KEEP);
    VkPipeline pipeline = VK_NULL_HANDLE;
    {{
      skyrim::PrefetchGate::RequiredScope iosRequired(m_pipeMgr->iosPrefetchGate());
{compile_lock}
      acquireCompileLock();
      bool ready = false;
      {{ std::lock_guard<sync::Spinlock> lock(m_mutex);
        if (auto* instance = this->findInstance({args})) {{
          ready = true;
          pipeline = instance->pipeline();
        }}
      }}
      if (ready) {{ recordWait(SKYRIM_PERF_WAIT_HIT); return pipeline; }}
      recordWait(SKYRIM_PERF_WAIT_CREATE);
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
    SkyrimPerfContextScope iosRequest(SKYRIM_PERF_ROLE_PREFETCH, SKYRIM_PERF_CONTEXT_KEEP);
    skyrim::PrefetchGate::Attempt iosAdmission(m_pipeMgr->iosPrefetchGate());
{compile_lock}
    for (;;) {{
      iosAdmission.wait(); // No DXVK/MoltenVK lock is held here.
      acquireCompileLock();
      if (iosAdmission.mayStart()) break;
      recordWait(SKYRIM_PERF_WAIT_UNKNOWN);
      compileLock.unlock();
    }}
    bool ready;
    {{ std::lock_guard<sync::Spinlock> lock(m_mutex);
      ready = this->findInstance({args}) != nullptr;
    }}
    if (ready) {{ recordWait(SKYRIM_PERF_WAIT_HIT); return; }}
    recordWait(SKYRIM_PERF_WAIT_CREATE);
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


def adapt_pipeline_locking(name: str, text: str) -> str:
    """Keep the serial route and add a startup-fixed exact-state route."""
    text = adapt_pipeline_locking_baseline(name, text)
    if "dxvk_graphics." not in name:
        return text
    if name.endswith(".h"):
        text = once(text, "#include <mutex>",
            '#include <mutex>\n#include <optional>\n#include "graphics_flights.h"')
        text = once(text, "    dxvk::mutex m_compileMutex;",
            "    dxvk::mutex m_compileMutex;\n"
            "    // OFF constructs no table mutex/condition variable. Fixed at construction.\n"
            "    std::optional<skyrim::GraphicsFlights<DxvkGraphicsPipelineStateInfo, DxvkRenderPass>> m_compileFlights;")
        return once(text, "    VkPipeline createPipeline(", """    VkPipeline getPipelineHandleExact(
      const DxvkGraphicsPipelineStateInfo& state,
      const DxvkRenderPass*                renderPass);

    void compilePipelineExact(
      const DxvkGraphicsPipelineStateInfo& state,
      const DxvkRenderPass*                renderPass);

    std::optional<VkPipeline> createInstanceExact(
      const DxvkGraphicsPipelineStateInfo& state,
      const DxvkRenderPass*                renderPass);

    VkPipeline createPipeline(""")
    text = once(text, "    m_shaders(std::move(shaders)) {",
        "    m_shaders(std::move(shaders)) {\n"
        "    if (m_pipeMgr->iosExactGraphicsState()) m_compileFlights.emplace();")
    text = once(text, "    SkyrimPerfContextScope iosRequest(SKYRIM_PERF_ROLE_DRAW, SKYRIM_PERF_CONTEXT_KEEP);",
        "    if (m_compileFlights) return this->getPipelineHandleExact(state, renderPass);\n\n"
        "    SkyrimPerfContextScope iosRequest(SKYRIM_PERF_ROLE_DRAW, SKYRIM_PERF_CONTEXT_KEEP);")
    text = once(text, "    SkyrimPerfContextScope iosRequest(SKYRIM_PERF_ROLE_PREFETCH, SKYRIM_PERF_CONTEXT_KEEP);",
        "    if (m_compileFlights) { this->compilePipelineExact(state, renderPass); return; }\n\n"
        "    SkyrimPerfContextScope iosRequest(SKYRIM_PERF_ROLE_PREFETCH, SKYRIM_PERF_CONTEXT_KEEP);")
    marker = "  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::findInstance("
    methods = (Path(__file__).parent / "graphics_state_methods.cpp.in").read_text(encoding="utf-8").rstrip() + "\n\n\n"
    return once(text, marker, methods + marker)


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
    admission = "" if name.endswith("dxvk_shader.cpp") else "    auto iosCacheCompile = m_pipeMgr->m_cache->compileScope();\n"
    return once(text, "    if (" + call + " != VK_SUCCESS)",
        admission + "    const uint64_t iosStarted = skyrim_perf_now_ns();\n"
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


def adapt_descriptor_capacity(name: str, text: str) -> str:
    if name.endswith('dxvk_descriptor.h'):
        text = once(text, '      VkDescriptorSetLayout layout);', '''      VkDescriptorSetLayout layout,
      uint32_t uniformDynamic, uint32_t combinedImages);''')
        return once(text, '    VkDescriptorPool m_pool;', '''    VkDescriptorPool m_pool;
    uint32_t m_iosDynamicCapacity = 0, m_iosImageCapacity = 0;
    uint32_t m_iosDynamicUsed = 0, m_iosImageUsed = 0;''')
    if name.endswith('dxvk_descriptor.cpp'):
        text = once(text, '    VkDescriptorPoolCreateInfo info;', '''    // MoltenVK may individually allocate descriptors after a type pool is
    // exhausted, while returning VK_SUCCESS. Roll over before those hot-path
    // allocations. Keep the original capacities and GPU retirement ownership.
    for (const auto& size : pools) {
      if (size.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        m_iosDynamicCapacity = size.descriptorCount;
      if (size.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
        m_iosImageCapacity = size.descriptorCount;
    }

    VkDescriptorPoolCreateInfo info;''')
        text = once(text, '  VkDescriptorSet DxvkDescriptorPool::alloc(VkDescriptorSetLayout layout) {', '''  VkDescriptorSet DxvkDescriptorPool::alloc(VkDescriptorSetLayout layout,
      uint32_t uniformDynamic, uint32_t combinedImages) {
    if (uniformDynamic > m_iosDynamicCapacity - m_iosDynamicUsed
     || combinedImages > m_iosImageCapacity - m_iosImageUsed)
      return VK_NULL_HANDLE;''')
        text = once(text, '    return set;', '''    m_iosDynamicUsed += uniformDynamic;
    m_iosImageUsed += combinedImages;
    return set;''')
        return once(text, '''    m_vkd->vkResetDescriptorPool(
      m_vkd->device(), m_pool, 0);''', '''    if (m_vkd->vkResetDescriptorPool(
          m_vkd->device(), m_pool, 0) != VK_SUCCESS)
      throw DxvkError("DxvkDescriptorPool: Failed to reset descriptor pool");
    m_iosDynamicUsed = m_iosImageUsed = 0;''')
    if name.endswith('dxvk_pipelayout.h'):
        text = once(text, '    VkDescriptorSetLayout descriptorSetLayout() const {', '''    uint32_t iosDynamicDescriptors() const { return m_iosDynamicDescriptors; }
    uint32_t iosCombinedImages() const { return m_iosCombinedImages; }

    VkDescriptorSetLayout descriptorSetLayout() const {''')
        return once(text, '    Flags<VkDescriptorType>         m_descriptorTypes;', '''    Flags<VkDescriptorType>         m_descriptorTypes;
    uint32_t m_iosDynamicDescriptors = 0, m_iosCombinedImages = 0;''')
    if name.endswith('dxvk_pipelayout.cpp'):
        return once(text, '      bindings[i].pImmutableSamplers = nullptr;', '''      bindings[i].pImmutableSamplers = nullptr;
      // Count the actual layout after static/dynamic conversion, once per
      // layout. No per-draw map lookup or descriptor scan.
      if (bindings[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        m_iosDynamicDescriptors += bindings[i].descriptorCount;
      if (bindings[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
        m_iosCombinedImages += bindings[i].descriptorCount;''')
    if name.endswith('dxvk_context.h'):
        return once(text, '''    VkDescriptorSet allocateDescriptorSet(
            VkDescriptorSetLayout     layout);''', '''    // Fixed meta layouts use 0 dynamic UBOs and <=2 combined samplers.
    // Graphics/compute layouts pass their exact immutable counts explicitly.
    VkDescriptorSet allocateDescriptorSet(
            VkDescriptorSetLayout     layout,
            uint32_t uniformDynamic = 0, uint32_t combinedImages = 2);''')
    text = once(text, '      set = allocateDescriptorSet(layout->descriptorSetLayout());', '''      set = allocateDescriptorSet(layout->descriptorSetLayout(),
        layout->iosDynamicDescriptors(), layout->iosCombinedImages());''')
    text = once(text, '''  VkDescriptorSet DxvkContext::allocateDescriptorSet(
          VkDescriptorSetLayout     layout) {''', '''  VkDescriptorSet DxvkContext::allocateDescriptorSet(
          VkDescriptorSetLayout     layout,
          uint32_t uniformDynamic, uint32_t combinedImages) {''')
    if text.count('m_descPool->alloc(layout)') != 2:
        raise ValueError('Descriptor pool retry calls changed')
    return text.replace('m_descPool->alloc(layout)', 'm_descPool->alloc(layout, uniformDynamic, combinedImages)')


def observe_water_draw(name: str, text: str) -> str:
    """Sample D3D9 bindings and their texels only during a manually armed probe."""
    if name.endswith('d3d9_device.h'):
        field = '    Rc<DxvkDevice>                  m_dxvkDevice;'
        return '#include "water_probe.h"\n' + once(text, field, field + '''
    struct IosWaterReadback {
      skyrim_water::CaptureTag tag;
      Rc<DxvkBuffer> buffer;
      Rc<sync::Fence> ready;
      skyrim_water::PixelEncoding encoding = skyrim_water::PixelEncoding::R32Float;
      skyrim_water::CompositeTint tint;
      unsigned format = VK_FORMAT_UNDEFINED;
    };
    std::array<IosWaterReadback, skyrim_water::PixelRoles> m_iosWaterReadbacks{};
    void PollWaterReadbacks();
    void QueueWaterReadback(const Rc<DxvkImageView>& view, const skyrim_water::SampleView& sample,
        skyrim_water::CaptureTag tag, skyrim_water::PixelRole role, const skyrim_water::CompositeTint& tint);''')
    if name.endswith('d3d9_shader.h'):
        return '#include "water_probe.h"\n' + once(text, '    D3D9PixelShader(\n',
            '    skyrim_water::Layout iosWaterLayout{};\n\n    D3D9PixelShader(\n')
    if name.endswith('d3d9_swapchain.cpp'):
        return '#include "water_probe.h"\n' + once(text, '      PresentImage(presentInterval);',
            '''      PresentImage(presentInterval);
      m_parent->PollWaterReadbacks();
      if (unlikely(skyrim_water::phase.load(std::memory_order_relaxed) != skyrim_water::Phase::Idle))
        skyrim_water::present(m_parent, this);''')
    if name.endswith('d3d9_device.cpp'):
        reset = '  HRESULT D3D9DeviceEx::ResetSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) {'
        text = once(text, reset, reset + '''
    for (auto& pending : m_iosWaterReadbacks) {
      pending.buffer = nullptr;
      pending.ready = nullptr;
    }
''')
        prepare = '  void D3D9DeviceEx::PrepareDraw(D3DPRIMITIVETYPE PrimitiveType) {'
        text = once(text, prepare, '''
  // Use padded offsets and rows for the Vulkan-to-Metal image copy. Only the
  // first texel in each row is written. CS owns its references after emission.
  static constexpr VkDeviceSize WaterReadbackStride = 256;

  void D3D9DeviceEx::QueueWaterReadback(const Rc<DxvkImageView>& view,
      const skyrim_water::SampleView& sample, skyrim_water::CaptureTag tag,
      skyrim_water::PixelRole role, const skyrim_water::CompositeTint& tint) {
    using namespace skyrim_water;
    auto& pending = m_iosWaterReadbacks[unsigned(role) - 1];
    if (pending.buffer != nullptr || pending.tag.sequence == tag.sequence ||
        !(sample.flags & ViewBound) ||
        (sample.flags & (ViewShadow | ViewAliasesColor | ViewAliasesDepth)))
      return;

    const auto& v = view->info();
    const auto& image = view->imageInfo();
    if (v.type != VK_IMAGE_VIEW_TYPE_2D || v.aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
        v.numLayers != 1 || v.numLevels != 1 || image.sampleCount != VK_SAMPLE_COUNT_1_BIT ||
        !(image.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
      return;

    const auto maps = [](VkComponentSwizzle actual, VkComponentSwizzle channel) {
      return actual == VK_COMPONENT_SWIZZLE_IDENTITY || actual == channel;
    };
    if (!maps(v.swizzle.r, VK_COMPONENT_SWIZZLE_R)) return;
    PixelEncoding encoding;
    if (depthRole(role)) {
      if (v.format != image.format) return;
      switch (v.format) {
        case VK_FORMAT_R32_SFLOAT:    encoding = PixelEncoding::R32Float; break;
        case VK_FORMAT_R16_SFLOAT:    encoding = PixelEncoding::R16Float; break;
        case VK_FORMAT_R8_UNORM:      encoding = PixelEncoding::R8Unorm; break;
        case VK_FORMAT_R8G8B8A8_UNORM: encoding = PixelEncoding::Rgba8Unorm; break;
        case VK_FORMAT_B8G8R8A8_UNORM: encoding = PixelEncoding::Bgra8Unorm; break;
        default: return;
      }
    } else {
      if (!maps(v.swizzle.g, VK_COMPONENT_SWIZZLE_G) ||
          !maps(v.swizzle.b, VK_COMPONENT_SWIZZLE_B)) return;
      // sRGB decoding preserves black/white and equality per channel.
      if (image.format == VK_FORMAT_R8G8B8A8_UNORM &&
          (v.format == image.format || v.format == VK_FORMAT_R8G8B8A8_SRGB))
        encoding = PixelEncoding::Rgba8Unorm;
      else if (image.format == VK_FORMAT_B8G8R8A8_UNORM &&
               (v.format == image.format || v.format == VK_FORMAT_B8G8R8A8_SRGB))
        encoding = PixelEncoding::Bgra8Unorm;
      else return;
    }
    PixelPoints points;
    if (!pixelPoints(sample.width, sample.height, points)) return;

    // Mark this request once even if the optional allocation fails.
    pending.tag = tag;
    DxvkBufferCreateInfo info{};
    info.size = WaterReadbackStride * points.size();
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_HOST_BIT;
    info.access = VK_ACCESS_HOST_READ_BIT;
    try {
      pending.ready = new sync::Fence();
      pending.buffer = m_dxvkDevice->createBuffer(info,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } catch (const DxvkError&) {
      pending.ready = nullptr;
      return;
    } catch (const std::bad_alloc&) {
      pending.ready = nullptr;
      return;
    }
    pending.encoding = encoding;
    pending.format = sample.format;
    pending.tint = tint;
    const VkImageSubresourceLayers layers{v.aspect, v.minLevel, v.minLayer, 1};
    EmitCs([buffer = pending.buffer, ready = pending.ready, source = view->image(),
            layers, points](DxvkContext* ctx) {
      for (size_t i = 0; i < points.size(); ++i) {
        const VkOffset3D point{int32_t(points[i].x), int32_t(points[i].y), 0};
        ctx->copyImageToBuffer(buffer, i * WaterReadbackStride, WaterReadbackStride, 0,
            source, layers, point, VkExtent3D{1, 1, 1});
      }
      ctx->signal(ready, 1);
    });
  }

  void D3D9DeviceEx::PollWaterReadbacks() {
    using namespace skyrim_water;
    for (size_t i = 0; i < m_iosWaterReadbacks.size(); ++i) {
      auto& pending = m_iosWaterReadbacks[i];
      if (pending.buffer == nullptr) continue;
      if (m_dxvkDevice->getDeviceStatus() == VK_SUCCESS && pixelsCurrent(pending.tag)) {
        if (!pending.ready->value()) continue;
        // A queue failure can signal the fence as well. Check again after it.
        if (m_dxvkDevice->getDeviceStatus() == VK_SUCCESS) {
          const auto role = static_cast<PixelRole>(i + 1);
          const auto* mapped = static_cast<const uint8_t*>(pending.buffer->mapPtr(0));
          if (mapped) {
            std::array<uint8_t, PixelPoints{}.size() * 4> bytes{};
            const size_t texelBytes = pending.encoding == PixelEncoding::R8Unorm ? 1
                : pending.encoding == PixelEncoding::R16Float ? 2 : 4;
            for (size_t point = 0; point < PixelPoints{}.size(); ++point)
              std::memcpy(bytes.data() + point * 4, mapped + point * WaterReadbackStride, texelBytes);
            PixelSummary summary;
            if (summarizePixels(bytes.data(), bytes.size(), pending.encoding, role, summary))
              reportPixels(pending.tag, role, pending.format, summary);
            TintSummary tintSummary;
            if (role == PixelRole::CompositeDepth &&
                summarizeTint(bytes.data(), bytes.size(), pending.encoding, pending.tint, tintSummary))
              reportTint(pending.tag, tintSummary);
          }
        }
      }
      pending.buffer = nullptr;
      pending.ready = nullptr;
    }
  }

''' + prepare)
        entry = '      UndirtyTextures(usedDirtyTextures);'
        return '#include "water_probe.h"\n' + once(text, entry, entry + '''
    if (unlikely(skyrim_water::phase.load(std::memory_order_relaxed) == skyrim_water::Phase::Capturing)
        && PrimitiveType != D3DPT_FORCE_DWORD) {
      auto* shader = m_state.pixelShader.ptr();
      const auto* bytes = shader ? &shader->GetCommonShader()->GetBytecode() : nullptr;
      skyrim_water::draw(this, shader ? &shader->iosWaterLayout : nullptr,
        bytes ? bytes->data() : nullptr, bytes ? bytes->size() : 0,
        [](void* device, const skyrim_water::Layout& layout, skyrim_water::CaptureTag tag) {
          auto* self = static_cast<D3D9DeviceEx*>(device);
          bool depthMs = false;
          const auto binding = [self](int slot, bool* multisampled) {
            using Binding = skyrim_water::Binding;
            if (slot < 0) return Binding::Undeclared;
            const auto* texture = GetCommonTexture(self->m_state.textures[slot]);
            if (!texture) return Binding::Unbound;
            const auto* desc = texture->Desc();
            if (multisampled) *multisampled = desc->MultiSample != D3DMULTISAMPLE_NONE;
            return (desc->Usage & D3DUSAGE_DEPTHSTENCIL) ? Binding::DepthStencilUsage : Binding::ColorUsage;
          };
          skyrim_water::Inputs inputs;
          inputs.depth = binding(layout.depth, &depthMs);
          inputs.color = binding(layout.color, nullptr);
          inputs.depthMultisampled = depthMs;
          const auto constants = [self](int slot) {
            return slot < 0 ? nullptr : self->m_state.psConsts.fConsts[slot].data;
          };
          inputs.params = skyrim_water::classifyParams(constants(layout.params));
          inputs.camera = skyrim_water::classifyCamera(constants(layout.camera));
          if (layout.family == skyrim_water::Family::WaterFog || layout.family == skyrim_water::Family::ImageBlur) {
            const auto describe = [self, tag](int slot, skyrim_water::PixelRole role,
                const skyrim_water::CompositeTint& tint) {
              using namespace skyrim_water;
              SampleView result;
              if (slot < 0) return result;
              const auto* texture = GetCommonTexture(self->m_state.textures[slot]);
              if (!texture) return result;
              const DWORD usage = texture->Desc()->Usage;
              const bool srgb = self->m_state.samplerStates[slot][D3DSAMP_SRGBTEXTURE] & 1;
              if (usage & D3DUSAGE_RENDERTARGET) result.flags |= ViewRenderTarget;
              if (usage & D3DUSAGE_DEPTHSTENCIL) result.flags |= ViewDepthStencil;
              if (srgb) result.flags |= ViewSrgbRequested;
              if (texture->IsShadow()) result.flags |= ViewShadow;
              // Match the view selected by D3D9's BindTexture, including its sRGB choice.
              const auto& view = texture->GetSampleView(srgb);
              if (view == nullptr || !(self->m_activeTextures & (1u << slot))) return result;
              result.flags |= ViewBound;
              const auto size = view->mipLevelExtent(0);
              result.format = unsigned(view->info().format);
              result.aspect = view->info().aspect;
              result.width = size.width;
              result.height = size.height;
              const auto* image = view->image().ptr();
              for (const auto& target : self->m_state.renderTargets)
                if (target != nullptr && target->GetCommonTexture()->GetImage().ptr() == image)
                  result.flags |= ViewAliasesColor;
              const auto& depth = self->m_state.depthStencil;
              if (depth != nullptr && depth->GetCommonTexture()->GetImage().ptr() == image)
                result.flags |= ViewAliasesDepth;
              self->QueueWaterReadback(view, result, tag, role, tint);
              return result;
            };
            const auto sample = [&inputs, &describe](int slot, skyrim_water::PixelRole role,
                const skyrim_water::CompositeTint& tint = {}) {
              inputs.views[unsigned(role) - 1] = describe(slot, role, tint);
            };
            using skyrim_water::PixelRole;
            if (layout.family == skyrim_water::Family::WaterFog) {
              sample(layout.depth, PixelRole::Depth);
              sample(layout.color, PixelRole::Refraction);
            } else {
              const auto& c = layout.composite;
              const auto* controls = constants(c.controls);
              const auto* projection = constants(c.projection);
              const auto* colorTint = constants(c.tint);
              const auto* tintRange = constants(c.tintRange);
              inputs.composite = skyrim_water::classifyComposite(controls, projection, colorTint, tintRange);
              sample(layout.depth, PixelRole::CompositeDepth,
                  skyrim_water::compositeTint(controls, projection, colorTint, tintRange));
              sample(c.sharp, PixelRole::CompositeSharp);
              sample(layout.color, PixelRole::CompositeBlurred);
            }
          }
          return inputs;
        });
    }
''')
    return text


def adapt(name: str, text: str) -> str:
    if name.endswith(('d3d9_multithread.h', 'd3d9_device.h', 'd3d9_swapchain.cpp')):
        import sys
        sys.path.insert(0, str(Path(__file__).parent))
        from d3d9_lock_timing import adapt as observe_device_lock
        text = observe_water_draw(name, observe_device_lock(name, text))
        if name.endswith('d3d9_swapchain.cpp'):
            # A minimized or unavailable swap chain can return D3D_OK without
            # queuing a frame. Confirm the source dimensions only after a real
            # swapchain image was acquired and a present request was queued.
            text = '#include "ios_runtime.h"\n' + once(text, '      SubmitPresent(sync, i);', '''      SubmitPresent(sync, i);
#if defined(SKYRIM_IOS_RENDER_SUBMISSION)
      if (!i) {
        const VkExtent3D size = swapImage->info().extent;
        skyrim_ios_render_submitted(size.width, size.height);
      }
#endif''')
        return text
    if name.endswith('d3d9_device.cpp'):
        import sys
        sys.path.insert(0, str(Path(__file__).parent))
        from lifecycle_device import adapt as adapt_lifecycle
        return observe_water_draw(name, adapt_lifecycle(text))
    if name.endswith(("vulkan_presenter.cpp", "dxvk_cmdlist.cpp", "dxvk_queue.cpp")):
        import sys
        sys.path.insert(0, str(Path(__file__).parent))
        from present_timing import adapt as observe_present
        return observe_present(name, text)
    if name.endswith('d3d9_config.h'):
        # Existing DXVK SM3 compatibility path: keep finite NRM/LOG boundary
        # behavior instead of disabling water/fog/postprocessing. Explicit
        # d3d9.floatEmulation overrides still work in the original options code.
        return once(text, 'constexpr bool FloatEmulationByDefault = false;',
            'constexpr bool FloatEmulationByDefault = true;')
    if name.endswith(("dxvk_descriptor.h", "dxvk_descriptor.cpp", "dxvk_pipelayout.h", "dxvk_pipelayout.cpp", "dxvk_context.h", "dxvk_context.cpp")):
        return adapt_descriptor_capacity(name, text)
    if name.endswith(('d3d9_shader.cpp', 'd3d9_shader.h')):
        return observe_water_draw(name, adapt_shader_inflight(name, text))
    if name.endswith(('dxvk_resource.h', 'dxvk_resource.cpp')):
        return adapt_resource_wait(name, text)
    if name.endswith(("dxvk_memory.cpp", "dxvk_memory.h")):
        return adapt_memory(name, text)
    if name.endswith("dxvk_state_cache.cpp"):
        return adapt_workers(text)
    if name.endswith("dxvk_state_cache.h"):
        return adapt_worker_header(text)
    if name.endswith(("dxvk_graphics.h", "dxvk_compute.h")):
        return adapt_pipeline_locking(name, text)
    if name.endswith("dxvk_pipemanager.h"):
        text = '#include "prefetch_gate.h"\n#include "graphics_policy.h"\n' + text
        text = once(text, '    void stopWorkerThreads() const;',
            '    void stopWorkerThreads() const;\n    skyrim::PrefetchGate& iosPrefetchGate() const { return m_iosPrefetchGate; }\n'
            '    bool iosExactGraphicsState() const { return m_iosExactGraphicsState; }')
        return once(text, '    const DxvkDevice*         m_device;',
            '    // Construct before and destroy after the state cache/workers.\n'
            '    const bool m_iosExactGraphicsState = skyrim::exactGraphicsStateEnabled();\n'
            '    mutable skyrim::PrefetchGate m_iosPrefetchGate;\n    const DxvkDevice*         m_device;')
    if name.endswith("dxvk_pipemanager.cpp"):
        return once(text, "new DxvkPipelineCache(device->vkd())",
            "new DxvkPipelineCache(device->vkd(), device->adapter()->deviceProperties())")
    if name.endswith("dxvk_pipecache.h"):
        text = '#include "pipeline_cache.h"\n#include <memory>\n' + text
        text = once(text, "DxvkPipelineCache(const Rc<vk::DeviceFn>& vkd);",
            "DxvkPipelineCache(const Rc<vk::DeviceFn>& vkd, const VkPhysicalDeviceProperties& properties);\n"
            "    void compiled() { m_disk->compiled(); }\n"
            "    skyrim::PipelineCache::CompileScope compileScope() { return skyrim::PipelineCache::CompileScope(*m_disk); }")
        return once(text, "VkPipelineCache         m_handle;",
            "VkPipelineCache         m_handle;\n    std::unique_ptr<skyrim::PipelineCache> m_disk;")
    if name.endswith("dxvk_pipecache.cpp"):
        return '''#include "dxvk_pipecache.h"
#include <cstdlib>
#include <cstdio>
#ifndef SKYRIM_PIPELINE_CACHE_REUSE_NAMESPACE
#define SKYRIM_PIPELINE_CACHE_REUSE_NAMESPACE SKYRIM_PIPELINE_CACHE_NAMESPACE
#endif
#ifndef SKYRIM_PIPELINE_CACHE_REUSE_REVIEWED
#define SKYRIM_PIPELINE_CACHE_REUSE_REVIEWED 0
#endif
namespace dxvk {
  DxvkPipelineCache::DxvkPipelineCache(const Rc<vk::DeviceFn>& vkd,
      const VkPhysicalDeviceProperties& properties) : m_vkd(vkd) {
    const char* reuse = std::getenv("SKYRIM_CACHE_REUSE");
    const bool reuseRequested = reuse && reuse[0] == '1' && !reuse[1];
    const char* sourceNamespace = reuseRequested
        ? SKYRIM_PIPELINE_CACHE_REUSE_NAMESPACE : SKYRIM_PIPELINE_CACHE_NAMESPACE;
    std::fprintf(stderr, "IOS_PIPE_CACHE event=identity reuse_requested=%u reuse_reviewed=%u\\n",
        unsigned(reuseRequested), unsigned(SKYRIM_PIPELINE_CACHE_REUSE_REVIEWED));
    m_disk = std::make_unique<skyrim::PipelineCache>(m_vkd->device(),
        m_vkd->vkCreatePipelineCache, m_vkd->vkGetPipelineCacheData,
        m_vkd->vkDestroyPipelineCache, properties,
        std::getenv("SKYRIM_VK_PIPELINE_CACHE_PATH"), sourceNamespace,
        std::getenv("SKYRIM_VK_PIPELINE_CACHE_IMPORT_PATH"));
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
    actual_meta = {p.relative_to(source).as_posix() for p in (source / "src/dxvk").glob("dxvk_meta*.cpp")}
    if actual_meta != set(META_DESCRIPTOR_BOUNDS):
        raise ValueError("Changed internal descriptor layout sources")
    for name, expected in META_DESCRIPTOR_BOUNDS.items():
        path = source / name
        if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_text(encoding="utf-8").encode()).hexdigest() != expected:
            raise ValueError("Changed internal descriptor bound: " + name)
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
