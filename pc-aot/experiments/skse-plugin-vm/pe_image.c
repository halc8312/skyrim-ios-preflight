/* SPDX-License-Identifier: GPL-3.0-only */
#include "pe_image.h"

#include <stdlib.h>
#include <string.h>

/* Microsoft PE/COFF specification: https://learn.microsoft.com/windows/win32/debug/pe-format
 * Read little-endian fields from checked byte ranges, including on unaligned input. */
enum {
    MAX_IMAGE_BYTES = 32 * 1024 * 1024, MAX_EXPORTS = 4096,
    MAX_RELOCATIONS = 65536, MAX_RELOC_BLOCKS = MAX_IMAGE_BYTES / 4096,
    EXPORTS = 0, IMPORTS = 1, RESOURCES = 2, SECURITY = 4, RELOCS = 5,
    DEBUG_INFO = 6, TLS = 9, BOUND_IMPORTS = 11, IAT = 12, DELAY_IMPORTS = 13,
    CLR = 14, DIRECTORY_COUNT = 16
};

typedef struct {
    uint32_t address, length;
} PeSpan;

typedef struct {
    const uint8_t *file;
    size_t file_length;
    SksePeImage image;
    PeSpan raw[SKSE_PE_MAX_SECTIONS], dir[DIRECTORY_COUNT];
    uint32_t directory_mask, entry_rva, preferred_base;
    unsigned has_writable_code, relocs_stripped;
} PeFile;

typedef struct {
    const SksePeImportBinding *bindings;
    size_t binding_count;
    unsigned flags, slot_count, table_count;
    SksePeStatus refused;
    PeSpan tables[SKSE_PE_MAX_IMPORT_LIBRARIES];
    struct { uint32_t rva, token; } slots[SKSE_PE_MAX_IMPORT_SYMBOLS];
} ImportPlan;

typedef struct {
    uint32_t *rvas;
    unsigned count;
} RelocationWords;

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void store_u32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}

static int fits(uint64_t offset, uint64_t count, uint64_t total) {
    return offset <= total && count <= total - offset;
}

static int power_of_two(uint32_t value) {
    return value && !(value & (value - 1));
}

static int overlaps(PeSpan a, PeSpan b) {
    return a.length && b.length && (uint64_t)a.address < (uint64_t)b.address + b.length
        && (uint64_t)b.address < (uint64_t)a.address + a.length;
}

static const SksePeSection *section_at(const SksePeImage *image, uint32_t rva, size_t length) {
    for (unsigned i = 0; i < image->section_count; i++) {
        const SksePeSection *section = &image->sections[i];
        if (rva >= section->rva && fits(rva - section->rva, length, section->length))
            return section;
    }
    return NULL;
}

int skse_pe_allows(const SksePeImage *image, uint32_t va, size_t length, unsigned required) {
    if (!image || !image->bytes || !length || !required
        || (required & ~(unsigned)(SKSE_PE_READ | SKSE_PE_WRITE | SKSE_PE_EXEC)) || va < image->base)
        return 0;
    uint32_t rva = va - image->base;
    if (!fits(rva, length, image->size)) return 0;
    if (fits(rva, length, image->headers_size)) return required == SKSE_PE_READ;
    const SksePeSection *section = section_at(image, rva, length);
    return section && (section->permissions & required) == required;
}

/* Metadata must use initialized bytes in the source file. A zero-filled BSS
 * tail cannot supply an invented terminator, export table or first instruction. */
static const uint8_t *file_at_rva(const PeFile *pe, uint32_t rva, size_t length, size_t *available) {
    uint32_t offset = rva, extent = pe->image.headers_size;
    if (rva >= extent) {
        const SksePeSection *section = section_at(&pe->image, rva, 1);
        if (!section) return NULL;
        PeSpan raw = pe->raw[section - pe->image.sections];
        extent = raw.length < section->length ? raw.length : section->length;
        uint32_t within = rva - section->rva;
        if (within >= extent) return NULL;
        extent -= within;
        offset = raw.address + within;
    } else {
        extent -= rva;
    }
    if (length > extent) return NULL;
    if (available) *available = extent;
    return pe->file + offset;
}

