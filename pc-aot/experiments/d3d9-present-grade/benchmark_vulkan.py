#!/usr/bin/env python3
"""Time the same 1× or 4× presentation SPIR-V on a physical Vulkan graphics queue."""

import argparse
from contextlib import ExitStack
from pathlib import Path
import random
import re
import statistics
import struct

import vulkan as vk

from prepare import pinned


def shader_header(path, sha256):
    words = re.findall(r"0x([0-9a-fA-F]{8})\b", pinned(path, sha256))
    return struct.pack(f"<{len(words)}I", *(int(word, 16) for word in words))


class OffscreenDraw:
    def __init__(self, cleanup, width, height):
        self.cleanup = cleanup
        self.extent = vk.VkExtent2D(width=width, height=height)
        self.area = vk.VkRect2D(offset=vk.VkOffset2D(x=0, y=0), extent=self.extent)
        info = vk.VkInstanceCreateInfo(
            pApplicationInfo=vk.VkApplicationInfo(
                pApplicationName="Present grade benchmark", apiVersion=vk.VK_API_VERSION_1_0
            )
        )
        instance = vk.vkCreateInstance(info, None)
        cleanup.callback(vk.vkDestroyInstance, instance, None)
        for adapter in vk.vkEnumeratePhysicalDevices(instance):
            self.properties = vk.vkGetPhysicalDeviceProperties(adapter)
            queues = vk.vkGetPhysicalDeviceQueueFamilyProperties(adapter)
            hardware = self.properties.deviceType in (
                vk.VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                vk.VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
            )
            self.family = next(
                (
                    i
                    for i, q in enumerate(queues)
                    if q.queueFlags & vk.VK_QUEUE_GRAPHICS_BIT and q.timestampValidBits
                ),
                None,
            )
            if hardware and self.family is not None:
                break
        else:
            raise ValueError("A physical GPU with graphics queue timestamps is required")
        self.adapter = adapter
        self.tick_mask = (1 << queues[self.family].timestampValidBits) - 1
        self.memory = vk.vkGetPhysicalDeviceMemoryProperties(adapter)
        qinfo = vk.VkDeviceQueueCreateInfo(queueFamilyIndex=self.family, pQueuePriorities=[1.0])
        self.device = vk.vkCreateDevice(adapter, vk.VkDeviceCreateInfo(pQueueCreateInfos=[qinfo]), None)
        cleanup.callback(vk.vkDestroyDevice, self.device, None)
        self.queue = vk.vkGetDeviceQueue(self.device, self.family, 0)
        pool = self.owned(
            "CommandPool",
            vk.VkCommandPoolCreateInfo(
                queueFamilyIndex=self.family, flags=vk.VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
            ),
        )
        self.cmd = vk.vkAllocateCommandBuffers(
            self.device,
            vk.VkCommandBufferAllocateInfo(
                commandPool=pool, level=vk.VK_COMMAND_BUFFER_LEVEL_PRIMARY, commandBufferCount=1
            ),
        )[0]
        self.timestamps = self.owned(
            "QueryPool", vk.VkQueryPoolCreateInfo(queryType=vk.VK_QUERY_TYPE_TIMESTAMP, queryCount=2)
        )
        self.rng = vk.VkImageSubresourceRange(
            aspectMask=vk.VK_IMAGE_ASPECT_COLOR_BIT, levelCount=1, layerCount=1
        )
        self.layer = vk.VkImageSubresourceLayers(aspectMask=vk.VK_IMAGE_ASPECT_COLOR_BIT, layerCount=1)

    def owned(self, kind, info):
        obj = getattr(vk, "vkCreate" + kind)(self.device, info, None)
        self.cleanup.callback(getattr(vk, "vkDestroy" + kind), self.device, obj, None)
        return obj

    def allocate(self, kind, info, flags):
        obj = getattr(vk, "vkCreate" + kind)(self.device, info, None)
        destroy = getattr(vk, "vkDestroy" + kind)
        memory = None
        try:
            req = getattr(vk, "vkGet" + kind + "MemoryRequirements")(self.device, obj)
            index = next(
                i
                for i in range(self.memory.memoryTypeCount)
                if req.memoryTypeBits & (1 << i) and self.memory.memoryTypes[i].propertyFlags & flags == flags
            )
            spec = vk.VkMemoryAllocateInfo(allocationSize=req.size, memoryTypeIndex=index)
            memory = vk.vkAllocateMemory(self.device, spec, None)
            getattr(vk, "vkBind" + kind + "Memory")(self.device, obj, memory, 0)
        except BaseException:
            destroy(self.device, obj, None)
            if memory is not None:
                vk.vkFreeMemory(self.device, memory, None)
            raise
        self.cleanup.callback(vk.vkFreeMemory, self.device, memory, None)
        self.cleanup.callback(destroy, self.device, obj, None)
        return obj, memory

    def image(self, usage, one_dimensional=False, samples=1):
        w, h = (1, 1) if one_dimensional else (self.extent.width, self.extent.height)
        kind = vk.VK_IMAGE_TYPE_1D if one_dimensional else vk.VK_IMAGE_TYPE_2D
        view_kind = vk.VK_IMAGE_VIEW_TYPE_1D if one_dimensional else vk.VK_IMAGE_VIEW_TYPE_2D
        fmt = vk.VK_FORMAT_R8G8B8A8_UNORM
        info = vk.VkImageCreateInfo(
            imageType=kind,
            format=fmt,
            extent=vk.VkExtent3D(width=w, height=h, depth=1),
            mipLevels=1,
            arrayLayers=1,
            samples=samples,
            tiling=vk.VK_IMAGE_TILING_OPTIMAL,
            usage=usage,
            sharingMode=vk.VK_SHARING_MODE_EXCLUSIVE,
            initialLayout=vk.VK_IMAGE_LAYOUT_UNDEFINED,
        )
        img, _ = self.allocate("Image", info, vk.VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        view = self.owned(
            "ImageView",
            vk.VkImageViewCreateInfo(image=img, viewType=view_kind, format=fmt, subresourceRange=self.rng),
        )
        return img, view

    def barrier(self, image, old, new, source_stage, dest_stage, source_access, dest_access):
        transition = vk.VkImageMemoryBarrier(
            oldLayout=old,
            newLayout=new,
            srcAccessMask=source_access,
            dstAccessMask=dest_access,
            srcQueueFamilyIndex=vk.VK_QUEUE_FAMILY_IGNORED,
            dstQueueFamilyIndex=vk.VK_QUEUE_FAMILY_IGNORED,
            image=image,
            subresourceRange=self.rng,
        )
        vk.vkCmdPipelineBarrier(self.cmd, source_stage, dest_stage, 0, 0, None, 0, None, 1, [transition])

    def begin(self):
        vk.vkResetCommandBuffer(self.cmd, 0)
        vk.vkBeginCommandBuffer(
            self.cmd, vk.VkCommandBufferBeginInfo(flags=vk.VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
        )

    def finish(self):
        vk.vkEndCommandBuffer(self.cmd)
        submit = vk.VkSubmitInfo(pCommandBuffers=[self.cmd])
        vk.vkQueueSubmit(self.queue, 1, [submit], vk.VK_NULL_HANDLE)
        vk.vkQueueWaitIdle(self.queue)

    def copy_region(self, width, height, x=0, y=0, buffer_offset=0):
        return vk.VkBufferImageCopy(
            bufferOffset=buffer_offset,
            imageSubresource=self.layer,
            imageOffset=vk.VkOffset3D(x=x, y=y, z=0),
            imageExtent=vk.VkExtent3D(width=width, height=height, depth=1),
        )

    def prepare_targets(self):
        width, height = self.extent.width, self.extent.height
        data = random.Random(17).randbytes(4 * width * height)
        self.points = [
            (0, 0),
            (width - 1, 0),
            (width - 1, height - 1),
            (0, height - 1),
            (width // 2, height // 2),
        ]
        self.source_pixels = [data[(y * width + x) * 4 : (y * width + x + 1) * 4] for x, y in self.points]
        self.buffer, self.host_memory = self.allocate(
            "Buffer",
            vk.VkBufferCreateInfo(
                size=len(data),
                usage=vk.VK_BUFFER_USAGE_TRANSFER_SRC_BIT | vk.VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                sharingMode=vk.VK_SHARING_MODE_EXCLUSIVE,
            ),
            vk.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | vk.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        )
        mapped = vk.vkMapMemory(self.device, self.host_memory, 0, len(data), 0)
        mapped[:] = data
        vk.vkUnmapMemory(self.device, self.host_memory)
        input_usage = vk.VK_IMAGE_USAGE_TRANSFER_DST_BIT | vk.VK_IMAGE_USAGE_SAMPLED_BIT
        source, src_view = self.image(input_usage)
        gamma, gamma_view = self.image(input_usage, one_dimensional=True)
        self.destination, dst_view = self.image(
            vk.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | vk.VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        )
        transfer = vk.VK_PIPELINE_STAGE_TRANSFER_BIT
        writable = vk.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
        sampled = vk.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        self.begin()
        for image in (source, gamma):
            self.barrier(
                image,
                vk.VK_IMAGE_LAYOUT_UNDEFINED,
                writable,
                vk.VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                transfer,
                0,
                vk.VK_ACCESS_TRANSFER_WRITE_BIT,
            )
        vk.vkCmdCopyBufferToImage(self.cmd, self.buffer, source, writable, 1, [self.copy_region(width, height)])
        vk.vkCmdClearColorImage(
            self.cmd, gamma, writable, vk.VkClearColorValue(float32=[0, 0, 0, 1]), 1, [self.rng]
        )
        for image in (source, gamma):
            self.barrier(
                image,
                writable,
                sampled,
                transfer,
                vk.VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                vk.VK_ACCESS_TRANSFER_WRITE_BIT,
                vk.VK_ACCESS_SHADER_READ_BIT,
            )
        self.barrier(
            self.destination,
            vk.VK_IMAGE_LAYOUT_UNDEFINED,
            vk.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            vk.VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            vk.VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            vk.VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        )
        self.finish()
        return src_view, gamma_view, dst_view

    def prepare_pipeline_layout(self, src, gamma, dst):
        combined = vk.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
        fragment = vk.VK_SHADER_STAGE_FRAGMENT_BIT
        bindings = [
            vk.VkDescriptorSetLayoutBinding(
                binding=i, descriptorType=combined, descriptorCount=1, stageFlags=fragment
            )
            for i in range(2)
        ]
        layout = self.owned("DescriptorSetLayout", vk.VkDescriptorSetLayoutCreateInfo(pBindings=bindings))
        pool = self.owned(
            "DescriptorPool",
            vk.VkDescriptorPoolCreateInfo(
                maxSets=1, pPoolSizes=[vk.VkDescriptorPoolSize(type=combined, descriptorCount=2)]
            ),
        )
        self.desc = vk.vkAllocateDescriptorSets(
            self.device, vk.VkDescriptorSetAllocateInfo(descriptorPool=pool, pSetLayouts=[layout])
        )[0]
        address = vk.VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
        self.sampler = self.owned(
            "Sampler",
            vk.VkSamplerCreateInfo(
                magFilter=vk.VK_FILTER_NEAREST,
                minFilter=vk.VK_FILTER_NEAREST,
                addressModeU=address,
                addressModeV=address,
                addressModeW=address,
            ),
        )
        self.bind_images(src, gamma)
        self.layout = self.owned(
            "PipelineLayout",
            vk.VkPipelineLayoutCreateInfo(
                pSetLayouts=[layout],
                pPushConstantRanges=[vk.VkPushConstantRange(stageFlags=fragment, offset=0, size=16)],
            ),
        )
        self.render_pass, self.fb = self.framebuffer(dst, 1)

    def bind_images(self, src, gamma):
        writes = [
            vk.VkWriteDescriptorSet(
                dstSet=self.desc,
                dstBinding=i,
                descriptorType=vk.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                pImageInfo=[
                    vk.VkDescriptorImageInfo(
                        sampler=self.sampler,
                        imageView=view,
                        imageLayout=vk.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    )
                ],
            )
            for i, view in enumerate((src, gamma))
        ]
        vk.vkUpdateDescriptorSets(self.device, len(writes), writes, 0, None)

    def framebuffer(self, dst, samples):
        color = vk.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        attachment = vk.VkAttachmentDescription(
            format=vk.VK_FORMAT_R8G8B8A8_UNORM,
            samples=samples,
            loadOp=vk.VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            storeOp=vk.VK_ATTACHMENT_STORE_OP_STORE,
            stencilLoadOp=vk.VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            stencilStoreOp=vk.VK_ATTACHMENT_STORE_OP_DONT_CARE,
            initialLayout=color,
            finalLayout=color,
        )
        subpass = vk.VkSubpassDescription(
            pipelineBindPoint=vk.VK_PIPELINE_BIND_POINT_GRAPHICS,
            pColorAttachments=[vk.VkAttachmentReference(attachment=0, layout=color)],
        )
        write = vk.VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        stage = vk.VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        order = vk.VkSubpassDependency(
            srcSubpass=vk.VK_SUBPASS_EXTERNAL,
            dstSubpass=0,
            srcStageMask=stage,
            dstStageMask=stage,
            srcAccessMask=write,
            dstAccessMask=write,
        )
        render_pass = self.owned(
            "RenderPass",
            vk.VkRenderPassCreateInfo(pAttachments=[attachment], pSubpasses=[subpass], pDependencies=[order]),
        )
        framebuffer = self.owned(
            "Framebuffer",
            vk.VkFramebufferCreateInfo(
                renderPass=render_pass,
                pAttachments=[dst],
                width=self.extent.width,
                height=self.extent.height,
                layers=1,
            ),
        )
        return render_pass, framebuffer

    def pipeline(self, vert, frag, source_samples=1, target_samples=1, mask=None, render_pass=None):
        # DXVK includes its present-sample specialization at SPIR-V SpecId 1225.
        values = vk.ffi.new("uint32_t[2]", [0, source_samples])
        entries = [vk.VkSpecializationMapEntry(constantID=1, size=4)]
        if source_samples > 1:
            entries.append(vk.VkSpecializationMapEntry(constantID=1225, offset=4, size=4))
        options = vk.VkSpecializationInfo(pMapEntries=entries, dataSize=8, pData=values)
        stages = []
        for code, stage, spec in [
            (vert, vk.VK_SHADER_STAGE_VERTEX_BIT, None),
            (frag, vk.VK_SHADER_STAGE_FRAGMENT_BIT, options),
        ]:
            module = self.owned("ShaderModule", vk.VkShaderModuleCreateInfo(codeSize=len(code), pCode=code))
            stages.append(
                vk.VkPipelineShaderStageCreateInfo(
                    stage=stage, module=module, pName="main", pSpecializationInfo=spec
                )
            )
        vp = vk.VkViewport(width=self.extent.width, height=self.extent.height, minDepth=0.0, maxDepth=1.0)
        info = vk.VkGraphicsPipelineCreateInfo(
            pStages=stages,
            pVertexInputState=vk.VkPipelineVertexInputStateCreateInfo(),
            pInputAssemblyState=vk.VkPipelineInputAssemblyStateCreateInfo(
                topology=vk.VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
            ),
            pViewportState=vk.VkPipelineViewportStateCreateInfo(pViewports=[vp], pScissors=[self.area]),
            pRasterizationState=vk.VkPipelineRasterizationStateCreateInfo(
                polygonMode=vk.VK_POLYGON_MODE_FILL, cullMode=vk.VK_CULL_MODE_NONE, lineWidth=1.0
            ),
            pMultisampleState=vk.VkPipelineMultisampleStateCreateInfo(
                rasterizationSamples=target_samples, pSampleMask=[mask] if mask is not None else None
            ),
            pColorBlendState=vk.VkPipelineColorBlendStateCreateInfo(
                pAttachments=[vk.VkPipelineColorBlendAttachmentState(colorWriteMask=15)]
            ),
            layout=self.layout,
            renderPass=render_pass if render_pass is not None else self.render_pass,
            subpass=0,
        )
        pipeline = vk.vkCreateGraphicsPipelines(self.device, vk.VK_NULL_HANDLE, 1, [info], None)[0]
        self.cleanup.callback(vk.vkDestroyPipeline, self.device, pipeline, None)
        return pipeline

    def bind_draw_inputs(self):
        cmd = self.cmd
        vk.vkCmdBindDescriptorSets(
            cmd, vk.VK_PIPELINE_BIND_POINT_GRAPHICS, self.layout, 0, 1, [self.desc], 0, None
        )
        vk.vkCmdPushConstants(
            cmd, self.layout, vk.VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, vk.ffi.new("uint32_t[4]")
        )

    def multisample_source(self, gamma_view, vert, plain, graded):
        usage = vk.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | vk.VK_IMAGE_USAGE_SAMPLED_BIT
        props = vk.vkGetPhysicalDeviceImageFormatProperties(
            self.adapter, vk.VK_FORMAT_R8G8B8A8_UNORM, vk.VK_IMAGE_TYPE_2D, vk.VK_IMAGE_TILING_OPTIMAL, usage, 0
        )
        if not props.sampleCounts & vk.VK_SAMPLE_COUNT_4_BIT:
            raise ValueError("This GPU cannot sample the 4× color fixture")
        image, view = self.image(usage, samples=4)
        render_pass, fb = self.framebuffer(view, 4)
        plain_pipe = self.pipeline(vert, plain, target_samples=4, render_pass=render_pass)
        half_pipe = self.pipeline(vert, graded, target_samples=4, mask=0b0101, render_pass=render_pass)
        color = vk.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        stage = vk.VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        self.begin()
        self.barrier(
            image,
            vk.VK_IMAGE_LAYOUT_UNDEFINED,
            color,
            vk.VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            stage,
            0,
            vk.VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        )
        self.bind_draw_inputs()
        begin = vk.VkRenderPassBeginInfo(renderPass=render_pass, framebuffer=fb, renderArea=self.area)
        vk.vkCmdBeginRenderPass(self.cmd, begin, vk.VK_SUBPASS_CONTENTS_INLINE)
        # Paint all four samples first, then alter two of them. No setup commands are timed.
        for pipeline in (plain_pipe, half_pipe):
            vk.vkCmdBindPipeline(self.cmd, vk.VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline)
            vk.vkCmdDraw(self.cmd, 3, 1, 0, 0)
        vk.vkCmdEndRenderPass(self.cmd)
        self.barrier(
            image,
            color,
            vk.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            stage,
            vk.VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            vk.VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            vk.VK_ACCESS_SHADER_READ_BIT,
        )
        self.finish()
        self.bind_images(view, gamma_view)

    def batch(self, pipeline, draws):
        self.begin()
        cmd = self.cmd
        vk.vkCmdResetQueryPool(cmd, self.timestamps, 0, 2)
        vk.vkCmdWriteTimestamp(cmd, vk.VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, self.timestamps, 0)
        vk.vkCmdBindPipeline(cmd, vk.VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline)
        self.bind_draw_inputs()
        begin = vk.VkRenderPassBeginInfo(renderPass=self.render_pass, framebuffer=self.fb, renderArea=self.area)
        for _ in range(draws):
            vk.vkCmdBeginRenderPass(cmd, begin, vk.VK_SUBPASS_CONTENTS_INLINE)
            vk.vkCmdDraw(cmd, 3, 1, 0, 0)
            vk.vkCmdEndRenderPass(cmd)
        vk.vkCmdWriteTimestamp(cmd, vk.VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, self.timestamps, 1)
        color, transfer = vk.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, vk.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
        output_stage, transfer_stage = (
            vk.VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            vk.VK_PIPELINE_STAGE_TRANSFER_BIT,
        )
        self.barrier(
            self.destination,
            color,
            transfer,
            output_stage,
            transfer_stage,
            vk.VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            vk.VK_ACCESS_TRANSFER_READ_BIT,
        )
        regions = [self.copy_region(1, 1, x, y, i * 4) for i, (x, y) in enumerate(self.points)]
        vk.vkCmdCopyImageToBuffer(cmd, self.destination, transfer, self.buffer, len(regions), regions)
        host = vk.VkMemoryBarrier(
            srcAccessMask=vk.VK_ACCESS_TRANSFER_WRITE_BIT, dstAccessMask=vk.VK_ACCESS_HOST_READ_BIT
        )
        vk.vkCmdPipelineBarrier(
            cmd, transfer_stage, vk.VK_PIPELINE_STAGE_HOST_BIT, 0, 1, [host], 0, None, 0, None
        )
        self.barrier(
            self.destination,
            transfer,
            color,
            transfer_stage,
            output_stage,
            vk.VK_ACCESS_TRANSFER_READ_BIT,
            vk.VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        )
        self.finish()
        times = vk.ffi.new("uint64_t[2]")
        vk.vkGetQueryPoolResults(self.device, self.timestamps, 0, 2, 16, times, 8, vk.VK_QUERY_RESULT_64_BIT)
        packed = bytes(vk.vkMapMemory(self.device, self.host_memory, 0, 4 * len(self.points), 0))
        vk.vkUnmapMemory(self.device, self.host_memory)
        us = ((times[1] - times[0]) & self.tick_mask) * self.properties.limits.timestampPeriod / 1000.0
        return us / draws, [packed[i : i + 4] for i in range(0, len(packed), 4)]


def warm_rgb(rgb):
    r, g, b = rgb
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    return [round(min(255, (0.9 * v + 0.1 * luma) * gain)) for v, gain in zip(rgb, (1.08, 1.0, 0.90))]


def benchmark(dxvk, compiled, width, height, sample_count):
    vert = shader_header(
        dxvk / "generated/dxvk_present_vert.h",
        "f4f879da2f12a28c4773fad6f41ddc2ee0ea082d2e7561ebd67de0af422a4065",
    )
    plain = shader_header(
        dxvk / "generated/dxvk_present_frag.h",
        "62e0c7b63b221c62a9444b72890b239685aefe33419b86c89587624c15e5d5ab",
    )
    graded = (compiled / "dxvk_present_frag.spv").read_bytes()
    with ExitStack() as cleanup:
        gpu = OffscreenDraw(cleanup, width, height)
        src, gamma, dst = gpu.prepare_targets()
        gpu.prepare_pipeline_layout(src, gamma, dst)
        if sample_count == 4:
            gpu.multisample_source(gamma, vert, plain, graded)
            plain = shader_header(
                dxvk / "generated/dxvk_present_frag_ms.h",
                "c9b4492278cc0fbac0de16ae761eefd36f154e759eef326ce7d8bfcc1dff6382",
            )
            graded = (compiled / "dxvk_present_frag_ms.spv").read_bytes()
        pipes = [gpu.pipeline(vert, frag, source_samples=sample_count) for frag in (plain, graded)]

        def correct(actual, original, grade):
            if not grade and sample_count == 1:
                return actual == original
            rgb = original[:3]
            if sample_count == 4:
                rgb = [(a + b) / 2 for a, b in zip(rgb, warm_rgb(rgb))]
            reference = warm_rgb(rgb) if grade else [round(v) for v in rgb]
            return actual[3] == original[3] and all(
                abs(actual[j] - ref) <= 1 for j, ref in enumerate(reference)
            )

        samples = [[], []]
        for i in range(15):
            for variant in (1, 0) if i % 2 else (0, 1):
                duration, pixels = gpu.batch(pipes[variant], 16)
                for actual, original in zip(pixels, gpu.source_pixels):
                    if not correct(actual, original, variant):
                        raise ValueError(f"Unexpected probe pixel for variant {variant}: {actual.hex()}")
                if i >= 3:
                    samples[variant].append(duration)
        old, warm = samples
        change = statistics.median(y - x for x, y in zip(old, warm))
        print(f"{gpu.properties.deviceName} {width}x{height}, {sample_count}x, 12 pairs of 16 draws")
        print(
            f"queue us/draw original={statistics.median(old):.3f} warm={statistics.median(warm):.3f} "
            f"paired_difference={change:.3f}"
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk", type=Path, required=True)
    parser.add_argument("--compiled", type=Path, required=True)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--samples", type=int, choices=(1, 4), default=1)
    args = parser.parse_args()
    if not (16 <= args.width <= 4096 and 16 <= args.height <= 4096):
        parser.error("Choose dimensions between 16 and 4096")
    benchmark(args.dxvk, args.compiled, args.width, args.height, args.samples)
