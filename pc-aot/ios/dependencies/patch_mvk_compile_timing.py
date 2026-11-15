#!/usr/bin/env python3
"""Observe the pinned Metal compiler dispatch/wait without changing its lifetime.

All hook arguments captured by the dispatch block are scalar values. No new
access to the compiler object occurs after block(), which may complete late and
destroy that object. Existing timeouts, locks, callbacks and error policy stay.
"""
import argparse
import hashlib
from pathlib import Path

PIN = 'c36370ad9689a635d88efcd626a872a9b77d8c087b43b886d35c969c58718593'
INCLUDE = b'#include "skyrim_performance_metrics.h"\n'
DISPATCH = b'\tdispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{ @autoreleasepool { block(); } });'
OBSERVED_DISPATCH = b'''\t// Numeric host observations only. The asynchronous block captures scalar
\t// timestamps; it must not inspect this after the original block returns.
\tconst uint64_t iosCompileStarted = skyrim_perf_now_ns();
\tconst SkyrimPerfContext iosWorkContext = skyrim_perf_context_get();
\tconst auto& iosStats = getPerformanceStats().shaderCompilation;
\tconst unsigned iosCompileKind =
\t\t_pPerformanceTracker == &iosStats.mslCompile ? SKYRIM_PERF_MVK_MSL_WAIT :
\t\t_pPerformanceTracker == &iosStats.functionSpecialization ? SKYRIM_PERF_MVK_SPECIALIZE_WAIT :
\t\t_pPerformanceTracker == &iosStats.pipelineCompile ? SKYRIM_PERF_MVK_PIPELINE_WAIT :
\t\tSKYRIM_PERF_MVK_OTHER_WAIT;
\tdispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
\t\tconst uint64_t iosDispatchStarted = skyrim_perf_now_ns();
\t\tskyrim_perf_interval_context(SKYRIM_PERF_MVK_DISPATCH_WAIT, iosCompileStarted, iosDispatchStarted, 0, 0, iosWorkContext);
\t\t@autoreleasepool { block(); }
\t\tskyrim_perf_interval_context(SKYRIM_PERF_MVK_DISPATCH_BLOCK, iosDispatchStarted, skyrim_perf_now_ns(), 0, 0, iosWorkContext);
\t});'''
END = b'\taddPerformanceInterval(*_pPerformanceTracker, _startTime);'
OBSERVED_END = END + b'\n\tskyrim_perf_end(iosCompileKind, iosCompileStarted, 0);'


def patched(raw):
    if hashlib.sha256(raw).hexdigest() != PIN:
        raise ValueError('Metal compiler source differs from the pinned revision')
    if raw.count(DISPATCH) != 1 or raw.count(END) != 1:
        raise ValueError('Metal compiler observation anchors changed')
    return INCLUDE + raw.replace(DISPATCH, OBSERVED_DISPATCH).replace(END, OBSERVED_END)


def original(raw):
    if not raw.startswith(INCLUDE) or raw.count(OBSERVED_DISPATCH) != 1 or raw.count(OBSERVED_END) != 1:
        raise ValueError('Incomplete Metal compiler observations')
    restored = raw[len(INCLUDE):].replace(OBSERVED_DISPATCH, DISPATCH).replace(OBSERVED_END, END)
    if patched(restored) != raw:
        raise ValueError('Unrelated changes in Metal compiler observations')
    return restored


def apply(source, header):
    path = source / 'MVKSync.mm'
    raw = path.read_bytes()
    candidate = patched(raw) if hashlib.sha256(raw).hexdigest() == PIN else patched(original(raw))
    metrics = header.read_bytes()
    # These hooks live in the app, not an optional diagnostic dynamic library.
    if b'SKYRIM_PERF_MVK_OTHER_WAIT' not in metrics:
        raise ValueError('Host timing ABI is older than the compiler observations')
    (source / 'skyrim_performance_metrics.h').write_bytes(metrics)
    path.write_bytes(candidate)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--header', type=Path, required=True)
    args = parser.parse_args()
    apply(args.source, args.header)
    print('Pinned Metal compiler observations present; execution policy unchanged')
