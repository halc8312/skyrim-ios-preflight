/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Classic (2011 / LE) plugin selection for the app profile.
 *
 * Startup-only, pure C. Reads only the TES4 header of a plugin the user put in
 * Data. Never changes Data, plugin bytes, timestamps, BSAs or saves. The only
 * file the caller publishes is the profile's plugins.txt, which the guest
 * reads through SHGetFolderPathA -> C:\Users\Player\Documents\Skyrim. */

enum { SKYRIM_PLUGIN_NAME_MAX = 256 };
/* Skyrim.esm + Update.esm + 253 others: the classic engine's 255 slots
 * (0xFF is reserved for runtime forms). */
enum { SKYRIM_PLUGIN_MAX_LOADED = 255, SKYRIM_PLUGIN_BASE_COUNT = 2 };
/* A TES4 header larger than this is refused instead of read into memory. */
enum { SKYRIM_PLUGIN_HEADER_MAX = 4 * 1024 * 1024 };

enum {
    SKYRIM_PLUGIN_HEADER_OK = 0,
    SKYRIM_PLUGIN_HEADER_TRUNCATED,     /* shorter than its declared header */
    SKYRIM_PLUGIN_HEADER_NOT_TES4,      /* not a classic Skyrim plugin */
    SKYRIM_PLUGIN_HEADER_COMPRESSED,    /* compressed TES4 is not valid */
    SKYRIM_PLUGIN_HEADER_TOO_LARGE,
    SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD,
    SKYRIM_PLUGIN_HEADER_BAD_MASTER,    /* unterminated / invalid master name */
    SKYRIM_PLUGIN_HEADER_NO_MEMORY
};

typedef struct {
    unsigned flags;          /* TES4 record flags; bit 0 = master (ESM) */
    unsigned version;        /* form version from the record header */
    unsigned master_count;
    char *masters;           /* master_count NUL-terminated names, back to back */
} SkyrimPluginHeader;

/* Bytes needed to parse the header: 24 + TES4 data size. 0 when the first
 * 8 bytes are not a TES4 record or the size exceeds SKYRIM_PLUGIN_HEADER_MAX. */
size_t skyrim_plugin_header_size(const unsigned char *first8, size_t length);
int skyrim_plugin_parse_header(const unsigned char *bytes, size_t length,
                               SkyrimPluginHeader *out);
const char *skyrim_plugin_master(const SkyrimPluginHeader *header, unsigned index);
void skyrim_plugin_header_free(SkyrimPluginHeader *header);

/* 1 for a plain .esp/.esm file name the guest can name in plugins.txt:
 * printable ASCII, no path separators, at most SKYRIM_PLUGIN_NAME_MAX-1 bytes.
 * Non-ASCII names are refused: the guest reads plugins.txt as ANSI. */
int skyrim_plugin_name_valid(const char *name);
/* 1 for Skyrim.esm / Update.esm (case-insensitive): always loaded first. */
int skyrim_plugin_is_base(const char *name);
/* Sort rank for newly found plugins: official DLC in their usual order,
 * then other masters, then plugins. Lower first. */
int skyrim_plugin_default_rank(const char *name, int is_master);

typedef enum {
    SKYRIM_PLUGIN_LOAD = 0,
    SKYRIM_PLUGIN_OFF,
    SKYRIM_PLUGIN_UNREADABLE,      /* header could not be parsed */
    SKYRIM_PLUGIN_BAD_NAME,
    SKYRIM_PLUGIN_DUPLICATE,       /* same name, different case */
    SKYRIM_PLUGIN_MISSING_MASTER,  /* a master is absent, off or blocked */
    SKYRIM_PLUGIN_MASTER_AFTER,    /* an ESM that needs an ESP master */
    SKYRIM_PLUGIN_CYCLE,
    SKYRIM_PLUGIN_LIMIT
} SkyrimPluginState;

typedef struct {
    const char *name;                  /* file name as found in Data */
    int enabled;                       /* user choice */
    int header_status;                 /* SKYRIM_PLUGIN_HEADER_* */
    const SkyrimPluginHeader *header;  /* valid when header_status == OK */
} SkyrimPluginEntry;

/* entries are in the user's order. Writes count states, and the indices of
 * loaded entries in load order to order_out (capacity count). problem_out[i]
 * receives the index of the first master that blocked entry i, or -1 (the
 * master name itself is available from the header). Returns loaded count.
 *
 * Order: masters (ESM flag) before plugins; inside each group the user's order
 * is kept except that every plugin follows all of its masters. */
size_t skyrim_plugin_order(const SkyrimPluginEntry *entries, size_t count,
                           size_t *order_out, SkyrimPluginState *state_out,
                           int *problem_out);

/* Complete plugins.txt bytes for the classic engine: CRLF lines, comment
 * header, Skyrim.esm, Update.esm, then loaded entries. Caller frees.
 * NULL on allocation failure only. */
char *skyrim_plugin_list_text(const SkyrimPluginEntry *entries,
                              const size_t *order, size_t loaded, size_t *out_bytes);
/* 1 when bytes start with the managed-file comment written above. */
int skyrim_plugin_list_is_managed(const char *bytes, size_t length);

/* Resolve one existing child of directory case-insensitively, as the guest's
 * path resolver does. 1: unique match copied to out; 0: absent (name copied);
 * -1: ambiguous aliases, unreadable directory or capacity too small. */
int skyrim_plugin_child_path(const char *directory, const char *name,
                             char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
