/* The Classic i386 serialization v4 slots used by this fixture. */
#pragma once
#include <stddef.h>
#include <stdint.h>
struct SerialApi;
typedef void (__cdecl *SerialEvent)(struct SerialApi *);
struct SerialApi {
    unsigned version;
    void (__cdecl *unique_id)(unsigned, unsigned);
    void (__cdecl *on_revert)(unsigned, SerialEvent);
    void (__cdecl *on_save)(unsigned, SerialEvent);
    void (__cdecl *on_load)(unsigned, SerialEvent);
    void (__cdecl *on_delete)(unsigned, void (__cdecl *)(uint64_t));
    _Bool (__cdecl *write_record)(unsigned, unsigned, const void *, unsigned);
    _Bool (__cdecl *open_record)(unsigned, unsigned);
    _Bool (__cdecl *write_data)(const void *, unsigned);
    _Bool (__cdecl *next_record)(unsigned *, unsigned *, unsigned *);
    unsigned (__cdecl *read_data)(void *, unsigned);
    _Bool (__cdecl *resolve_handle)(uint64_t, uint64_t *);
    _Bool (__cdecl *resolve_form)(unsigned, unsigned *);
};
_Static_assert(sizeof(struct SerialApi) == 52 && offsetof(struct SerialApi, read_data) == 40, "i386 v4 layout");