static SksePeStatus name_at(const PeFile *pe, uint32_t rva, const char **name) {
    size_t available = 0;
    const uint8_t *start = file_at_rva(pe, rva, 1, &available);
    if (!start) return SKSE_PE_BAD_FORMAT;
    if (available > SKSE_PE_MAX_IMPORT_NAME) available = SKSE_PE_MAX_IMPORT_NAME;
    const uint8_t *end = memchr(start, 0, available);
    if (!end || end == start) return SKSE_PE_BAD_FORMAT;
    for (const uint8_t *p = start; p < end; p++)
        if (*p < 32 || *p > 126) return SKSE_PE_BAD_FORMAT;
    *name = (const char *)start;
    return SKSE_PE_OK;
}

static SksePeStatus read_sections(PeFile *pe, const uint8_t *table,
                                  uint32_t section_align, uint32_t file_align) {
    for (unsigned i = 0; i < pe->image.section_count; i++) {
        const uint8_t *s = table + i * 40;
        PeSpan raw = {u32(s + 20), u32(s + 16)};
        PeSpan virtual_range = {u32(s + 12), u32(s + 8)};
        if (!virtual_range.length) virtual_range.length = raw.length;
        if (!virtual_range.length || virtual_range.address < pe->image.headers_size
            || virtual_range.address % section_align
            || !fits(virtual_range.address, virtual_range.length, pe->image.size))
            return SKSE_PE_BAD_FORMAT;
        if (raw.length && (raw.address < pe->image.headers_size || raw.address % file_align
                           || raw.length % file_align || !fits(raw.address, raw.length, pe->file_length)))
            return SKSE_PE_BAD_FORMAT;
        for (unsigned j = 0; j < i; j++) {
            const SksePeSection *earlier = &pe->image.sections[j];
            PeSpan older_virtual = {earlier->rva, earlier->length};
            if (overlaps(virtual_range, older_virtual) || overlaps(raw, pe->raw[j]))
                return SKSE_PE_BAD_FORMAT;
        }
        uint32_t characteristics = u32(s + 36);
        unsigned perms = 0;
        if (characteristics & 0x40000000u) perms |= SKSE_PE_READ;
        if (characteristics & 0x80000000u) perms |= SKSE_PE_WRITE;
        if (characteristics & 0x20000000u) perms |= SKSE_PE_EXEC;
        if ((perms & (SKSE_PE_WRITE | SKSE_PE_EXEC)) == (SKSE_PE_WRITE | SKSE_PE_EXEC))
            pe->has_writable_code = 1;
        if ((perms & SKSE_PE_EXEC) && virtual_range.length > raw.length)
            return SKSE_PE_UNSUPPORTED_LAYOUT;
        pe->image.sections[i] = (SksePeSection){virtual_range.address, virtual_range.length, perms};
        pe->raw[i] = raw;
    }
    return SKSE_PE_OK;
}

