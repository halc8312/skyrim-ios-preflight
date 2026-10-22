"""Prepare fresh public synthetic headers only. No game, SDK or network access.

The previously reviewed atomic-pointer overlay is applied with exact anchors.
No private-framework builder, input, generated layout or build entry is read.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
def overlay(text: str) -> str:
    changes=[
        ('extern "C" const int32_t skyrt_ea_offset[4096];','extern "C" const int32_t *skyrt_ea_offset;'),
        ('return ea + (uint32_t)skyrt_ea_offset[ea >> 20];','const int32_t *offsets = __atomic_load_n(&skyrt_ea_offset, __ATOMIC_ACQUIRE);\n    return ea + (uint32_t)__atomic_load_n(&offsets[ea >> 20], __ATOMIC_ACQUIRE);'),
        ('extern "C" uint8_t skyrt_written[];','extern "C" uint8_t *skyrt_written;'),
        ('((volatile uint8_t*)skyrt_written)[ea >> 12] = 0;','uint8_t *written = __atomic_load_n(&skyrt_written, __ATOMIC_ACQUIRE);\n    __atomic_store_n(&written[ea >> 12], (uint8_t)0, __ATOMIC_RELEASE);'),
        ('((volatile uint8_t*)skyrt_written)[(ea + size - 1) >> 12] = 0;','__atomic_store_n(&written[(ea + size - 1) >> 12], (uint8_t)0, __ATOMIC_RELEASE);'),
        ('#include <csetjmp>','#include <csetjmp>\nextern "C" int setjmp(jmp_buf) __attribute__((returns_twice));\nextern "C" void longjmp(jmp_buf, int) __attribute__((noreturn));'),
        ('extern "C" const int32_t *skyrt_ea_offset;','static_assert(sizeof(int32_t)==4 && alignof(int32_t)==4);\nstatic_assert(sizeof(uint8_t)==1 && alignof(uint8_t)==1);\nstatic_assert(__atomic_always_lock_free(4,nullptr) && __atomic_always_lock_free(1,nullptr));\nextern "C" const int32_t *skyrt_ea_offset;'),
        ('// leaves the page marked for next time. Plain byte stores suffice: the renderer\n// only reads memory after the guest published it through the ring buffer, which\n// orders everything before.','// leaves the page marked for next time. This public owned overlay uses release\n// atomic byte stores; ring synchronization remains a separate runtime contract.'),
    ]
    for old,new in changes:
        if text.count(old)!=1: raise ValueError('public context overlay anchor mismatch')
        text=text.replace(old,new)
    return text
def prepare(source: Path, generic: Path, output: Path) -> None:
    source=source.resolve(strict=True);generic=generic.resolve(strict=True)
    if output.exists() or output.resolve().is_relative_to(source) or output.resolve().is_relative_to(generic):
        raise ValueError('output must be fresh and outside public input trees')
    original=(source/'cpp/ppc_context.h').read_bytes()
    # Keep original bytes for the provenance hash/after-read check. Normalize
    # only the generated text so exact multi-line anchors work for LF and CRLF.
    text=overlay(original.decode('utf-8').replace('\r\n','\n'))
    output.mkdir(parents=False,exist_ok=False)
    (output/'ppc_context.h').write_text(text,encoding='utf-8',newline='\n')
    (output/'ppc_config.h').write_text('#pragma once\n#define PPC_CONFIG_H_INCLUDED\n#define PPC_IMAGE_BASE 0x82000000u\n#define PPC_IMAGE_SIZE 0x2000u\n#define PPC_CODE_BASE 0x82001000u\n#define PPC_CODE_SIZE 0x10u\n',encoding='utf-8',newline='\n')
    if (source/'cpp/ppc_context.h').read_bytes()!=original: raise ValueError('public input changed')
    print('public_header_sha256='+hashlib.sha256(original).hexdigest())

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',required=True,type=Path)
    parser.add_argument('--generic',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    prepare(args.source,args.generic,args.output)
