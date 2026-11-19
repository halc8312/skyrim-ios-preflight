/* Static translation of the project's own serialization_fixture.c Windows DLL.
 * DLL SHA-256 107eb9644161fe19519d9fcf53ea02362cc76935f75529863fe53168c6d8e847.
 * Reproduce with verify_ios_translation.py. */
#include "funcs_serial.h"
#include "lift_prelude.h"

void skyrim_serial_i386_query(CPU *c) {
    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Flags f = c->flags;
    uint32_t jt_ = 0;
    (void)f; (void)jt_;
L_101000:
    ecx = (uint32_t)(M32((uint32_t)(esp + 0x8u)));
    eax = (uint32_t)(M32((uint32_t)(esp + 0x4u)));
    (void)logic32((uint32_t)(eax & eax), &f);
    edx = (edx & 0xFFFFFF00u) | (uint8_t)(f.zf);
    (void)logic32((uint32_t)(ecx & ecx), &f);
    edx = (edx & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(f.zf) << 8);
    edx = (edx & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(logic8((uint8_t)(((uint8_t)(edx >> 8)) | ((uint8_t)edx)), &f)) << 8);
    if (!f.zf) goto L_101043;
    goto L_101016;
L_101016:
    TS32((uint32_t)(ecx), (uint32_t)((uint32_t)0x1u));
    TS32((uint32_t)(ecx + 0x4u), (uint32_t)((uint32_t)0x102000u));
    TS32((uint32_t)(ecx + 0x8u), (uint32_t)((uint32_t)0x2u));
    (void)sub32(TL32((uint32_t)(eax)), (uint32_t)0x1070030u, &f);
    if (!f.zf) goto L_101043;
    goto L_101032;
L_101032:
    (void)sub32(TL32((uint32_t)(eax + 0x4u)), (uint32_t)0x9200000u, &f);
    if (!f.zf) goto L_101043;
    goto L_10103B;
L_10103B:
    (void)sub32(TL32((uint32_t)(eax + 0xCu)), (uint32_t)0x0u, &f);
    eax = (eax & 0xFFFFFF00u) | (uint8_t)(f.zf);
    esp += 4; SPILL(); return;
L_101043:
    eax = (uint32_t)(logic32(0, &f));
    esp += 4; SPILL(); return;
}

void skyrim_serial_i386_load(CPU *c) {
    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Flags f = c->flags;
    uint32_t jt_ = 0;
    (void)f; (void)jt_;
L_101050:
    { uint32_t v_ = edi; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = esi; esp -= 4; M32(esp) = v_; }
    esi = (uint32_t)(M32((uint32_t)(esp + 0xCu)));
    (void)logic32((uint32_t)(esi & esi), &f);
    if (f.zf) goto L_1010C4;
    goto L_10105A;
L_10105A:
    eax = (uint32_t)(TL32((uint32_t)(esi + 0x10u)));
    (void)logic32((uint32_t)(eax & eax), &f);
    if (f.zf) goto L_1010C4;
    goto L_101061;
L_101061:
    (void)sub32(TL32((uint32_t)(esi + 0x14u)), (uint32_t)0x0u, &f);
    if (f.zf) goto L_1010C4;
    goto L_101067;
L_101067:
    { uint32_t v_ = 0x3u; esp -= 4; M32(esp) = v_; }
    { uint32_t t_ = eax; esp -= 4; M32(esp) = 0x10106Bu; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0x4u, &f));
    (void)logic32((uint32_t)(eax & eax), &f);
    if (f.zf) goto L_1010C4;
    goto L_101072;
L_101072:
    (void)sub32(TL32((uint32_t)(eax)), (uint32_t)0x4u, &f);
    if (!f.zf) goto L_1010C4;
    goto L_101077;
L_101077:
    edi = (uint32_t)(eax);
    { uint32_t t_ = TL32((uint32_t)(esi + 0x14u)); esp -= 4; M32(esp) = 0x10107Cu; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    ecx = (uint32_t)(eax);
    (void)sub32(eax, (uint32_t)0xFFFFFFFFu, &f);
    eax = (eax & 0xFFFFFF00u) | (uint8_t)(!f.zf);
    if (f.zf) goto L_1010C6;
    goto L_101086;
L_101086:
    TS32((uint32_t)(0x103000u), (uint32_t)(ecx));
    { uint32_t v_ = 0x46585452u; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = ecx; esp -= 4; M32(esp) = v_; }
    esi = (uint32_t)(edi);
    edi = (uint32_t)(ecx);
    { uint32_t t_ = TL32((uint32_t)(esi + 0x4u)); esp -= 4; M32(esp) = 0x101099u; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0x8u, &f));
    { uint32_t v_ = 0x1010D0u; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = edi; esp -= 4; M32(esp) = v_; }
    { uint32_t t_ = TL32((uint32_t)(esi + 0x8u)); esp -= 4; M32(esp) = 0x1010A5u; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0x8u, &f));
    { uint32_t v_ = 0x1010E0u; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = edi; esp -= 4; M32(esp) = v_; }
    { uint32_t t_ = TL32((uint32_t)(esi + 0xCu)); esp -= 4; M32(esp) = 0x1010B1u; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0x8u, &f));
    { uint32_t v_ = 0x101120u; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = edi; esp -= 4; M32(esp) = v_; }
    { uint32_t t_ = TL32((uint32_t)(esi + 0x10u)); esp -= 4; M32(esp) = 0x1010BDu; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0x8u, &f));
    eax = (eax & 0xFFFFFF00u) | (uint8_t)((uint8_t)0x1u);
    goto L_1010C6;
L_1010C4:
    eax = (uint32_t)(logic32(0, &f));
