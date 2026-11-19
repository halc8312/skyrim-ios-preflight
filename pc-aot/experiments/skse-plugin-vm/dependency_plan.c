/* SPDX-License-Identifier: GPL-3.0-only */
#include "dependency_plan.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum { NEW, VISITING, FINISHED } Visit;
typedef struct {
    SkseTrialDependency source;
    SksePeReport manifest;
    SksePeRequiredExport required[SKSE_PE_MAX_IMPORT_SYMBOLS];
    unsigned required_count, consumers, attach_step;
    uint32_t base;
    Visit visit;
} Module;

typedef struct {
    Module modules[SKSE_DEPENDENCY_MAX_MODULES];
    unsigned count, ordered;
    size_t source_bytes, image_bytes;
    const SkseTrialDependency *catalog;
    size_t catalog_count;
    const SksePeImportBinding *services;
    size_t service_count;
    unsigned static_tls;
    SkseDependencyTrace *trace;
    SkseDependencySpace *space;
} Plan;

int skse_trial_system_library(const char *name) {
    static const char *const system[] = {
        "kernel32.dll", "kernelbase.dll", "ntdll.dll", "advapi32.dll",
        "shell32.dll", "shlwapi.dll", "ws2_32.dll", "user32.dll", "gdi32.dll",
        "ole32.dll", "oleaut32.dll", "comctl32.dll", "comdlg32.dll", "rpcrt4.dll",
        "version.dll", "winmm.dll", "msvcrt.dll", "ucrtbase.dll",
        "d3d9.dll", "d3dx9_42.dll", "d3dx9_43.dll", "dxgi.dll", "dinput8.dll"
    };
    if (!name) return false;
    for (size_t i = 0; i < sizeof system / sizeof system[0]; ++i) {
        if (skse_pe_library_name_equal(name, system[i])) return true;
    }
    return false;
}

int skse_dependency_local_name_valid(const char *name) {
    if (!name) return false;
    size_t n = 0;
    for (; n < SKSE_PE_MAX_IMPORT_NAME && name[n]; ++n) {
        unsigned c = (uint8_t)name[n];
        if (c < 0x20 || c >= 0x7f || c == '/' || c == '\\' || c == ':') return false;
    }
    return n > 4 && n < SKSE_PE_MAX_IMPORT_NAME && skse_pe_library_name_equal(name + n - 4, ".dll") &&
           !skse_trial_system_library(name);
}

