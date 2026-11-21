/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "mod_papyrus_inputs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_hex(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) printf("%02x", bytes[i]);
}

static void require_empty(const SkyrimPapyrusInputs *inputs) {
    assert(inputs->settings_fd == -1 && inputs->game_fd == -1);
    assert(!inputs->dll && !inputs->pex && !inputs->dll_size && !inputs->pex_size);
    assert(!inputs->wearable_plugin && !inputs->wearable_plugin_size);
    assert(!inputs->storage_pex && !inputs->storage_pex_size);
    for (size_t i = 0; i < sizeof inputs->profile; ++i) assert(!inputs->profile[i]);
}

static void require_directory(int fd, int root, const char *path, int is_private) {
    struct stat dir, expected;
    assert((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    assert(!fstat(fd, &dir) && !fstatat(root, path, &expected, AT_SYMLINK_NOFOLLOW));
    assert(S_ISDIR(dir.st_mode) && dir.st_dev == expected.st_dev && dir.st_ino == expected.st_ino);
    if (is_private) assert(!(dir.st_mode & 0077));
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(root >= 0);
    SkyrimPapyrusInputs inputs = {.settings_fd = -2, .game_fd = -2};
    int error = skyrim_papyrus_inputs_open(root, argv[2], (const char *const *)argv + 3,
                                          (size_t)(argc - 3), &inputs);
    printf("%d", error);
    if (error) {
        require_empty(&inputs);
    } else {
        char view[64];
        int n = snprintf(view, sizeof view, "Views/%s", argv[2]);
        assert(n > 0 && (size_t)n < sizeof view);
        require_directory(inputs.game_fd, root, view, 0);
        require_directory(inputs.settings_fd, root, "PapyrusSettings", 1);
        printf(" ");
        print_hex(inputs.profile, sizeof inputs.profile);
        printf(" ");
        print_hex(inputs.dll, inputs.dll_size);
        printf(" ");
        print_hex(inputs.pex, inputs.pex_size);
        if (inputs.wearable_plugin) {
            printf(" ");
            print_hex(inputs.wearable_plugin, inputs.wearable_plugin_size);
        }
        if (inputs.storage_pex) {
            printf(" ");
            print_hex(inputs.storage_pex, inputs.storage_pex_size);
        }
        int game = inputs.game_fd, settings = inputs.settings_fd;
        skyrim_mods_release_papyrus_inputs(&inputs);
        assert(fcntl(game, F_GETFD) == -1 && errno == EBADF);
        assert(fcntl(settings, F_GETFD) == -1 && errno == EBADF);
    }
    assert(fcntl(root, F_GETFD) >= 0 && close(root) == 0);
    skyrim_mods_release_papyrus_inputs(&inputs);
    require_empty(&inputs);
    puts("");
    return 0;
}
