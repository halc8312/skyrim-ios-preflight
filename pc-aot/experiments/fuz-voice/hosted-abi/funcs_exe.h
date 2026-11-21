/* SPDX-License-Identifier: GPL-3.0-only */
/* Authored hosted declarations for the two reviewed Classic routines. */
#ifndef SKYRIM_FUZ_HOSTED_FUNCS_EXE_H
#define SKYRIM_FUZ_HOSTED_FUNCS_EXE_H
struct CPU;
void f_B08630(struct CPU *); // BSIStream constructor, thiscall(name, root).
void f_B08970(struct CPU *); // BSIStream destructor, thiscall(flags).
#endif
