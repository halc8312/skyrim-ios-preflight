/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* The Classic SDK registers UI.GetInt with UInt32 storage. Legacy MSVC x86
 * truncates via a signed 64-bit intermediate. An out-of-range conversion
 * produces an integer-indefinite value whose low 32 bits are zero. */
static uint32_t skse_classic_ui_integer(double number) {
    if (!(number >= -0x1p63 && number < 0x1p63)) return 0;
    return (uint32_t)(int64_t)number;
}

static void skse_classic_ui_result(CPU *cpu, unsigned op, uint32_t value, uint32_t result) {
    enum { GFX_BOOL = 2, GFX_NUMBER = 3, GFX_STRING = 4, GFX_MANAGED = 0x40, GFX_TYPE_MASK = 0x8f };
    unsigned tagged = M32(value + 4);
    unsigned type = tagged & GFX_TYPE_MASK;
    switch (op) {
    case S_UI_GBOOL:
        if (type == GFX_BOOL) M32(result + 4) = M8(value + 8) != 0;
        break;
    case S_UI_GINT:
        if (type == GFX_NUMBER) M32(result + 4) = skse_classic_ui_integer(MF64(value + 8));
        break;
    case S_UI_GFLOAT:
        if (type == GFX_NUMBER) M32(result + 4) = float_bits((float)MF64(value + 8));
        break;
    case S_UI_GSTRING:
        if (type == GFX_STRING) {
            uint32_t string = M32(value + 8);
            if (string && (tagged & GFX_MANAGED)) string = M32(string);
            if (string) vm_string(cpu, result, str(string));
        }
        break;
    }
}

/* The caller has cleared the VM result and assigned the requested Papyrus
 * type. A missing menu/target or incompatible GFx type keeps that default. */
static void skse_classic_ui_read(CPU *cpu, unsigned op, const uint32_t *args, uint32_t result) {
    if (!args[0] || !args[1]) return;
    uint32_t view = skse_menu_view(str(args[0]));
    if (!view) return;
    uint32_t value = gmalloc(16);
    memset(GPTR(value), 0, 16);
    /* GetVariable takes the original interned guest path synchronously. */
    uint32_t query[] = {value, args[1]};
    if ((uint8_t)skse_virtual(cpu, view, 17, 2, query))
        skse_classic_ui_result(cpu, op, value, result);
    skse_gfx_clean(cpu, value);
    gfree(value);
}