static SksePeStatus read_headers(PeFile *pe) {
    if (pe->file_length > SKSE_PE_MAX_FILE_BYTES) return SKSE_PE_SIZE_LIMIT;
    if (pe->file_length < 64) return SKSE_PE_TRUNCATED;
    if (memcmp(pe->file, "MZ", 2)) return SKSE_PE_BAD_FORMAT;
    uint32_t nt_offset = u32(pe->file + 60);
    if (nt_offset < 64) return SKSE_PE_BAD_FORMAT;
    if (!fits(nt_offset, 24, pe->file_length)) return SKSE_PE_TRUNCATED;
    const uint8_t *nt = pe->file + nt_offset;
    if (memcmp(nt, "PE\0\0", 4)) return SKSE_PE_BAD_FORMAT;
    if (u16(nt + 4) != 0x014c) return SKSE_PE_WRONG_MACHINE;
    if ((u16(nt + 22) & 0x2002) != 0x2002) return SKSE_PE_BAD_FORMAT;
    pe->relocs_stripped = u16(nt + 22) & 1u;
    pe->image.section_count = u16(nt + 6);
    if (!pe->image.section_count || pe->image.section_count > SKSE_PE_MAX_SECTIONS)
        return SKSE_PE_SIZE_LIMIT;
    uint16_t opt_size = u16(nt + 20);
    uint64_t opt_offset = (uint64_t)nt_offset + 24;
    if (!fits(opt_offset, opt_size, pe->file_length)) return SKSE_PE_TRUNCATED;
    if (opt_size < 96) return SKSE_PE_BAD_FORMAT;
    const uint8_t *opt = nt + 24;
    if (u16(opt) != 0x010b) return SKSE_PE_WRONG_MACHINE;
    pe->preferred_base = pe->image.base = u32(opt + 28);
    pe->image.size = u32(opt + 56);
    pe->image.headers_size = u32(opt + 60);
    pe->entry_rva = u32(opt + 16);
    uint32_t section_align = u32(opt + 32), file_align = u32(opt + 36);
    if (!pe->image.base || (pe->image.base & 0xffff) || section_align < 4096
        || file_align < 512 || file_align > 65536 || section_align < file_align
        || !power_of_two(section_align) || !power_of_two(file_align))
        return SKSE_PE_UNSUPPORTED_LAYOUT;
    if (!pe->image.size || pe->image.size > MAX_IMAGE_BYTES) return SKSE_PE_SIZE_LIMIT;
    if ((uint64_t)pe->image.base + pe->image.size > (UINT64_C(1) << 32)
        || pe->image.size % section_align || pe->image.headers_size % file_align
        || pe->image.headers_size > pe->image.size)
        return SKSE_PE_BAD_FORMAT;
    if (pe->image.headers_size > pe->file_length) return SKSE_PE_TRUNCATED;
    uint32_t dirs = u32(opt + 92);
    if (dirs > DIRECTORY_COUNT) return SKSE_PE_UNSUPPORTED_LAYOUT;
    if (!fits(96, (uint64_t)dirs * 8, opt_size)) return SKSE_PE_BAD_FORMAT;
    uint64_t table_offset = opt_offset + opt_size;
    if (!fits(table_offset, (uint64_t)pe->image.section_count * 40, pe->image.headers_size))
        return SKSE_PE_BAD_FORMAT;
    SksePeStatus status = read_sections(pe, pe->file + table_offset, section_align, file_align);
    if (status != SKSE_PE_OK) return status;
    for (unsigned i = 0; i < dirs; i++) {
        PeSpan dir = {u32(opt + 96 + i * 8), u32(opt + 100 + i * 8)};
        if (!dir.address && !dir.length) continue;
        if (!dir.address || !dir.length) return SKSE_PE_BAD_FORMAT;
        if (i == SECURITY) { /* The certificate table uses file offsets. */
            if (!fits(dir.address, dir.length, pe->file_length)) return SKSE_PE_BAD_FORMAT;
        } else if (!file_at_rva(pe, dir.address, dir.length, NULL)) {
            return SKSE_PE_BAD_FORMAT;
        }
        pe->directory_mask |= 1u << i;
        pe->dir[i] = dir;
    }
    if (pe->entry_rva) {
        const SksePeSection *s = section_at(&pe->image, pe->entry_rva, 1);
        if (!s || !(s->permissions & SKSE_PE_EXEC) || !file_at_rva(pe, pe->entry_rva, 1, NULL))
            return SKSE_PE_BAD_FORMAT;
        pe->image.entry_va = pe->image.base + pe->entry_rva;
    }
    return SKSE_PE_OK;
}

static const uint8_t *thunk_word(const PeFile *pe, uint32_t rva, unsigned index) {
    uint64_t word = (uint64_t)rva + (uint64_t)index * 4;
    if (word > UINT32_MAX) return NULL;
    return file_at_rva(pe, (uint32_t)word, 4, NULL);
}