static void copy_name(char dst[SKSE_PE_MAX_IMPORT_NAME], const char *src) {
    size_t i = 0;
    if (src) while (i + 1 < SKSE_PE_MAX_IMPORT_NAME && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static bool reject(Plan *plan, unsigned from, const char *name, SkseDependencyStatus reason, SksePeStatus pe) {
    SkseDependencyTrace *trace = plan->trace;
    trace->status = reason;
    trace->image_status = pe;
    trace->root_image_error = !from && reason == SKSE_DEPENDENCY_INVALID_IMAGE;
    copy_name(trace->requester, from ? plan->modules[from].source.file_name : NULL);
    copy_name(trace->requested, name);
    return false;
}

static bool first_library(const SksePeReport *manifest, unsigned i) {
    for (unsigned j = 0; j < i; ++j)
        if (skse_pe_library_name_equal(manifest->libraries[i], manifest->libraries[j])) return false;
    return true;
}

static int known_module(const Plan *plan, const char *name) {
    for (unsigned i = 0; i < plan->count; ++i) {
        const char *found = plan->modules[i].source.file_name;
        if (found && skse_pe_library_name_equal(name, found)) return (int)i;
    }
    return -1;
}

static bool walk(Plan *plan, unsigned index);

static bool resolve(Plan *plan, unsigned from, const char *name) {
    if (!skse_dependency_local_name_valid(name)) return reject(plan, from, name, SKSE_DEPENDENCY_INVALID_NAME, SKSE_PE_BAD_ARGUMENT);
    int known = known_module(plan, name);
    /* Check ambiguity across the root and the whole *selected* directory,
     * even when a previous branch has already found a matching module. */
    unsigned matches = known == 0 ? 1u : 0u;
    const SkseTrialDependency *target = NULL;
    for (size_t i = 0; i < plan->catalog_count; ++i) {
        const char *candidate = plan->catalog[i].file_name;
        if (!candidate || !skse_pe_library_name_equal(candidate, name)) continue;
        target = &plan->catalog[i];
        if (++matches > 1) return reject(plan, from, name, SKSE_DEPENDENCY_AMBIGUOUS, SKSE_PE_BAD_ARGUMENT);
    }
    if (known >= 0) {
        if (plan->modules[known].visit == VISITING)
            return reject(plan, from, name, (unsigned)known == from ? SKSE_DEPENDENCY_SELF : SKSE_DEPENDENCY_CYCLE,
                          SKSE_PE_UNSUPPORTED_IMPORTS);
        plan->modules[known].consumers |= 1u << from;
        return true;
    }
    if (!target) return reject(plan, from, name, SKSE_DEPENDENCY_MISSING, SKSE_PE_MISSING_DEPENDENCY_EXPORT);
    if (!target->bytes || !target->length) return reject(plan, from, name, SKSE_DEPENDENCY_UNREADABLE, SKSE_PE_BAD_ARGUMENT);
    if (plan->count == SKSE_DEPENDENCY_MAX_MODULES)
        return reject(plan, from, name, SKSE_DEPENDENCY_LIMIT, SKSE_PE_SIZE_LIMIT);
    unsigned next = plan->count++;
    plan->modules[next].source = *target;
    plan->modules[next].consumers = 1u << from;
    return walk(plan, next);
}

static bool walk(Plan *plan, unsigned index) {
    Module *module = &plan->modules[index];
    const char *name = index ? module->source.file_name : NULL;
    if (module->source.length > SKSE_PE_MAX_FILE_BYTES - plan->source_bytes)
        return reject(plan, index, name, SKSE_DEPENDENCY_SIZE_LIMIT, SKSE_PE_SIZE_LIMIT);
    plan->source_bytes += module->source.length;
    module->visit = VISITING;
    SksePeStatus pe = skse_pe_inspect(module->source.bytes, module->source.length, &module->manifest);
    /* A plain helper need not export SKSE functions. Full required exports,
     * import targets, relocations and the entry point are checked while mapping. */
    if (!(pe == SKSE_PE_OK || pe == SKSE_PE_UNSUPPORTED_IMPORTS || pe == SKSE_PE_UNSUPPORTED_ENTRY ||
          (plan->static_tls && pe == SKSE_PE_UNSUPPORTED_TLS) ||
          (index && pe == SKSE_PE_MISSING_EXPORTS)))
        return reject(plan, index, name, SKSE_DEPENDENCY_INVALID_IMAGE, pe);
    if (module->manifest.image_size > SKSE_PE_MAX_FILE_BYTES - plan->image_bytes)
        return reject(plan, index, name, SKSE_DEPENDENCY_SIZE_LIMIT, SKSE_PE_SIZE_LIMIT);
    plan->image_bytes += module->manifest.image_size;
    if (index == 0) {
        pe = skse_pe_classic_exports(module->source.bytes, module->source.length);
        if (pe != SKSE_PE_OK) return reject(plan, 0, NULL, SKSE_DEPENDENCY_INVALID_IMAGE, pe);
        /* Count all direct imports before traversing, including on a later refusal. */
        for (unsigned i = 0; i < module->manifest.library_count; ++i)
            if (!skse_trial_system_library(module->manifest.libraries[i]) && first_library(&module->manifest, i))
                ++plan->trace->direct_count;
    }
    for (unsigned i = 0; i < module->manifest.library_count; ++i) {
        const char *link = module->manifest.libraries[i];
        if (skse_trial_system_library(link) || !first_library(&module->manifest, i)) continue;
        if (!resolve(plan, index, link)) return false;
    }
    module->visit = FINISHED;
    if (index) {
        module->attach_step = plan->ordered;
        plan->space->helper_order[plan->ordered++] = index;
    }
    return true;
}

/* The authored Windows diamond unloads its two independent branches in import
 * order. A shared provider stays live until its last consumer has detached. */
static bool plan_detach(Plan *plan) {
    unsigned pending = ((1u << plan->count) - 1u) & ~1u; /* The root detaches first. */
    for (unsigned step = 0; step < plan->ordered; ++step) {
        unsigned node = 1;
        while (node < plan->count && (!(pending & (1u << node)) || (plan->modules[node].consumers & pending))) ++node;
        if (node == plan->count) return reject(plan, 0, NULL, SKSE_DEPENDENCY_CYCLE, SKSE_PE_UNSUPPORTED_IMPORTS);
        plan->space->detach_steps[step] = plan->modules[node].attach_step;
        pending &= ~(1u << node);
    }
    return true;
}

static bool space_available(const Plan *plan, uint32_t base, uint32_t length) {
    uint64_t end = (uint64_t)base + length;
    if (!base || (base & 0xffffu) || !length || end > UINT64_C(0x100000000)) return false;
    for (unsigned i = 0; i < plan->count; ++i) {
        const Module *other = &plan->modules[i];
        if (other->base && end > other->base && base < (uint64_t)other->base + other->manifest.image_size) return false;
    }
    for (size_t i = 0; i < plan->service_count; ++i) {
        uint32_t address = plan->services[i].service_token;
        if (address >= base && address < end) return false;
    }
    return true;
}

static bool assign_addresses(Plan *plan, uint32_t root_base) {
    Module *root = &plan->modules[0];
    if (!root_base) root_base = root->manifest.preferred_base;
    if (!space_available(plan, root_base, root->manifest.image_size))
        return reject(plan, 0, NULL, SKSE_DEPENDENCY_ADDRESS_SPACE, SKSE_PE_UNSUPPORTED_LAYOUT);
    root->base = root_base;
    /* Give fixed images their only possible addresses before placing movable DLLs. */
    for (unsigned pass = 0; pass < 2; ++pass) for (unsigned i = 1; i < plan->count; ++i) {
        Module *module = &plan->modules[i];
        bool relocatable = (module->manifest.directory_mask & (1u << 5)) != 0;
        if (relocatable != (pass != 0)) continue;
        uint32_t length = module->manifest.image_size;
        uint32_t base = module->manifest.preferred_base;
        if (!space_available(plan, base, length)) {
            if (!relocatable)
                return reject(plan, i, module->source.file_name, SKSE_DEPENDENCY_ADDRESS_SPACE, SKSE_PE_RELOCATIONS_REQUIRED);
            /* Each valid PE is at most 32 MiB. These disjoint slots are guest scalars. */
            static const uint32_t candidates[] = {0x40000000, 0x50000000, 0x70000000, 0x78000000, 0x20000000, 0x48000000};
            base = 0;
            for (size_t j = 0; j < sizeof candidates / sizeof candidates[0]; ++j)
                if (space_available(plan, candidates[j], length)) { base = candidates[j]; break; }
            if (!base) return reject(plan, i, module->source.file_name, SKSE_DEPENDENCY_ADDRESS_SPACE, SKSE_PE_UNSUPPORTED_LAYOUT);
        }
        module->base = base;
    }
    return true;
}

static int required_index(const Module *module, const char *name) {
    for (unsigned i = 0; i < module->required_count; ++i)
        if (!strcmp(module->required[i].name, name)) return (int)i;
    return -1;
}

static bool gather_exports(Plan *plan) {
    /* In a diamond a provider may need distinct exports for several consumers.
     * Collect the union before allocating even the first leaf image. */
    for (unsigned i = 0; i < plan->count; ++i) {
        const SksePeReport *manifest = &plan->modules[i].manifest;
        for (unsigned j = 0; j < manifest->symbol_count; ++j) {
            const SksePeImportSymbol *symbol = &manifest->symbols[j];
            const char *library = manifest->libraries[symbol->library_index];
            if (symbol->by_ordinal) return reject(plan, i, library, SKSE_DEPENDENCY_INVALID_IMAGE, SKSE_PE_ORDINAL_IMPORT);
            if (skse_trial_system_library(library)) continue;
            int provider = known_module(plan, library);
            if (provider < 1) return reject(plan, i, library, SKSE_DEPENDENCY_MISSING, SKSE_PE_MISSING_DEPENDENCY_EXPORT);
            Module *target = &plan->modules[provider];
            if (required_index(target, symbol->name) >= 0) continue;
            if (target->required_count == SKSE_PE_MAX_IMPORT_SYMBOLS)
                return reject(plan, i, library, SKSE_DEPENDENCY_SIZE_LIMIT, SKSE_PE_SIZE_LIMIT);
            target->required[target->required_count++].name = symbol->name;
        }
    }
    return true;
}

static uint32_t service_address(const Plan *plan, const char *library, const char *name) {
    for (size_t i = 0; i < plan->service_count; ++i) {
        const SksePeImportBinding *service = &plan->services[i];
        if (skse_pe_library_name_equal(service->library, library) && !strcmp(service->symbol, name))
            return service->service_token;
    }
    return 0;
}

static bool services_valid(const SksePeImportBinding *services, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const SksePeImportBinding *current = &services[i];
        if (!skse_trial_system_library(current->library) || !current->symbol || !current->service_token) return false;
        size_t size = 0;
        while (size < SKSE_PE_MAX_IMPORT_NAME && current->symbol[size]) {
            unsigned byte = (uint8_t)current->symbol[size++];
            if (byte < 32 || byte > 126) return false;
        }
        if (!size || size == SKSE_PE_MAX_IMPORT_NAME) return false;
        for (size_t j = 0; j < i; ++j) {
            if (services[j].service_token == current->service_token ||
                (skse_pe_library_name_equal(services[j].library, current->library) &&
                 !strcmp(services[j].symbol, current->symbol))) return false;
        }
    }
    return true;
}

static bool map_module(Plan *plan, unsigned index) {
    Module *module = &plan->modules[index];
    SksePeImportBinding bindings[SKSE_PE_MAX_IMPORT_SYMBOLS];
    size_t count = 0;
    for (unsigned i = 0; i < module->manifest.symbol_count; ++i) {
        const SksePeImportSymbol *symbol = &module->manifest.symbols[i];
        const char *library = module->manifest.libraries[symbol->library_index];
        size_t j = 0;
        while (j < count && !(skse_pe_library_name_equal(library, bindings[j].library) &&
                              !strcmp(symbol->name, bindings[j].symbol))) ++j;
        if (j < count) continue;
        uint32_t address = service_address(plan, library, symbol->name);
        if (!address && !skse_trial_system_library(library)) {
            int target = known_module(plan, library);
            if (target > 0) {
                Module *provider = &plan->modules[target];
                int exported = required_index(provider, symbol->name);
                if (exported >= 0) address = provider->required[exported].address;
            }
        }
        if (!address) {
            copy_name(plan->trace->import_symbol, symbol->name);
            return reject(plan, index, library, SKSE_DEPENDENCY_INVALID_IMAGE, SKSE_PE_UNAPPROVED_IMPORT);
        }
        bindings[count++] = (SksePeImportBinding){library, symbol->name, address};
    }
    const SkseTrialDependency *file = &module->source;
    SksePeImage *mapped = &plan->space->images[index];
    SksePeStatus status;
    if (plan->static_tls) {
        status = skse_pe_map_tls(file->bytes, file->length, module->base, bindings, count, SKSE_PE_ALLOW_ENTRY,
                                index ? module->required : NULL, index ? module->required_count : 0,
                                mapped, &plan->space->tls[index]);
    } else if (index) {
        status = skse_pe_map_library(file->bytes, file->length, module->base, bindings, count, SKSE_PE_ALLOW_ENTRY,
                                    module->required, module->required_count, mapped);
    } else {
        status = skse_pe_map_at_base(file->bytes, file->length, module->base, bindings, count, SKSE_PE_ALLOW_ENTRY, mapped);
    }
    return status == SKSE_PE_OK || reject(plan, index, index ? file->file_name : NULL,
                                         status == SKSE_PE_NO_MEMORY ? SKSE_DEPENDENCY_NO_MEMORY : SKSE_DEPENDENCY_INVALID_IMAGE, status);
}

void skse_dependency_close(SkseDependencySpace *space) {
    if (!space) return;
    for (unsigned i = 0; i < SKSE_DEPENDENCY_MAX_MODULES; ++i) skse_pe_free(&space->images[i]);
    memset(space, 0, sizeof *space);
}

SkseDependencyStatus skse_dependency_map(SkseTrialDependency primary, const SkseTrialDependency *catalog, size_t count,
                                        uint32_t primary_base, unsigned static_tls,
                                        const SksePeImportBinding *services, size_t service_count,
                                        SkseDependencySpace *out, SkseDependencyTrace *trace) {
    if (out) memset(out, 0, sizeof *out);
    if (!trace) return SKSE_DEPENDENCY_INVALID_IMAGE;
    memset(trace, 0, sizeof *trace);
    if (!out || !primary.bytes || !primary.length || (!catalog && count) || (!services && service_count)) {
        trace->image_status = SKSE_PE_BAD_ARGUMENT;
        trace->root_image_error = 1;
        return trace->status = SKSE_DEPENDENCY_INVALID_IMAGE;
    }
    if (count > SKSE_DEPENDENCY_MAX_CATALOG || service_count > SKSE_PE_MAX_IMPORT_SYMBOLS) {
        trace->image_status = SKSE_PE_SIZE_LIMIT;
        return trace->status = SKSE_DEPENDENCY_LIMIT;
    }
    if (!services_valid(services, service_count)) {
        trace->image_status = SKSE_PE_BAD_ARGUMENT;
        trace->root_image_error = 1;
        return trace->status = SKSE_DEPENDENCY_INVALID_IMAGE;
    }
    Plan *plan = calloc(1, sizeof *plan);
    if (!plan) { trace->image_status = SKSE_PE_NO_MEMORY; return trace->status = SKSE_DEPENDENCY_NO_MEMORY; }
    plan->modules[0].source = primary;
    plan->count = 1;
    plan->space = out;
    plan->trace = trace;
    plan->catalog = catalog;
    plan->catalog_count = count;
    plan->services = services;
    plan->service_count = service_count;
    plan->static_tls = static_tls;
    bool valid = walk(plan, 0) && plan_detach(plan) && assign_addresses(plan, primary_base) && gather_exports(plan);
    for (unsigned i = 0; valid && i < plan->ordered; ++i) valid = map_module(plan, out->helper_order[i]);
    if (valid) valid = map_module(plan, 0);
    if (valid) {
        out->count = plan->count;
        trace->helper_count = plan->ordered;
        for (unsigned i = 0; i < plan->ordered; ++i)
            copy_name(trace->load_order[i], plan->modules[out->helper_order[i]].source.file_name);
    } else {
        skse_dependency_close(out);
    }
    free(plan);
    return trace->status;
}
