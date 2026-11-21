/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_scalar.h"
#include <string.h>

/* Classic 1.7.3 / TESV 1.9.32 VM helpers verified for the hosted typed frame. */
static const SkseX86Service scalar_helpers[] = {
    {0x00c3a620, SKSE_CLASSIC_FRAME_ARG_OFFSET, SKSE_X86_CLASSIC_FRAME},
    {0x00c3ac40, SKSE_CLASSIC_FRAME_ARG_GET, SKSE_X86_CLASSIC_FRAME},
    {0x00c328e0, SKSE_CLASSIC_FRAME_DESTROY, SKSE_X86_CLASSIC_FRAME},
    {0x00a51210, SKSE_CLASSIC_FRAME_STRING_SET, SKSE_X86_CLASSIC_FRAME},
    {0x00a511c0, SKSE_CLASSIC_FRAME_STRING_CONSTRUCT, SKSE_X86_CLASSIC_FRAME}
};
enum { SCALAR_HELPERS = sizeof scalar_helpers / sizeof scalar_helpers[0], SCALAR_VIEWS = 8 };

static void put_word(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t word(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool owns_declaration(const SkseClassicScalarScope *scope, const SkseClassicDeclaration *row) {
    const SkseClassicCatalog *catalog = scope->catalog;
    const SksePapyrusTrial *trial = scope->services->papyrus;
    if (!catalog || !catalog->rows || !catalog->count || catalog->count > catalog->limit ||
        catalog->count > SKSE_CLASSIC_CATALOG_MAX_ROWS ||
        !trial || catalog->trial != trial || catalog->epoch != scope->epoch || trial->epoch != scope->epoch ||
        trial->phase != SKSE_PAPYRUS_REGISTERED) return false;
    for (size_t i = 0; i < catalog->count; ++i)
        if (&catalog->rows[i] == row) return true;
    return false;
}

static bool copy_arguments(const SkseClassicDeclaration *row, const SkseClassicScalar *input, uint32_t address,
                           uint8_t *text, size_t *used, SkseClassicFrame *frame) {
    for (size_t i = 0; i < row->count; ++i) {
        const SkseClassicScalar *value = &input[i];
        if (value->type != row->arguments[i] || value->type < 2 || value->type > 5) return false;
        if (value->type != 2) {
            if (value->text || value->text_size || (value->type == 5 && value->bits > 1)) return false;
            frame->arguments[i] = value->bits;
            continue;
        }
        const size_t size = value->text_size;
        if (!address || value->bits || !value->text || !size || size > SKSE_CLASSIC_FRAME_STRING_BYTES ||
            memchr(value->text, 0, size) != value->text + size - 1 ||
            (uint64_t)address + *used + size > UINT32_MAX) return false;
        memcpy(text + *used, value->text, size);
        frame->arguments[i] = address + (uint32_t)*used;
        *used += size;
    }
    return true;
}

static bool read_result(SkseX86Heap *heap, const SkseClassicFrame *frame, SkseClassicScalarResult *result) {
    const uint8_t *bytes = skse_x86_heap_memory(heap, frame->address, 8, false);
    if (!bytes || word(bytes) != frame->function->result) return false;
    result->type = word(bytes);
    const uint32_t bits = word(bytes + 4);
    if (result->type != 2) {
        if (result->type == 5 && bits > 1) return false;
        result->bits = bits;
        return true;
    }
    SkseClassicStringResult *text = frame->string_result;
    if (!text || !text->selected || text->selected > text->count || text->selected > SKSE_CLASSIC_FRAME_STRINGS)
        return false;
    const unsigned slot = text->selected - 1, offset = slot * SKSE_CLASSIC_FRAME_STRING_BYTES;
    const size_t size = text->lengths[slot];
    if (bits != text->address + offset || !size || size > sizeof result->text ||
        memchr(text->bytes + offset, 0, size) != text->bytes + offset + size - 1) return false;
    memcpy(result->text, text->bytes + offset, size);
    return true;
}

SkseClassicScalarStatus skse_classic_scalar_call(const SkseClassicScalarScope *scope,
    const SkseClassicDeclaration *row, const SkseClassicScalar *input, size_t count, uint32_t work_limit,
    bool preparing, SkseClassicScalarResult *output, SkseX86Result *trace) {
    if (trace) *trace = (SkseX86Result){.status = SKSE_X86_BAD_ARGUMENT};
    if (!scope || !scope->services || !scope->epoch || !scope->stack_id ||
        !row || !output || (count && !input) || count > SKSE_CLASSIC_CATALOG_MAX_ARGS ||
        !owns_declaration(scope, row) || count != row->count || row->is_static != 1 || row->result < 2 || row->result > 5 ||
        (row->result == 2 && !scope->text_base) || !work_limit ||
        (scope->services->files && scope->services->files->profile_aborted) ||
        (preparing && (scope->prepare_method != row || !scope->services->files || !scope->services->files->read_only)))
        return SKSE_SCALAR_ARGUMENT;
    SkseX86Services services = *scope->services;
    if (!services.heap || !services.entries || services.count > SKSE_X86_MAX_SERVICES - SCALAR_HELPERS ||
        services.classic_catalog || services.classic_frame || services.classic_load_forms ||
        services.read_view_count > SCALAR_VIEWS || (services.read_view_count && !services.read_views))
        return SKSE_SCALAR_ARGUMENT;

    SkseClassicStringResult text = {.address = scope->text_base};
    SkseClassicFrame frame = {.function = row, .string_result = row->result == 2 ? &text : NULL, .preparing = preparing};
    uint8_t argument_text[SKSE_CLASSIC_CATALOG_MAX_ARGS * SKSE_CLASSIC_FRAME_STRING_BYTES];
    size_t used = 0, view_count = services.read_view_count;
    if (!copy_arguments(row, input, scope->arguments_base, argument_text, &used, &frame) ||
        view_count + (used != 0) + (frame.string_result != NULL) > SCALAR_VIEWS) return SKSE_SCALAR_ARGUMENT;

    SkseX86ReadView views[SCALAR_VIEWS];
    if (view_count) memcpy(views, services.read_views, view_count * sizeof views[0]);
    if (used) views[view_count++] = (SkseX86ReadView){scope->arguments_base, argument_text, used};
    if (frame.string_result) views[view_count++] = (SkseX86ReadView){text.address, text.bytes, sizeof text.bytes};
    services.read_views = views;
    services.read_view_count = view_count;
    SkseX86Service entries[SKSE_X86_MAX_SERVICES];
    memcpy(entries, services.entries, services.count * sizeof entries[0]);
    memcpy(entries + services.count, scalar_helpers, sizeof scalar_helpers);
    services.entries = entries;
    services.count += SCALAR_HELPERS;
    services.classic_frame = &frame;

    SkseX86Heap *heap = services.heap;
    frame.address = skse_x86_heap_allocate(heap, SKSE_CLASSIC_FRAME_BYTES, true);
    if (!frame.address) return SKSE_SCALAR_MEMORY;
    uint8_t *bytes = skse_x86_heap_memory(heap, frame.address, SKSE_CLASSIC_FRAME_BYTES, true);
    if (!bytes) {
        skse_x86_heap_release(heap, frame.address);
        return SKSE_SCALAR_MEMORY;
    }
    put_word(bytes + SKSE_CLASSIC_FRAME_STATE, frame.address + SKSE_CLASSIC_FRAME_LIST);
    put_word(bytes + SKSE_CLASSIC_FRAME_STATE + 0x1c, row->count);
    for (size_t i = 0; i < count; ++i) {
        put_word(bytes + SKSE_CLASSIC_FRAME_ARGS + i * 8, input[i].type);
        put_word(bytes + SKSE_CLASSIC_FRAME_ARGS + i * 8 + 4, frame.arguments[i]);
    }

    SksePapyrusTrial *trial = services.papyrus;
    const uint32_t arguments[] = {0, trial->registry, scope->stack_id, frame.address,
                                  frame.address + SKSE_CLASSIC_FRAME_STATE};
    trial->phase = SKSE_PAPYRUS_RUNNING;
    const SkseX86Result execution = skse_x86_call_thiscall(scope->images, scope->image_count, row->run, row->object,
        scope->scratch, arguments, sizeof arguments / sizeof arguments[0], work_limit, services);
    if (trace) *trace = execution;

    SkseClassicScalarResult completed = {0};
    SkseClassicScalarStatus status = execution.status == SKSE_X86_BAD_ARGUMENT && !execution.work
        ? SKSE_SCALAR_ARGUMENT : SKSE_SCALAR_EXECUTION;
    if (execution.status == SKSE_X86_OK)
        status = (uint8_t)execution.eax == 1 && read_result(heap, &frame, &completed) ? SKSE_SCALAR_OK : SKSE_SCALAR_RESULT;
    if (!skse_x86_heap_release(heap, frame.address)) status = SKSE_SCALAR_RESULT;
    if (status == SKSE_SCALAR_OK || status == SKSE_SCALAR_ARGUMENT) {
        trial->phase = SKSE_PAPYRUS_REGISTERED;
    } else {
        trial->phase = SKSE_PAPYRUS_SEALED;
        if (services.files) services.files->profile_aborted = true;
    }
    if (status == SKSE_SCALAR_OK) *output = completed;
    return status;
}
