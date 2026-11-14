"""Observe the pinned presentation path without changing waits or results."""


def adapt(name, text):
    from prepare_dxvk import once
    text = '#include "performance_metrics.h"\n' + text
    if name.endswith('vulkan_presenter.cpp'):
        # Includes speculative acquisition on the submit thread, not just the
        # application-thread acquire. Keep SUBOPTIMAL as a usable image.
        import re
        pattern = r'(    +)(m_acquireStatus = m_vkd->vkAcquireNextImageKHR\([\s\S]+?;)'
        text, count = re.subn(pattern, lambda m:
            m[1] + 'const uint64_t iosAcquireStarted = skyrim_perf_now_ns();\n' +
            m[1] + m[2] + '\n' + m[1] +
            'skyrim_perf_end(SKYRIM_PERF_IMAGE_ACQUIRE, iosAcquireStarted, m_acquireStatus < 0);', text)
        if count != 2:
            raise ValueError('Expected both image acquisition sites')
        anchor = '    VkResult status = m_vkd->vkQueuePresentKHR(m_device.queue, &info);'
        return once(text, anchor,
            '    const uint64_t iosPresentStarted = skyrim_perf_now_ns();\n' + anchor +
            '\n    skyrim_perf_end(SKYRIM_PERF_QUEUE_PRESENT, iosPresentStarted, status < 0);')
    if name.endswith('dxvk_cmdlist.cpp'):
        text = once(text, '  VkResult DxvkCommandList::synchronize() {',
            '  VkResult DxvkCommandList::synchronize() {\n    const uint64_t iosFenceStarted = skyrim_perf_now_ns();')
        text = once(text, '    return status;\n  }\n  \n  \n  void DxvkCommandList::beginRecording()',
            '    skyrim_perf_end(SKYRIM_PERF_FENCE_WAIT, iosFenceStarted, status != VK_SUCCESS);\n'
            '    return status;\n  }\n  \n  \n  void DxvkCommandList::beginRecording()')
        return once(text, '    return m_vkd->vkQueueSubmit(queue, 1, &submitInfo, fence);',
            '    const uint64_t iosSubmitStarted = skyrim_perf_now_ns();\n'
            '    const VkResult iosSubmitResult = m_vkd->vkQueueSubmit(queue, 1, &submitInfo, fence);\n'
            '    skyrim_perf_end(SKYRIM_PERF_QUEUE_SUBMIT, iosSubmitStarted, iosSubmitResult != VK_SUCCESS);\n'
            '    return iosSubmitResult;')
    if name.endswith('dxvk_queue.cpp'):
        anchor = '''    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [status] {
      return status->result.load() != VK_NOT_READY;
    });'''
        return once(text, anchor,
            '    const uint64_t iosSubmissionStarted = skyrim_perf_now_ns();\n' + anchor +
            '\n    skyrim_perf_end(SKYRIM_PERF_SUBMISSION_WAIT, iosSubmissionStarted, status->result.load() < 0);')
    raise ValueError('Unknown presentation timing input')
