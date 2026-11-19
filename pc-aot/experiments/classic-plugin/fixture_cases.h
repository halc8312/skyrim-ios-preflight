/* Inputs shared by the Windows reference and the AOT harness. */
#pragma once
enum Case {
    GAME, WRONG_SKSE, WRONG_RUNTIME, EDITOR, OLD_RELEASE, INVALID_HANDLE,
    NO_RELEASE_CALLBACK, NO_HANDLE_CALLBACK, NULL_API, NULL_INFO, CASE_COUNT
};

struct Arguments {
    unsigned skse_version, runtime_version, editor, release, handle;
};
static struct Arguments arguments(enum Case id) {
    struct Arguments value = {0x01070030u, 0x09200000u, 0, 48, 59};
    if (id == WRONG_SKSE) value.skse_version = 0x01070020u;
    if (id == WRONG_RUNTIME) value.runtime_version = 0x091d0000u;
    if (id == EDITOR) value.editor = 1;
    if (id == OLD_RELEASE) value.release = 47;
    if (id == INVALID_HANDLE) value.handle = 0xffffffffu;
    return value;
}
