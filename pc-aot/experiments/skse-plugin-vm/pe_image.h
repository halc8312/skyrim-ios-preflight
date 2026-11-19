/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_PE_IMAGE_H
#define SKYRIM_EXPERIMENT_SKSE_PE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SKSE_PE_READ = 1, SKSE_PE_WRITE = 2, SKSE_PE_EXEC = 4 };
enum { SKSE_PE_MAX_FILE_BYTES = 64 * 1024 * 1024 };
enum { SKSE_PE_MAX_SECTIONS = 96, SKSE_PE_MAX_IMPORT_LIBRARIES = 32,
       SKSE_PE_MAX_IMPORT_SYMBOLS = 256, SKSE_PE_MAX_IMPORT_NAME = 128,
       SKSE_PE_MAX_TLS_CALLBACKS = 64 };

typedef enum {
    SKSE_PE_OK = 0, SKSE_PE_BAD_ARGUMENT, SKSE_PE_TRUNCATED,
    SKSE_PE_BAD_FORMAT, SKSE_PE_WRONG_MACHINE, SKSE_PE_SIZE_LIMIT,
    SKSE_PE_UNSUPPORTED_LAYOUT, SKSE_PE_UNSUPPORTED_WX,
    SKSE_PE_UNSUPPORTED_IMPORTS, SKSE_PE_UNSUPPORTED_TLS,
    SKSE_PE_UNSUPPORTED_DELAY_IMPORTS, SKSE_PE_UNSUPPORTED_CLR,
    SKSE_PE_UNSUPPORTED_DIRECTORY, SKSE_PE_UNSUPPORTED_ENTRY,
    SKSE_PE_FORWARDED_EXPORT, SKSE_PE_UNSUPPORTED_CALLING_CONVENTION,
    SKSE_PE_MISSING_EXPORTS, SKSE_PE_NO_MEMORY, SKSE_PE_UNAPPROVED_IMPORT,
    SKSE_PE_ORDINAL_IMPORT, SKSE_PE_BOUND_IMPORT, SKSE_PE_RELOCATIONS_REQUIRED,
    SKSE_PE_UNSUPPORTED_RELOCATION, SKSE_PE_BAD_RELOCATION,
    SKSE_PE_OVERLAPPING_RELOCATION, SKSE_PE_RELOCATION_IAT_CONFLICT,
    SKSE_PE_MISSING_DEPENDENCY_EXPORT, SKSE_PE_FORWARDED_DEPENDENCY_EXPORT, SKSE_PE_BAD_TLS
} SksePeStatus;

typedef struct {
    uint32_t rva, length;
    unsigned permissions;
} SksePeSection;

typedef struct {
    uint32_t base, size, query_va, load_va, entry_va;
    uint8_t *bytes; /* Owned, ordinary data storage. Never executable host memory. */
    uint32_t headers_size;
    uint16_t section_count;
    SksePeSection sections[SKSE_PE_MAX_SECTIONS];
} SksePeImage;

typedef struct {
    uint16_t library_index, ordinal;
    uint8_t by_ordinal;
    char name[SKSE_PE_MAX_IMPORT_NAME];
} SksePeImportSymbol;

typedef struct {
    uint32_t directory_mask; /* Bit n denotes nonempty PE data directory n. */
    uint32_t entry_rva;
    unsigned has_writable_code;
    uint16_t library_count, symbol_count;
    char libraries[SKSE_PE_MAX_IMPORT_LIBRARIES][SKSE_PE_MAX_IMPORT_NAME];
    SksePeImportSymbol symbols[SKSE_PE_MAX_IMPORT_SYMBOLS];
    uint32_t preferred_base, image_size;
} SksePeReport;

typedef struct {
    const char *library, *symbol;
    uint32_t service_token; /* A service token or approved export address in the interpreter. */
} SksePeImportBinding;

typedef struct {
    const char *name;
    uint32_t address;
} SksePeRequiredExport;

