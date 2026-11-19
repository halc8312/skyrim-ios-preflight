/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Included by the staged skse_system.c after its VM and setting accessors.
 * skse_system_run has initialized the result type and its default value. */
static void skse_classic_ini_dispatch(CPU *cpu, unsigned operation,
        const uint32_t *arguments, uint32_t result) {
    uint32_t setting = skse_ini_setting(cpu, str(arguments[0]));
    if (!setting) return;
    uint32_t stored_name = M32(setting + 8);
    if (!stored_name) return;
    char type = str(stored_name)[0];
    switch (operation) {
    case S_INI_BOOL:
        if (type == 'b') M32(result + 4) = M8(setting + 4) != 0;
        break;
    case S_INI_INT:
        if (type == 'i') M32(result + 4) = M32(setting + 4);
        break;
    case S_INI_FLOAT:
        /* Preserve the exact float bits, including negative zero. */
        if (type == 'f') M32(result + 4) = M32(setting + 4);
        break;
    case S_INI_STRING:
        if (type == 's' || type == 'S') {
            uint32_t value = M32(setting + 4);
            if (value) vm_string(cpu, result, str(value));
        }
        break;
    case S_SET_INI_FLOAT:
        if (type == 'f') M32(setting + 4) = arguments[1];
        break;
    }
}