L_1010C6:
    { uint32_t v_ = M32(esp); esp += 4; esi = (uint32_t)(v_); }
    { uint32_t v_ = M32(esp); esp += 4; edi = (uint32_t)(v_); }
    esp += 4; SPILL(); return;
}

void skyrim_serial_i386_revert(CPU *c) {
    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Flags f = c->flags;
    uint32_t jt_ = 0;
    (void)f; (void)jt_;
L_1010D0:
    TS32((uint32_t)(0x103000u), (uint32_t)((uint32_t)0x0u));
    esp += 4; SPILL(); return;
}

void skyrim_serial_i386_save(CPU *c) {
    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Flags f = c->flags;
    uint32_t jt_ = 0;
    (void)f; (void)jt_;
L_1010E0:
    { uint32_t v_ = eax; esp -= 4; M32(esp) = v_; }
    eax = (uint32_t)(M32((uint32_t)(esp + 0x8u)));
    ecx = (uint32_t)(TL32((uint32_t)(0x103000u)));
    ecx = (uint32_t)(add32(ecx, (uint32_t)0x5u, &f));
    M32((uint32_t)(esp)) = (uint32_t)(ecx);
    ecx = (uint32_t)(esp);
    { uint32_t v_ = 0x4u; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = ecx; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = 0x1u; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = 0x56414C55u; esp -= 4; M32(esp) = v_; }
    { uint32_t t_ = TL32((uint32_t)(eax + 0x18u)); esp -= 4; M32(esp) = 0x101100u; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0x10u, &f));
    (void)logic8((uint8_t)(((uint8_t)eax) & ((uint8_t)eax)), &f);
    if (f.zf) goto L_10110F;
    goto L_101107;
L_101107:
    eax = (uint32_t)(M32((uint32_t)(esp)));
    TS32((uint32_t)(0x103000u), (uint32_t)(eax));
L_10110F:
    { uint32_t v_ = M32(esp); esp += 4; eax = (uint32_t)(v_); }
    esp += 4; SPILL(); return;
}

void skyrim_serial_i386_restore(CPU *c) {
    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    Flags f = c->flags;
    uint32_t jt_ = 0;
    (void)f; (void)jt_;
L_101120:
    { uint32_t v_ = ebp; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = ebx; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = edi; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = esi; esp -= 4; M32(esp) = v_; }
    esp = (uint32_t)(sub32(esp, (uint32_t)0x10u, &f));
    esi = (uint32_t)(M32((uint32_t)(esp + 0x24u)));
    edi = (uint32_t)((uint32_t)(esp + 0x4u));
    ebx = (uint32_t)((uint32_t)(esp + 0x8u));
    ebp = (uint32_t)((uint32_t)(esp + 0xCu));
L_101140:
    { uint32_t v_ = edi; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = ebx; esp -= 4; M32(esp) = v_; }
    { uint32_t v_ = ebp; esp -= 4; M32(esp) = v_; }
    { uint32_t t_ = TL32((uint32_t)(esi + 0x24u)); esp -= 4; M32(esp) = 0x101146u; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0xCu, &f));
    (void)logic8((uint8_t)(((uint8_t)eax) & ((uint8_t)eax)), &f);
    if (f.zf) goto L_101181;
    goto L_10114D;
L_10114D:
    (void)sub32(M32((uint32_t)(esp + 0xCu)), (uint32_t)0x56414C55u, &f);
    if (!f.zf) goto L_101140;
    goto L_101157;
L_101157:
    (void)sub32(M32((uint32_t)(esp + 0x8u)), (uint32_t)0x1u, &f);
    if (!f.zf) goto L_101140;
    goto L_10115E;
L_10115E:
    (void)sub32(M32((uint32_t)(esp + 0x4u)), (uint32_t)0x4u, &f);
    if (!f.zf) goto L_101140;
    goto L_101165;
L_101165:
    { uint32_t v_ = 0x4u; esp -= 4; M32(esp) = v_; }
    eax = (uint32_t)((uint32_t)(esp + 0x4u));
    { uint32_t v_ = eax; esp -= 4; M32(esp) = v_; }
    { uint32_t t_ = TL32((uint32_t)(esi + 0x28u)); esp -= 4; M32(esp) = 0x10116Fu; SPILL(); { static const FnEntry *ic_; const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); else rt_call_ic(c, t_, &ic_); } RELOAD(); }
    esp = (uint32_t)(add32(esp, (uint32_t)0x8u, &f));
    (void)sub32(eax, (uint32_t)0x4u, &f);
    if (!f.zf) goto L_101140;
    goto L_101177;
L_101177:
    eax = (uint32_t)(M32((uint32_t)(esp)));
    TS32((uint32_t)(0x103000u), (uint32_t)(eax));
    goto L_101140;
L_101181:
    esp = (uint32_t)(add32(esp, (uint32_t)0x10u, &f));
    { uint32_t v_ = M32(esp); esp += 4; esi = (uint32_t)(v_); }
    { uint32_t v_ = M32(esp); esp += 4; edi = (uint32_t)(v_); }
    { uint32_t v_ = M32(esp); esp += 4; ebx = (uint32_t)(v_); }
    { uint32_t v_ = M32(esp); esp += 4; ebp = (uint32_t)(v_); }
    esp += 4; SPILL(); return;
}


#include "funcs_serial.h"
#include "rt_table.h"
const FnEntry g_fn_table_serial[] = {
    {0x101000u, skyrim_serial_i386_query},
    {0x101050u, skyrim_serial_i386_load},
    {0x1010D0u, skyrim_serial_i386_revert},
    {0x1010E0u, skyrim_serial_i386_save},
    {0x101120u, skyrim_serial_i386_restore},
};
const unsigned g_fn_count_serial = 5;