static unsigned lower_ascii(unsigned ch) {
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static int same_library(const char *a, const char *b) {
    while (*a && lower_ascii((uint8_t)*a) == lower_ascii((uint8_t)*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static int binding_name_valid(const char *name) {
    if (!name || !*name) return 0;
    for (size_t i = 0; i < SKSE_PE_MAX_IMPORT_NAME; i++) {
        unsigned ch = (uint8_t)name[i];
        if (!ch) return 1;
        if (ch < 32 || ch > 126) return 0;
    }
    return 0;
}

static SksePeStatus validate_bindings(const PeFile *pe, const ImportPlan *plan) {
    if ((!plan->bindings && plan->binding_count) || plan->binding_count > SKSE_PE_MAX_IMPORT_SYMBOLS
        || (plan->flags & ~(unsigned)SKSE_PE_ALLOW_ENTRY))
        return SKSE_PE_BAD_ARGUMENT;
    for (size_t i = 0; i < plan->binding_count; i++) {
        const SksePeImportBinding *b = &plan->bindings[i];
        if (!binding_name_valid(b->library) || !binding_name_valid(b->symbol) || !b->service_token
            || (b->service_token >= pe->image.base && b->service_token - pe->image.base < pe->image.size))
            return SKSE_PE_BAD_ARGUMENT;
        for (size_t j = 0; j < i; j++) {
            const SksePeImportBinding *previous = &plan->bindings[j];
            if (same_library(b->library, previous->library) && !strcmp(b->symbol, previous->symbol))
                return SKSE_PE_BAD_ARGUMENT;
        }
    }
    return SKSE_PE_OK;
}

static void refuse_import(ImportPlan *plan, SksePeStatus reason) {
    if (plan->refused == SKSE_PE_OK) plan->refused = reason;
}

static uint32_t service_token(const ImportPlan *plan, const char *library, const char *name) {
    for (size_t i = 0; i < plan->binding_count; i++) {
        const SksePeImportBinding *b = &plan->bindings[i];
        if (same_library(library, b->library) && !strcmp(name, b->symbol)) return b->service_token;
    }
    return 0;
}

static SksePeStatus check_iat_region(const PeFile *pe) {
    PeSpan iat = pe->dir[IAT];
    if (!iat.address) return SKSE_PE_UNSUPPORTED_IMPORTS;
    const SksePeSection *s = section_at(&pe->image, iat.address, iat.length);
    if ((iat.address | iat.length) % 4 || !s || !(s->permissions & SKSE_PE_READ)
        || (s->permissions & SKSE_PE_EXEC))
        return SKSE_PE_BAD_FORMAT;
    for (unsigned i = 0; i < DIRECTORY_COUNT; i++) {
        /* An import directory can describe an entire section. Its actual
         * descriptors, names and lookup arrays are checked separately. */
        if (i != IMPORTS && i != IAT && i != SECURITY && overlaps(iat, pe->dir[i]))
            return SKSE_PE_BAD_FORMAT;
    }
    return SKSE_PE_OK;
}

static SksePeStatus finish_import_table(const PeFile *pe, ImportPlan *plan, uint32_t lookup,
                                        uint32_t addresses, unsigned words) {
    PeSpan whole_iat = pe->dir[IAT];
    PeSpan target = {addresses, (words + 1) * 4};
    PeSpan source = {lookup, target.length};
    if ((addresses | lookup) % 4 || addresses < whole_iat.address
        || !fits(addresses - whole_iat.address, target.length, whole_iat.length)
        || !file_at_rva(pe, lookup, source.length, NULL)
        || (lookup != addresses && overlaps(source, whole_iat)))
        return SKSE_PE_BAD_FORMAT;
    for (unsigned i = 0; i < plan->table_count; i++)
        if (overlaps(target, plan->tables[i])) return SKSE_PE_BAD_FORMAT;
    plan->tables[plan->table_count++] = target;
    if (!words) refuse_import(plan, SKSE_PE_UNAPPROVED_IMPORT);
    return SKSE_PE_OK;
}

static SksePeStatus read_imports(const PeFile *pe, SksePeReport *report, ImportPlan *plan) {
    PeSpan dir = pe->dir[IMPORTS];
    if (!dir.address) return SKSE_PE_OK;
    if (plan) {
        SksePeStatus status = check_iat_region(pe);
        if (status != SKSE_PE_OK) return status;
    }
    const uint8_t *descs = file_at_rva(pe, dir.address, dir.length, NULL);
    unsigned libraries = 0, symbols = 0;
    for (size_t offset = 0; fits(offset, 20, dir.length); offset += 20) {
        const uint8_t *d = descs + offset;
        uint32_t lookup = u32(d), names = u32(d + 12), addresses = u32(d + 16);
        if (!lookup && !u32(d + 4) && !u32(d + 8) && !names && !addresses) {
            if (plan) {
                PeSpan descriptors = {dir.address, (uint32_t)offset + 20};
                if (overlaps(descriptors, pe->dir[IAT]) || !libraries
                    || (symbols + libraries) * 4 != pe->dir[IAT].length)
                    return SKSE_PE_BAD_FORMAT;
            }
            return SKSE_PE_OK;
        }
        if (libraries >= SKSE_PE_MAX_IMPORT_LIBRARIES) return SKSE_PE_SIZE_LIMIT;
        if (!names || !addresses) return SKSE_PE_BAD_FORMAT;
        const char *library = NULL;
        SksePeStatus status = name_at(pe, names, &library);
        if (status != SKSE_PE_OK) return status;
        if (plan) {
            PeSpan dll_name = {names, (uint32_t)strlen(library) + 1};
            if (overlaps(dll_name, pe->dir[IAT])) return SKSE_PE_BAD_FORMAT;
            if (u32(d + 4) || (u32(d + 8) && u32(d + 8) != UINT32_MAX))
                refuse_import(plan, SKSE_PE_BOUND_IMPORT);
        }
        if (report) {
            memcpy(report->libraries[libraries], library, strlen(library) + 1);
            report->library_count = (uint16_t)(libraries + 1);
        }
        if (!lookup) lookup = addresses;
        for (unsigned i = 0; ; i++) {
            const uint8_t *word = thunk_word(pe, lookup, i);
            const uint8_t *slot = thunk_word(pe, addresses, i);
            if (!word || !slot) return SKSE_PE_BAD_FORMAT;
            uint32_t value = u32(word);
            if (plan && value != u32(slot)) refuse_import(plan, SKSE_PE_BOUND_IMPORT);
            if (!value) {
                if (plan) {
                    status = finish_import_table(pe, plan, lookup, addresses, i);
                    if (status != SKSE_PE_OK) return status;
                }
                break;
            }
            if (symbols >= SKSE_PE_MAX_IMPORT_SYMBOLS) return SKSE_PE_SIZE_LIMIT;
            int by_ordinal = (value & 0x80000000u) != 0;
            const char *name = NULL;
            if (by_ordinal) {
                if (value & 0x7fff0000u) return SKSE_PE_BAD_FORMAT;
            } else {
                if (!file_at_rva(pe, value, 3, NULL)) return SKSE_PE_BAD_FORMAT;
                status = name_at(pe, value + 2, &name);
                if (status != SKSE_PE_OK) return status;
            }
            if (plan) {
                uint32_t token = 0;
                if (by_ordinal) {
                    refuse_import(plan, SKSE_PE_ORDINAL_IMPORT);
                } else {
                    PeSpan hint_and_name = {value, 2 + (uint32_t)strlen(name) + 1};
                    if (overlaps(hint_and_name, pe->dir[IAT])) return SKSE_PE_BAD_FORMAT;
                    token = service_token(plan, library, name);
                    if (!token) refuse_import(plan, SKSE_PE_UNAPPROVED_IMPORT);
                }
                plan->slots[symbols].rva = addresses + 4 * i; /* thunk_word checked this range. */
                plan->slots[symbols].token = token;
                plan->slot_count++;
            }
            if (report) {
                SksePeImportSymbol *symbol = &report->symbols[symbols];
                symbol->library_index = (uint16_t)libraries;
                symbol->by_ordinal = (uint8_t)by_ordinal;
                if (by_ordinal) symbol->ordinal = (uint16_t)value;
                else memcpy(symbol->name, name, strlen(name) + 1);
                report->symbol_count = (uint16_t)(symbols + 1);
            }
            symbols++;
        }
        libraries++;
    }
    return SKSE_PE_BAD_FORMAT; /* No complete null descriptor in the directory. */
}

static int compare_rva(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static SksePeStatus collect_relocation(const PeFile *pe, RelocationWords *move, uint32_t rva) {
    PeSpan word = {rva, 4};
    if (overlaps(word, pe->dir[IAT])) return SKSE_PE_RELOCATION_IAT_CONFLICT;
    if (overlaps(word, pe->dir[RELOCS]) || overlaps(word, pe->dir[EXPORTS]) || overlaps(word, pe->dir[IMPORTS]))
        return SKSE_PE_BAD_RELOCATION;
    const uint8_t *stored = file_at_rva(pe, rva, 4, NULL);
    if (!stored) return SKSE_PE_BAD_RELOCATION;
    uint32_t address = u32(stored);
    if (address < pe->preferred_base || address - pe->preferred_base >= pe->image.size)
        return SKSE_PE_BAD_RELOCATION;
    move->rvas[move->count++] = rva;
    return SKSE_PE_OK;
}

static SksePeStatus check_relocations(const PeFile *pe, RelocationWords *move) {
    PeSpan dir = pe->dir[RELOCS];
    if (move && (!dir.address || pe->relocs_stripped)) return SKSE_PE_RELOCATIONS_REQUIRED;
    if (!dir.address) return SKSE_PE_OK;
    if (move) {
        if (dir.length < 8) return SKSE_PE_BAD_RELOCATION;
        uint32_t capacity = dir.length / 2;
        if (capacity > MAX_RELOCATIONS) capacity = MAX_RELOCATIONS;
        move->rvas = malloc(capacity * sizeof(*move->rvas));
        if (!move->rvas) return SKSE_PE_NO_MEMORY;
    }
    const uint8_t *data = file_at_rva(pe, dir.address, dir.length, NULL);
    unsigned blocks = 0, items = 0;
    for (size_t offset = 0; offset < dir.length;) {
        if (!fits(offset, 8, dir.length)) return SKSE_PE_BAD_FORMAT;
        uint32_t page = u32(data + offset), size = u32(data + offset + 4);
        if (page % 4096 || page >= pe->image.size || size < 8 || size % 4
            || !fits(offset, size, dir.length))
            return SKSE_PE_BAD_FORMAT;
        if (++blocks > MAX_RELOC_BLOCKS || (size - 8) / 2 > MAX_RELOCATIONS - items)
            return SKSE_PE_SIZE_LIMIT;
        items += (size - 8) / 2;
        for (uint32_t i = 8; i < size; i += 2) {
            uint16_t item = u16(data + offset + i);
            unsigned type = item >> 12;
            if (!type) continue; /* ABSOLUTE pads a block. */
            if (type != 3) return move ? SKSE_PE_UNSUPPORTED_RELOCATION : SKSE_PE_UNSUPPORTED_LAYOUT;
            uint32_t rva = page + (item & 4095);
            if (!section_at(&pe->image, rva, 4))
                return move ? SKSE_PE_BAD_RELOCATION : SKSE_PE_BAD_FORMAT;
            if (move) {
                SksePeStatus status = collect_relocation(pe, move, rva);
                if (status != SKSE_PE_OK) return status;
            }
        }
        offset += size;
    }
    if (move) {
        if (!move->count) return SKSE_PE_RELOCATIONS_REQUIRED;
        qsort(move->rvas, move->count, sizeof(*move->rvas), compare_rva);
        for (unsigned i = 1; i < move->count; i++)
            if (move->rvas[i] - move->rvas[i - 1] < 4) return SKSE_PE_OVERLAPPING_RELOCATION;
    }
    return SKSE_PE_OK;
}

static SksePeStatus read_exports(PeFile *pe) {
    PeSpan dir = pe->dir[EXPORTS];
    if (!dir.address) return SKSE_PE_MISSING_EXPORTS;
    if (dir.length < 40) return SKSE_PE_BAD_FORMAT;
    const uint8_t *d = file_at_rva(pe, dir.address, 40, NULL);
    uint32_t nf = u32(d + 20), nn = u32(d + 24);
    if (nf > MAX_EXPORTS || nn > MAX_EXPORTS) return SKSE_PE_SIZE_LIMIT;
    if (!nf || !nn) return SKSE_PE_MISSING_EXPORTS;
    if (!u32(d + 28) || !u32(d + 32) || !u32(d + 36)) return SKSE_PE_BAD_FORMAT;
    const uint8_t *funcs = file_at_rva(pe, u32(d + 28), nf * 4, NULL);
    const uint8_t *names = file_at_rva(pe, u32(d + 32), nn * 4, NULL);
    const uint8_t *ords = file_at_rva(pe, u32(d + 36), nn * 2, NULL);
    if (!funcs || !names || !ords) return SKSE_PE_BAD_FORMAT;
    for (uint32_t i = 0; i < nn; i++) {
        uint32_t ordinal = u16(ords + i * 2);
        if (ordinal >= nf || !u32(names + i * 4)) return SKSE_PE_BAD_FORMAT;
        const char *name = NULL;
        SksePeStatus status = name_at(pe, u32(names + i * 4), &name);
        if (status != SKSE_PE_OK) return status;
        if (*name == '_') name++; /* Optional x86 cdecl symbol underscore. */
        uint32_t *target = NULL;
        if (!strcmp(name, "SKSEPlugin_Query")) target = &pe->image.query_va;
        else if (!strcmp(name, "SKSEPlugin_Load")) target = &pe->image.load_va;
        else if (!strncmp(name, "SKSEPlugin_Query@", sizeof("SKSEPlugin_Query@") - 1)
                 || !strncmp(name, "SKSEPlugin_Load@", sizeof("SKSEPlugin_Load@") - 1))
            return SKSE_PE_UNSUPPORTED_CALLING_CONVENTION;
        if (!target) continue;
        uint32_t rva = u32(funcs + ordinal * 4);
        if (*target || !rva) return SKSE_PE_BAD_FORMAT;
        if (rva >= dir.address && rva - dir.address < dir.length)
            return SKSE_PE_FORWARDED_EXPORT;
        const SksePeSection *section = section_at(&pe->image, rva, 1);
        if (!section || !(section->permissions & SKSE_PE_EXEC) || !file_at_rva(pe, rva, 1, NULL))
            return SKSE_PE_BAD_FORMAT;
        *target = pe->image.base + rva;
    }
    return pe->image.query_va && pe->image.load_va ? SKSE_PE_OK : SKSE_PE_MISSING_EXPORTS;
}

static SksePeStatus parse(PeFile *pe, SksePeReport *report, ImportPlan *plan,
                          uint32_t guest_base, RelocationWords *move) {
    SksePeStatus status = read_headers(pe);
    if (status != SKSE_PE_OK) return status;
    if (guest_base) {
        if ((guest_base & 0xffffu) || (uint64_t)guest_base + pe->image.size > (UINT64_C(1) << 32))
            return SKSE_PE_BAD_ARGUMENT;
        pe->image.base = guest_base;
        if (pe->entry_rva) pe->image.entry_va = guest_base + pe->entry_rva;
    }
    if (plan) {
        status = validate_bindings(pe, plan);
        if (status != SKSE_PE_OK) return status;
    }
    if (report) {
        report->directory_mask = pe->directory_mask;
        report->entry_rva = pe->entry_rva;
        report->has_writable_code = pe->has_writable_code;
    }
    status = read_imports(pe, report, plan);
    if (status != SKSE_PE_OK) return status;
    if (pe->has_writable_code) return SKSE_PE_UNSUPPORTED_WX;
    if (pe->dir[TLS].address) return SKSE_PE_UNSUPPORTED_TLS;
    if (pe->dir[DELAY_IMPORTS].address) return SKSE_PE_UNSUPPORTED_DELAY_IMPORTS;
    if (pe->dir[CLR].address) return SKSE_PE_UNSUPPORTED_CLR;
    uint32_t supported = (1u << EXPORTS) | (1u << RELOCS) | (1u << RESOURCES) | (1u << DEBUG_INFO);
    if (!plan) {
        if (pe->dir[IMPORTS].address || pe->dir[BOUND_IMPORTS].address || pe->dir[IAT].address)
            return SKSE_PE_UNSUPPORTED_IMPORTS;
    } else {
        if (pe->dir[BOUND_IMPORTS].address) return SKSE_PE_BOUND_IMPORT;
        if (pe->dir[IAT].address && !pe->dir[IMPORTS].address) return SKSE_PE_BAD_FORMAT;
        if (plan->refused != SKSE_PE_OK) return plan->refused;
        supported |= (1u << IMPORTS) | (1u << IAT);
    }
    if (pe->directory_mask & ~supported) return SKSE_PE_UNSUPPORTED_DIRECTORY;
    if (pe->entry_rva && (!plan || !(plan->flags & SKSE_PE_ALLOW_ENTRY)))
        return SKSE_PE_UNSUPPORTED_ENTRY;
    status = check_relocations(pe, pe->image.base == pe->preferred_base ? NULL : move);
    return status == SKSE_PE_OK ? read_exports(pe) : status;
}

SksePeStatus skse_pe_inspect(const uint8_t *file, size_t length, SksePeReport *report) {
    if (report) memset(report, 0, sizeof(*report));
    if (!file || !report) return SKSE_PE_BAD_ARGUMENT;
    PeFile pe = {.file = file, .file_length = length};
    return parse(&pe, report, NULL, 0, NULL);
}

static SksePeStatus copy_image(PeFile *pe, const ImportPlan *plan, const RelocationWords *move, SksePeImage *out) {
    pe->image.bytes = calloc(1, pe->image.size);
    if (!pe->image.bytes) return SKSE_PE_NO_MEMORY;
    memcpy(pe->image.bytes, pe->file, pe->image.headers_size);
    for (unsigned i = 0; i < pe->image.section_count; i++) {
        SksePeSection s = pe->image.sections[i];
        PeSpan raw = pe->raw[i];
        uint32_t count = s.length < raw.length ? s.length : raw.length;
        if (count) memcpy(pe->image.bytes + s.rva, pe->file + raw.address, count);
    }
    for (unsigned i = 0; i < move->count; i++) {
        uint8_t *word = pe->image.bytes + move->rvas[i];
        store_u32(word, pe->image.base + (u32(word) - pe->preferred_base));
    }
    if (plan) {
        for (unsigned i = 0; i < plan->slot_count; i++) {
            uint8_t *slot = pe->image.bytes + plan->slots[i].rva;
            store_u32(slot, plan->slots[i].token);
        }
    }
    *out = pe->image;
    return SKSE_PE_OK;
}

static SksePeStatus map_image(const uint8_t *file, size_t length, uint32_t guest_base,
                              ImportPlan *plan, SksePeImage *out) {
    if (!out) return SKSE_PE_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!file) return SKSE_PE_BAD_ARGUMENT;
    PeFile pe = {.file = file, .file_length = length};
    RelocationWords move = {0};
    SksePeStatus status = parse(&pe, NULL, plan, guest_base, &move);
    if (status == SKSE_PE_OK) status = copy_image(&pe, plan, &move, out);
    free(move.rvas);
    return status;
}

SksePeStatus skse_pe_map(const uint8_t *file, size_t length, SksePeImage *out) {
    return map_image(file, length, 0, NULL, out);
}

SksePeStatus skse_pe_map_with_imports(const uint8_t *file, size_t length,
                                      const SksePeImportBinding *bindings, size_t binding_count,
                                      unsigned flags, SksePeImage *out) {
    ImportPlan plan = {.bindings = bindings, .binding_count = binding_count, .flags = flags};
    return map_image(file, length, 0, &plan, out);
}

SksePeStatus skse_pe_map_at_base(const uint8_t *file, size_t length, uint32_t guest_base,
                                 const SksePeImportBinding *bindings, size_t binding_count,
                                 unsigned flags, SksePeImage *out) {
    if (!guest_base) {
        if (out) memset(out, 0, sizeof(*out));
        return SKSE_PE_BAD_ARGUMENT;
    }
    ImportPlan plan = {.bindings = bindings, .binding_count = binding_count, .flags = flags};
    return map_image(file, length, guest_base, &plan, out);
}

void skse_pe_free(SksePeImage *image) {
    if (image) {
        free(image->bytes);
        memset(image, 0, sizeof(*image));
    }
}

const char *skse_pe_status_text(SksePeStatus status) {
    static const char *const names[] = {
        "ok", "bad argument", "truncated headers", "malformed PE", "requires PE32 i386",
        "parser limit exceeded", "unsupported PE layout", "writable executable section",
        "imports unsupported", "TLS unsupported", "delay imports unsupported", "CLR unsupported",
        "unsupported data directory", "DLL entry point unsupported", "forwarded SKSE export",
        "unsupported SKSE calling convention", "missing SKSE exports", "insufficient memory",
        "import absent from the service list", "ordinal imports unsupported", "bound imports unsupported",
        "relocations required at alternate base", "unsupported relocation type", "invalid image relocation",
        "overlapping relocation words", "relocation overlaps import address table"
    };
    unsigned index = (unsigned)status;
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown PE status";
}
