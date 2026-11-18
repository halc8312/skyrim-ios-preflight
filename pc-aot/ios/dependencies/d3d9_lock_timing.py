"""Observe the pinned D3D9 device lock without changing its spin/recursion policy.

Only the device-local lock is instrumented. Generic DXVK recursive/spin locks
and all lock lifetimes remain unchanged. A failed weak CAS with owner zero is
not contention; same-thread recursion neither starts a wait nor changes owner.
"""
from pathlib import Path


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('D3D9 lock observer anchor changed')
    return text.replace(old, new, 1)


def adapt(name, text):
    if name.endswith('d3d9_multithread.h'):
        template = Path(__file__).with_name('d3d9_lock_timing.cpp.in').read_text(encoding='utf-8')
        text = once(text, '#include "d3d9_include.h"',
            '#include "d3d9_include.h"\n#include "../util/sync/sync_spinlock.h"\n'
            '#include "lock_metrics.h"\n#include "performance_metrics.h"')
        begin = text.index('  /**\n   * \\brief Device lock')
        end = text.index('  /**\n   * \\brief D3D9 context lock', begin)
        text = text[:begin] + template + '\n\n' + text[end:]
        text = once(text, '    D3D9DeviceLock AcquireLock() {',
            '    D3D9DeviceLock AcquireLock(unsigned operation = SKYRIM_LOCK_OP_D3D9_OTHER) {')
        text = once(text, '? D3D9DeviceLock(m_mutex)', '? D3D9DeviceLock(m_mutex, operation)')
        return once(text, '    sync::RecursiveSpinlock m_mutex;',
            '    D3D9ObservedRecursiveSpinlock m_mutex;')
    if name.endswith('d3d9_device.h'):
        return once(text, '''    D3D9DeviceLock LockDevice() {
      return m_multithread.AcquireLock();
    }''', '''    D3D9DeviceLock LockDevice(unsigned operation = SKYRIM_LOCK_OP_D3D9_OTHER) {
      return m_multithread.AcquireLock(operation);
    }''')
    if name.endswith('d3d9_swapchain.cpp'):
        begin = text.index('  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::Present(')
        end = text.index('  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetFrontBufferData(', begin)
        method = once(text[begin:end], 'D3D9DeviceLock lock = m_parent->LockDevice();',
            'D3D9DeviceLock lock = m_parent->LockDevice(SKYRIM_LOCK_OP_D3D9_PRESENT);')
        return text[:begin] + method + text[end:]
    raise ValueError('Unknown D3D9 lock observer input')