typedef struct {
    uint32_t present, template_rva, template_bytes, zero_fill, alignment, index_rva;
    uint32_t callback_count, callback_rvas[SKSE_PE_MAX_TLS_CALLBACKS];
} SksePeTlsInfo;

enum { SKSE_PE_ALLOW_ENTRY = 1u };

/* Inspect a file without allocating its image. The manifest describes regular
 * imports, including when the result is SKSE_PE_UNSUPPORTED_IMPORTS. Partial
 * reports on malformed input are for diagnostics only. */
SksePeStatus skse_pe_inspect(const uint8_t *file, size_t length, SksePeReport *report);
/* Identify the two Classic plugin entry points without mapping or resolving
 * imports. Success only describes the exports and their executable ranges. */
SksePeStatus skse_pe_classic_exports(const uint8_t *file, size_t length);
/* Map a preferred-base, import-free i386 PE32 DLL with two cdecl exports and
 * no loader entry point. Only initialized RX/X bytes may supply instructions.
 * Does not resolve imports, apply relocations, run code or change host memory
 * permissions. File and image limits are 64 and 32 MiB. Pass a fresh output,
 * or call skse_pe_free before reuse. Failure clears out. */
SksePeStatus skse_pe_map(const uint8_t *file, size_t length, SksePeImage *out);
/* Each named import must match a caller-owned binding, with ASCII-folded DLL
 * name and exact function name. The image is published only after validation
 * of the entire import table. A mapped entry_va is only returned with explicit
 * SKSE_PE_ALLOW_ENTRY; the caller is responsible for its emulated lifecycle. */
SksePeStatus skse_pe_map_with_imports(const uint8_t *file, size_t length,
                                      const SksePeImportBinding *bindings, size_t binding_count,
                                      unsigned flags, SksePeImage *out);
/* Explicit, 64 KiB-aligned guest address. Moving an image supports only
 * initialized HIGHLOW words pointing into the preferred image. Storage and
 * service tokens are never native addresses. The caller owns VM range layout. */
SksePeStatus skse_pe_map_at_base(const uint8_t *file, size_t length, uint32_t guest_base,
                                 const SksePeImportBinding *bindings, size_t binding_count,
                                 unsigned flags, SksePeImage *out);
/* Dependency DLLs expose only the requested, exact named functions. The
 * caller owns the source and decides which import names may refer to it.
 * Failure clears the output image and every requested address. */
SksePeStatus skse_pe_map_library(const uint8_t *file, size_t length, uint32_t guest_base,
                                 const SksePeImportBinding *bindings, size_t binding_count, unsigned flags,
                                 SksePeRequiredExport *required, size_t required_count, SksePeImage *out);
/* Opt in to reading static TLS metadata without assigning a slot, allocating
 * thread data, or dispatching callbacks. All returned addresses are guest RVAs.
 * Read template_bytes from out->bytes + template_rva; zero_fill follows the
 * template when a caller creates its own block. Alignment zero means unspecified.
 * guest_base zero selects the preferred base. A NULL/zero required list selects
 * Classic Query/Load. An explicit list has the same contract as map_library.
 * Both outputs and valid requested export slots are cleared on failure. */
SksePeStatus skse_pe_map_tls(const uint8_t *file, size_t length, uint32_t guest_base,
                             const SksePeImportBinding *bindings, size_t binding_count, unsigned flags,
                             SksePeRequiredExport *required, size_t required_count,
                             SksePeImage *out, SksePeTlsInfo *out_tls);
void skse_pe_free(SksePeImage *image);
/* Nonempty range must fit fully inside one region with every requested bit. */
int skse_pe_allows(const SksePeImage *image, uint32_t va, size_t length, unsigned required);
/* The returned region is clipped to the image allocation. Headers form a
 * read-only region. Passing NULL performs only the permission check. */
int skse_pe_region(const SksePeImage *image, uint32_t va, size_t length, unsigned required, SksePeSection *out);
const char *skse_pe_status_text(SksePeStatus status);
/* PE library names use ASCII case-insensitive comparison. */
int skse_pe_library_name_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
#endif
