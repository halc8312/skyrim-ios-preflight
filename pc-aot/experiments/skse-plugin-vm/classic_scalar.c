/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_scalar.h"
#include <string.h>

/* Classic 1.7.3 / TESV 1.9.32 VM helpers verified for the hosted typed frame. */
static const SkseX86Service scalar_helpers[] = {
    {0x00c3a620, SKSE_CLASSIC_FRAME_ARG_OFFSET, SKSE_X86_CLASSIC_FRAME},
    {0x00c3ac40, SKSE_CLASSIC_FRAME_ARG_GET, SKSE_X86_CLASSIC_FRAME},
    {0x00c328e0, SKSE_CLASSIC_FRAME_DESTROY, SKSE_X86_CLASSIC_FRAME},
    {0x00a51210, SKSE_CLASSIC_FRAME_STRING_SET, SKSE_X86_CLASSIC_FRAME},
    {0x00a511c0, SKSE_CLASSIC_FRAME_STRING_CONSTRUCT, SKSE_X86_CLASSIC_FRAME},
    {0xe0010660, SKSE_CLASSIC_FRAME_INVALID_HANDLE, SKSE_X86_CLASSIC_FRAME},
    {0xe0010664, SKSE_CLASSIC_FRAME_HAS_TYPE, SKSE_X86_CLASSIC_FRAME},
    {0xe0010668, SKSE_CLASSIC_FRAME_VALID_HANDLE, SKSE_X86_CLASSIC_FRAME},
    {0xe001066c, SKSE_CLASSIC_FRAME_RESOLVE_HANDLE, SKSE_X86_CLASSIC_FRAME}
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
        if (value->type != row->arguments[i]) return false;
        if (skse_classic_type_is_form(value->type)) {
            if (value->bits || value->text || value->text_size) return false;
            continue; /* Explicit None, unless the prepared first argument supplies a Form. */
        }
        if (value->type < 2 || value->type > 5) return false;
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

typedef struct {
    SkseClassicFormArgument form;
    uint8_t policy_root[4], policy[0x40 + SKSE_CLASSIC_FORM_VIEW_BYTES];
} ScalarForm;

static bool prepare_form(ScalarForm *borrowed, const SkseClassicFormSnapshot *identity, SkseX86Heap *heap) {
    enum { Policy = 0x14008000, Form = Policy + 0x40 };
    SkseClassicFormArgument *form = &borrowed->form;
    *form = (SkseClassicFormArgument){.policy = Policy, .view = Form, .form_id = identity->id,
        .form_type = identity->type, .handle = identity->handle,
        .identifier = skse_x86_heap_allocate(heap, SKSE_CLASSIC_IDENTIFIER_BYTES, true)};
    uint8_t *identifier = skse_x86_heap_memory(heap, form->identifier, SKSE_CLASSIC_IDENTIFIER_BYTES, true);
    if (!identifier) {
        if (form->identifier) skse_x86_heap_release(heap, form->identifier);
        return false;
    }
    put_word(identifier + 0x10, (uint32_t)form->handle);
    put_word(identifier + 0x14, (uint32_t)(form->handle >> 32));
    put_word(identifier + 0x18, 1);
    put_word(borrowed->policy_root, Policy);
    put_word(borrowed->policy, Policy + 8);
    put_word(borrowed->policy + 8 + 3 * 4, scalar_helpers[SKSE_CLASSIC_FRAME_INVALID_HANDLE].address);
    put_word(borrowed->policy + 8 + 1 * 4, scalar_helpers[SKSE_CLASSIC_FRAME_HAS_TYPE].address);
    put_word(borrowed->policy + 8 + 2 * 4, scalar_helpers[SKSE_CLASSIC_FRAME_VALID_HANDLE].address);
    put_word(borrowed->policy + 8 + 8 * 4, scalar_helpers[SKSE_CLASSIC_FRAME_RESOLVE_HANDLE].address);
    put_word(borrowed->policy + 0x40 + 0x0c, form->form_id);
    borrowed->policy[0x40 + 0x12] = form->form_type;
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
    const SkseClassicFormSnapshot *identity = scope->first_form;
    const size_t helpers_count = identity ? SCALAR_HELPERS : SKSE_CLASSIC_FRAME_INVALID_HANDLE;
    if (!services.heap || !services.entries || services.count > SKSE_X86_MAX_SERVICES - helpers_count ||
        services.classic_catalog || services.classic_frame || services.classic_load_forms ||
        services.read_view_count > SCALAR_VIEWS || (services.read_view_count && !services.read_views))
        return SKSE_SCALAR_ARGUMENT;
    if (identity && (preparing || !count || row->arguments[0] != SKSE_CLASSIC_CATALOG_FORM ||
                     !identity->id || !identity->type || !identity->handle)) return SKSE_SCALAR_ARGUMENT;

    SkseClassicStringResult text = {.address = scope->text_base};
    SkseClassicFrame frame = {.function = row, .string_result = row->result == 2 ? &text : NULL, .preparing = preparing};
    uint8_t argument_text[SKSE_CLASSIC_CATALOG_MAX_ARGS * SKSE_CLASSIC_FRAME_STRING_BYTES];
    size_t used = 0, view_count = services.read_view_count;
    if (!copy_arguments(row, input, scope->arguments_base, argument_text, &used, &frame) ||
        view_count + (used != 0) + (frame.string_result != NULL) + (identity ? 2u : 0u) > SCALAR_VIEWS)
        return SKSE_SCALAR_ARGUMENT;

    SkseX86ReadView views[SCALAR_VIEWS];
    if (view_count) memcpy(views, services.read_views, view_count * sizeof views[0]);
    if (used) views[view_count++] = (SkseX86ReadView){scope->arguments_base, argument_text, used};
    if (frame.string_result) views[view_count++] = (SkseX86ReadView){text.address, text.bytes, sizeof text.bytes};
    SkseX86Heap *heap = services.heap;
    ScalarForm borrowed = {0};
    if (identity) {
        if (!prepare_form(&borrowed, identity, heap)) return SKSE_SCALAR_MEMORY;
        frame.form = &borrowed.form;
        frame.arguments[0] = borrowed.form.identifier;
        views[view_count++] = (SkseX86ReadView){SKSE_CLASSIC_FORM_POLICY_SLOT, borrowed.policy_root, sizeof borrowed.policy_root};
        views[view_count++] = (SkseX86ReadView){borrowed.form.policy, borrowed.policy, sizeof borrowed.policy};
    }
    services.read_views = views;
    services.read_view_count = view_count;
    SkseX86Service entries[SKSE_X86_MAX_SERVICES];
    memcpy(entries, services.entries, services.count * sizeof entries[0]);
    memcpy(entries + services.count, scalar_helpers, helpers_count * sizeof scalar_helpers[0]);
    services.entries = entries;
    services.count += helpers_count;
    services.classic_frame = &frame;

    frame.address = skse_x86_heap_allocate(heap, SKSE_CLASSIC_FRAME_BYTES, true);
    uint8_t *bytes = frame.address ? skse_x86_heap_memory(heap, frame.address, SKSE_CLASSIC_FRAME_BYTES, true) : NULL;
    if (!bytes) {
        if (frame.address) skse_x86_heap_release(heap, frame.address);
        if (identity) skse_x86_heap_release(heap, borrowed.form.identifier);
        return SKSE_SCALAR_MEMORY;
    }
    put_word(bytes + SKSE_CLASSIC_FRAME_STATE, frame.address + SKSE_CLASSIC_FRAME_LIST);
    put_word(bytes + SKSE_CLASSIC_FRAME_STATE + 0x1c, row->count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t declared = input[i].type;
        const uint32_t live_type = skse_classic_type_is_form(declared) ? (identity && i == 0) : declared;
        put_word(bytes + SKSE_CLASSIC_FRAME_ARGS + i * 8, live_type);
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
    if (identity && !skse_x86_heap_release(heap, borrowed.form.identifier)) status = SKSE_SCALAR_RESULT;
    if (status == SKSE_SCALAR_OK || status == SKSE_SCALAR_ARGUMENT) {
        trial->phase = SKSE_PAPYRUS_REGISTERED;
    } else {
        trial->phase = SKSE_PAPYRUS_SEALED;
        if (services.files) services.files->profile_aborted = true;
    }
    if (status == SKSE_SCALAR_OK) *output = completed;
    return status;
}
