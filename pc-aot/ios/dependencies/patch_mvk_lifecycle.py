#!/usr/bin/env python3
"""Route every pinned native commit through the application's lifecycle gate.

Queue execute scopes cover early enqueue as well as commit: closing a gate
between those operations can otherwise strand a later scheduled buffer.
No Vulkan result, Metal completion handler, semaphore, or resource is removed.
"""
import argparse
import hashlib
from pathlib import Path
import re

PINS = {
    'GPUObjects/MVKQueue.mm': 'd205c00ef1733d1183ea7ffe0519e8a687789dda9f66dc4f7a5183e053ca8699',
    'GPUObjects/MVKDevice.mm': '1561014e63e7a1300e1d5b4a17cdb783a8ee1e223346293421f02db5eedd9394',
    'GPUObjects/MVKImage.mm': '707d1fa4ddec9be1c9d519c3407c4fb3e166094f54fedc42a6e714c9bb609e88',
    'Commands/MVKCommandBuffer.mm': '4ddc0844ecb8b63117caec0581120d7c7bcf12a74eea196ed061f5cff559afc2',
}
COUNTS = {'GPUObjects/MVKQueue.mm': 3, 'GPUObjects/MVKDevice.mm': 1,
          'GPUObjects/MVKImage.mm': 2, 'Commands/MVKCommandBuffer.mm': 1}
DECL = b'''// Skyrim iOS lifecycle adapter: strong host symbols, no silent fallback.
extern "C" void skyrim_gpu_commit_metal(void *buffer);
'''
SCOPE = b'''extern "C" void skyrim_gpu_submission_enter(void);
extern "C" void skyrim_gpu_submission_leave(void);
struct SkyrimMetalSubmissionScope {
    SkyrimMetalSubmissionScope() { skyrim_gpu_submission_enter(); }
    ~SkyrimMetalSubmissionScope() { skyrim_gpu_submission_leave(); }
    SkyrimMetalSubmissionScope(const SkyrimMetalSubmissionScope&) = delete;
    SkyrimMetalSubmissionScope& operator=(const SkyrimMetalSubmissionScope&) = delete;
};
'''


# CPU semaphore waits must finish before admission and before early enqueue.
# Otherwise pause can wait for a consumer whose producer is blocked by pause.
# Native MTLEvent's nil encodeWait is a no-op in the pinned MVKSync.mm.
QUEUE_PREFLIGHT = b'''
	// Resolve CPU-side dependencies before admission/early enqueue. Native
	// event waits still encode at their original GPU command positions below.
	if (!_emulatedWaitDone) {
		for (auto& ws : _waitSemaphores) { ws.encodeWait(nil); }
		_emulatedWaitDone = true;
	}
	SkyrimMetalSubmissionScope iosSubmissionScope;'''
PRESENT_PREFLIGHT = b'''
	for (auto& ws : _waitSemaphores) { ws.encodeWait(nil); }
	SkyrimMetalSubmissionScope iosSubmissionScope;'''
PRESENT_INLINE_WAIT = b'\n\t\tws.encodeWait(nil);\t\t\t// Inline semaphore waits'


def patched(name, raw):
    if name not in PINS or hashlib.sha256(raw).hexdigest() != PINS[name]:
        raise ValueError('Changed MoltenVK lifecycle input: ' + name)
    result, count = re.subn(rb'\[([\w.]+) commit\];',
        lambda m: b'skyrim_gpu_commit_metal((__bridge void *)' + m[1] + b');', raw)
    if count != COUNTS[name]:
        raise ValueError('Native commit inventory changed')
    if name == 'GPUObjects/MVKQueue.mm':
        for owner in (b'MVKQueueCommandBufferSubmission', b'MVKQueuePresentSurfaceSubmission'):
            anchor = b'VkResult ' + owner + b'::execute() {'
            if result.count(anchor) != 1:
                raise ValueError('Submission scope changed')
            preflight = QUEUE_PREFLIGHT if owner == b'MVKQueueCommandBufferSubmission' else PRESENT_PREFLIGHT
            result = result.replace(anchor, anchor + preflight, 1)
        if result.count(PRESENT_INLINE_WAIT) != 1:
            raise ValueError('Inline present wait changed')
        result = result.replace(PRESENT_INLINE_WAIT, b'', 1)
        result = SCOPE + result
    return DECL + result


def apply(source, pristine):
    edits = {}
    sync = pristine / 'GPUObjects/MVKSync.mm'
    if hashlib.sha256(sync.read_bytes()).hexdigest() != 'c36370ad9689a635d88efcd626a872a9b77d8c087b43b886d35c969c58718593':
        raise ValueError('CPU/native semaphore wait semantics changed')
    # Review the entire pinned native tree, not only known callsites. Refuse a
    # new bare commit/enqueue which would bypass the strong application gate.
    commits = {}
    enqueues = {}
    for path in pristine.rglob('*'):
        if path.suffix not in ('.h', '.m', '.mm', '.cpp'): continue
        raw = path.read_bytes()
        if path.relative_to(pristine).as_posix() == 'GPUObjects/MVKDevice.h':
            # Metal residency-set commit applies allocation membership; it is
            # not an MTLCommandBuffer submission. Pin both reviewed callsites.
            if hashlib.sha256(raw).hexdigest() != '41cb6fb262f043d540c97f216bd8bb99e461ae7191b932854236d2cc8f71c586':
                raise ValueError('Residency-set commit inventory changed')
            raw = raw.replace(b'[_residencySet commit]', b'REVIEWED_RESIDENCY_COMMIT')
        commit = re.findall(rb'\[[^\]\n]+\s+commit\s*\]', raw)
        enqueue = re.findall(rb'\[[^\]\n]+\s+enqueue\s*\]', raw)
        if commit: commits[path.relative_to(pristine).as_posix()] = len(commit)
        if enqueue: enqueues[path.relative_to(pristine).as_posix()] = len(enqueue)
    if commits != COUNTS or enqueues != {'GPUObjects/MVKQueue.mm': 1}:
        raise ValueError('Native commit/enqueue source inventory changed')
    for name in PINS:
        original = (pristine / name).read_bytes()
        expected = patched(name, original)
        current = (source / name).read_bytes()
        if current not in (original, expected):
            raise ValueError('Unreviewed lifecycle stage: ' + name)
        edits[name] = expected
    for name, content in edits.items(): (source / name).write_bytes(content)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--pristine', type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.pristine.resolve():
        parser.error('keep pristine source separate')
    apply(args.source, args.pristine)
    print('Seven native commits and both submission scopes gated')
