/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "mod_papyrus_inputs.h"
#include "probe_asset_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char dll_path[] = "Data/SKSE/Plugins/StorageUtil.dll";
static const char pex_path[] = "Data/Scripts/JsonUtil.pex";
static const char wearable_plugin_path[] = "Data/Chesko_WearableLantern.esp";
static const char storage_pex_path[] = "Data/Scripts/StorageUtil.pex";
/* PapyrusUtil Classic 3.3 is 978944 and 8182 bytes respectively. */
enum { DLL_LIMIT = 2 * 1024 * 1024, PEX_LIMIT = 64 * 1024, WEARABLE_PLUGIN_LIMIT = 110384 };
static const SkyrimPapyrusInputs unavailable = {.settings_fd = -1, .game_fd = -1};

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool parse_uuid(const char *name, uint8_t *bytes) {
    if (!name || strlen(name) != 36) return false;
    for (size_t i = 0; i < 16; ++i) {
        if ((i == 4 || i == 6 || i == 8 || i == 10) && *name++ != '-') return false;
        const int a = hex_digit(*name++), b = hex_digit(*name++);
        if (a < 0 || b < 0) return false;
        if (bytes) bytes[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}

static int owned_directory_at(int parent, const char *name, const struct stat *support,
                              bool private) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat info;
    int error = 0;
    if (fstat(fd, &info)) error = errno;
    else if (info.st_dev != support->st_dev || info.st_uid != support->st_uid ||
             (private && (info.st_mode & (S_IRWXG | S_IRWXO)))) error = EACCES;
    if (error) {
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int snapshot(int directory, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    /* Translate the selected-file reader's Windows errors at the iOS boundary. */
    switch (skse_probe_asset_read_unique(&directory, path, limit, bytes, length)) {
        case 0: return 0;
        case 2: case 3: return ENOENT;
        case 5: return EACCES;
        case 8: return ENOMEM;
        case 223: return EFBIG;
        default: return EIO;
    }
}

static int package_asset_matches(int package, const char *path, const uint8_t *expected,
                                 size_t expected_size, bool *found) {
    uint8_t *actual = NULL;
    size_t size = 0;
    int error = snapshot(package, path, expected_size, &actual, &size);
    *found = error == 0;
    if (error == ENOENT) return 0;
    if (error == EFBIG || (!error && (size != expected_size || memcmp(actual, expected, size))))
        error = ESTALE;
    free(actual);
    return error;
}

static int validate_package_pair(int mods, const struct stat *support, const char *const *ids,
                                 size_t count, const SkyrimPapyrusInputs *inputs) {
    /* Later packages win. The first package that provides either file must
     * provide both, and the actual projected bytes must match that package. */
    for (size_t i = count; i > 0; --i) {
        int package = owned_directory_at(mods, ids[i-1], support, false);
        if (package < 0) return errno;
        bool dll, pex = false;
        int error = package_asset_matches(package, dll_path, inputs->dll, inputs->dll_size, &dll);
        if (!error) error = package_asset_matches(package, pex_path, inputs->pex, inputs->pex_size, &pex);
        close(package);
        if (error) return error;
        if (dll || pex) return dll && pex ? 0 : ESTALE;
    }
    return ENOENT;
}

int skyrim_papyrus_inputs_open(int support_fd, const char *profile_id,
                             const char *const *package_ids, size_t count,
                             SkyrimPapyrusInputs *out) {
    if (!out) return EINVAL;
    *out = unavailable;
    SkyrimPapyrusInputs inputs = unavailable;
    if (!parse_uuid(profile_id, inputs.profile) || !package_ids || !count) return EINVAL;
    for (size_t i = 0; i < count; ++i)
        if (!parse_uuid(package_ids[i], NULL)) return EINVAL;
    struct stat support;
    const int flags = fcntl(support_fd, F_GETFL);
    if (flags < 0) return errno;
    if (fstat(support_fd, &support)) return errno;
    if ((flags & O_ACCMODE) != O_RDONLY || !S_ISDIR(support.st_mode) ||
        support.st_uid != geteuid()) return EACCES;

    int views = owned_directory_at(support_fd, "Views", &support, false);
    if (views < 0) return errno;
    inputs.game_fd = owned_directory_at(views, profile_id, &support, false);
    int error = inputs.game_fd < 0 ? errno : 0;
    close(views);
    if (error) return error;

    error = snapshot(inputs.game_fd, dll_path, DLL_LIMIT, &inputs.dll, &inputs.dll_size);
    if (!error) error = snapshot(inputs.game_fd, pex_path, PEX_LIMIT, &inputs.pex, &inputs.pex_size);
    if (!error && (!inputs.dll_size || !inputs.pex_size)) error = ENOEXEC;
    if (!error) {
        int mods = owned_directory_at(support_fd, "Mods", &support, false);
        if (mods < 0) error = errno;
        else {
            error = validate_package_pair(mods, &support, package_ids, count, &inputs);
            close(mods);
        }
    }
    if (!error && mkdirat(support_fd, "PapyrusSettings", 0700) && errno != EEXIST) error = errno;
    if (!error) {
        inputs.settings_fd = owned_directory_at(support_fd, "PapyrusSettings", &support, true);
        if (inputs.settings_fd < 0) error = errno;
    }
    if (!error && !snapshot(inputs.game_fd, wearable_plugin_path, WEARABLE_PLUGIN_LIMIT,
                            &inputs.wearable_plugin, &inputs.wearable_plugin_size)) {
        (void)snapshot(inputs.game_fd, storage_pex_path, PEX_LIMIT, &inputs.storage_pex, &inputs.storage_pex_size);
    }
    if (error) skyrim_mods_release_papyrus_inputs(&inputs);
    else *out = inputs;
    return error;
}

void skyrim_mods_release_papyrus_inputs(SkyrimPapyrusInputs *inputs) {
    if (!inputs) return;
    if (inputs->settings_fd >= 0) close(inputs->settings_fd);
    if (inputs->game_fd >= 0) close(inputs->game_fd);
    free(inputs->dll);
    free(inputs->pex);
    free(inputs->wearable_plugin);
    free(inputs->storage_pex);
    *inputs = unavailable;
}
