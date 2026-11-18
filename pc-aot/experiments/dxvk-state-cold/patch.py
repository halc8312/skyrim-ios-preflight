#!/usr/bin/env python3
"""Retained historical graphics-state patch and independent serial oracle.

Current production stages already contain startup-selected OFF/ON routes and are
intentionally rejected by this historical patch. Reproduce the current route
with test_graphics_state_flights.py / test_graphics_state_flights_metal.py;
SKYRIM_EXACT_GRAPHICS_STATE=1 is the exact startup opt-in, default OFF.
This patch accepts only a separate historical serial baseline or its own output.
Production imports no files from this experiment directory.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = ROOT / 'vendor/skydream/third_party/dxvk'
NAMES = ('src/dxvk/dxvk_graphics.h', 'src/dxvk/dxvk_graphics.cpp')
spec = importlib.util.spec_from_file_location('state_cold_baseline', ROOT / 'ios/dependencies/prepare_dxvk.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def baseline(name: str) -> str:
    if name not in NAMES:
        raise ValueError('Unsupported graphics experiment source')
    source = SOURCE / name
    if source.is_symlink() or not source.is_file():
        raise ValueError('Pinned DXVK source missing or linked')
    text = source.read_text(encoding='utf-8')
    if hashlib.sha256(text.encode()).hexdigest() != adapter.EXPECTED[name]:
        raise ValueError('Pinned graphics source fingerprint changed')
    # Historical independent serial baseline. Production now contains both
    # startup-selected routes; never feed that composition back into this patch.
    serial = adapter.adapt_pipeline_locking_baseline(name, text)
    return adapter.adapt_timing(name, serial) if name.endswith('.cpp') else serial


def candidate(name: str, text: str) -> str:
    if name.endswith('.h'):
        text = adapter.once(text, '#include <mutex>',
            '#include <mutex>\n#include <optional>\n#include "graphics_flights.h"')
        text = adapter.once(text,
            '    // Slow compiler calls never hold the instance-list spinlock.\n    dxvk::mutex m_compileMutex;',
            '    // Experimental exact-state flights; no global cold compile mutex.\n'
            '    skyrim::GraphicsFlights<DxvkGraphicsPipelineStateInfo, DxvkRenderPass> m_compileFlights;')
        return adapter.once(text, '    DxvkGraphicsPipelineInstance* createInstance(',
            '    std::optional<VkPipeline> createInstance(')
    begin = text.index('  VkPipeline DxvkGraphicsPipeline::getPipelineHandle(')
    end = text.index('  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::findInstance(', begin)
    methods = (HERE / 'graphics_methods.cpp.in').read_text(encoding='utf-8').rstrip() + '\n\n\n'
    return text[:begin] + methods + text[end:]


def patched(name: str, text: str) -> str:
    old = baseline(name)
    new = candidate(name, old)
    if text not in (old, new):
        raise ValueError('Graphics experiment expects the exact staged baseline')
    return new


def apply(stage: Path) -> None:
    stage = stage.resolve(strict=True)
    # Refuse every vendor tree or source checkout ancestor, including symlinks.
    vendor = (ROOT / 'vendor').resolve()
    if stage == ROOT or stage in ROOT.parents or stage == vendor or vendor in stage.parents:
        raise ValueError('Use a separate staged tree, never the source or vendor')
    outputs = {}
    for name in NAMES:
        path = stage / name
        if path.is_symlink() or not path.is_file() or stage not in path.resolve().parents:
            raise ValueError('Staged graphics target is missing or linked outside stage')
        outputs[path] = patched(name, path.read_text(encoding='utf-8'))
    helper = stage / 'src/dxvk/graphics_flights.h'
    support = (HERE / 'graphics_flights.h').read_text(encoding='utf-8')
    if helper.is_symlink() or stage not in helper.resolve().parents:
        raise ValueError('Staged graphics helper is linked outside stage')
    if helper.exists() and helper.read_text(encoding='utf-8') != support:
        raise ValueError('Unrecognized existing graphics helper')
    outputs[helper] = support
    # Validate both input files and helper before modifying any staged copy.
    for path, text in outputs.items():
        path.write_text(text, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', type=Path)
    args = parser.parse_args()
    apply(args.stage)
    print('Experimental graphics flights applied to the separate staged tree only')
