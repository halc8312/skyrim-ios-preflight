#ifndef SKYRIM_CLASSIC_SKSE_TRANSLATION_LIST_H
#define SKYRIM_CLASSIC_SKSE_TRANSLATION_LIST_H

#include "mod_plugins.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* g_save_dir is the profile passed to the guest. Its existing list may use
 * Windows capitalization, with or without a UTF-8 byte-order mark. */
static inline FILE *skyrim_skse_open_plugin_list(const char *profile) {
    char docs[PATH_MAX], folder[PATH_MAX], path[PATH_MAX];
    struct stat st;
    if (!profile || !*profile ||
        skyrim_plugin_child_path(profile, "Documents", docs, sizeof docs) != 1 ||
        lstat(docs, &st) || !S_ISDIR(st.st_mode) ||
        skyrim_plugin_child_path(docs, "Skyrim", folder, sizeof folder) != 1 ||
        lstat(folder, &st) || !S_ISDIR(st.st_mode) ||
        skyrim_plugin_child_path(folder, "plugins.txt", path, sizeof path) != 1)
        return NULL;

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 1024 * 1024) {
        close(fd);
        return NULL;
    }
    FILE *list = fdopen(fd, "rb");
    if (!list) { close(fd); return NULL; }
    unsigned char prefix[3];
    if (fread(prefix, 1, sizeof prefix, list) != sizeof prefix ||
        memcmp(prefix, "\xef\xbb\xbf", sizeof prefix) != 0) {
        if (ferror(list) || fseek(list, 0, SEEK_SET)) {
            fclose(list);
            return NULL;
        }
    }
    return list;
}

#endif
