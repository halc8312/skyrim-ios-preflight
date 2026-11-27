#!/usr/bin/env python3
"""Conservative cache identity, with exact-reviewed observation-only exceptions.

An unknown revision of an observation file falls back to the full identity. It
never inherits compatibility merely because its filename sounds diagnostic.
Runtime OS/GPU/driver checks and the driver's opaque header remain independent.
"""
import argparse
import hashlib
import json
from pathlib import Path

INPUTS = (
    'source-lock.json', 'ios/dependencies/build-dependencies.sh',
    'ios/dependencies/patch_spirv_cross.py', 'ios/dependencies/patch_moltenvk.py',
    'ios/dependencies/prepare_dxvk.py', 'ios/dependencies/patch_mvk_hot_cache.py',
    'ios/dependencies/mvk_hot_cache.cpp.in', 'ios/dependencies/patch_mvk_module_cold.py',
    'ios/dependencies/patch_mvk_compressed_copy.py', 'ios/dependencies/verify_lifecycle_baseline.py',
    'ios/dependencies/patch_mvk_library.py', 'ios/dependencies/patch_mvk_compile_timing.py',
    'ios/runtime-port/performance_metrics.h', 'ios/dependencies/present_timing.py',
    'ios/dependencies/shader_inflight.cpp.in', 'ios/dependencies/prefetch_gate.h',
    'ios/dependencies/graphics_flights.h', 'ios/dependencies/graphics_policy.h',
    'ios/dependencies/graphics_state_methods.cpp.in',
    'ios/dependencies/pipeline_cache.h', 'ios/dependencies/pipeline_cache.cpp',
    'ios/runtime-port/prepare_runtime.py', 'ios/runtime-port/lifecycle_adapters.py',
    'ios/dependencies/lifecycle_device.py',
    'ios/dependencies/d3d9_lock_timing.py', 'ios/dependencies/d3d9_lock_timing.cpp.in',
    'ios/runtime-port/runtime_lock_timing.py', 'ios/runtime-port/runtime_lock_event.h.in',
    'ios/runtime-port/runtime_audio_lock.c.in', 'ios/runtime-port/runtime_com_lock.cpp.in',
    'ios/runtime-port/lock_metrics.h', 'ios/runtime-port/lock_metrics.c',
    'ios/runtime-port/water_probe.h', 'ios/runtime-port/water_probe.cpp',
)
OBSERVATION_INPUTS = frozenset((
    'ios/runtime-port/performance_metrics.h',
    'ios/dependencies/present_timing.py',
    'ios/dependencies/patch_mvk_compile_timing.py',
))
CONTRACT = 'ios/dependencies/pipeline_cache_compatibility.json'


def source_digest(data):
    return hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()


def identities(root, compiler, sdk, target):
    root = Path(root)
    hashes = {name: source_digest((root/name).read_bytes()) for name in INPUTS}
    # Include the identity algorithm itself. Changing validation must require a
    # new cache identity too; changing only the reviewed allow-list must not.
    hashes['ios/dependencies/pipeline_cache_namespace.py'] = source_digest(
        (root/'ios/dependencies/pipeline_cache_namespace.py').read_bytes())
    base = ['skyrim-source-v2', compiler, sdk, target]

    def fingerprint(items):
        return hashlib.sha256(json.dumps(base + sorted(items.items()),
            ensure_ascii=True, separators=(',', ':')).encode()).hexdigest()

    strict = fingerprint(hashes)
    reviewed = False
    compatible = dict(hashes)
    try:
        contract = json.loads((root/CONTRACT).read_text(encoding='utf-8'))
        if set(contract) != {'schema', 'epoch', 'observations'} or type(contract['schema']) is not int or contract['schema'] != 1:
            raise ValueError('unknown compatibility contract')
        if not isinstance(contract['epoch'], str) or not contract['epoch']:
            raise ValueError('empty compatibility epoch')
        observations = contract['observations']
        if set(observations) != OBSERVATION_INPUTS:
            raise ValueError('unexpected observation input')
        for name in OBSERVATION_INPUTS:
            accepted = observations[name]
            if not isinstance(accepted, list) or not accepted or any(
                not isinstance(h, str) or len(h) != 64 or any(c not in '0123456789abcdef' for c in h)
                for h in accepted):
                raise ValueError('invalid reviewed digest')
            if hashes[name] not in accepted:
                raise ValueError('unreviewed observation source')
            compatible[name] = 'reviewed-observation:' + contract['epoch']
        reviewed = True
    except (OSError, ValueError, TypeError, KeyError):
        pass  # Strict identity is the safe, explicit fallback, not an old hit.
    return dict(strict=strict, reuse=fingerprint(compatible) if reviewed else strict,
                reviewed=int(reviewed), inputs=[*hashes, CONTRACT])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--sdk', required=True)
    parser.add_argument('--target', required=True)
    args = parser.parse_args()
    print(json.dumps(identities(args.root, args.compiler, args.sdk, args.target), sort_keys=True))
