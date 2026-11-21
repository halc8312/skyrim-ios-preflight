/* SPDX-License-Identifier: GPL-3.0-only
 * App-side declarations for experiments/fuz-voice/lift_resource_lookup.c.
 * The reviewed BSIStream constructor and destructor live in the guest; the
 * host reaches them through the runtime's dispatch by original address, with
 * the same stack frame the lifted direct call would use. */
#ifndef SKYRIM_FUZ_RUNTIME_FUNCS_EXE_H
#define SKYRIM_FUZ_RUNTIME_FUNCS_EXE_H
#include "x86.h"
void rt_call(CPU *, uint32_t);
static inline void f_B08630(CPU *c) { rt_call(c, 0x00B08630u); } /* BSIStream(name, root), thiscall */
static inline void f_B08970(CPU *c) { rt_call(c, 0x00B08970u); } /* ~BSIStream(flags), thiscall */
#endif
