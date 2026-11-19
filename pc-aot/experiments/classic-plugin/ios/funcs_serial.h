/* Static translation of the project's own serialization_fixture.c Windows DLL.
 * DLL SHA-256 107eb9644161fe19519d9fcf53ea02362cc76935f75529863fe53168c6d8e847.
 * Reproduce with verify_ios_translation.py. */
#pragma once
#include "x86.h"
void skyrim_serial_i386_query(CPU *c);
void skyrim_serial_i386_load(CPU *c);
void skyrim_serial_i386_revert(CPU *c);
void skyrim_serial_i386_save(CPU *c);
void skyrim_serial_i386_restore(CPU *c);
