/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Where a MOD archive keeps its game data. Scores every directory prefix (up
 * to four levels) by entries that look like Skyrim Data content (textures/,
 * meshes/, scripts/, *.esp, *.bsa, ...) and returns the best one, shortest on
 * ties. Returns 1 and the prefix ("" or "Folder/Data/") on success, 0 when no
 * game data was found. *installer is set when fomod/ModuleConfig.xml exists. */
int skyrim_mod_data_root(const char *const *paths, size_t count,
                         char *prefix, size_t capacity, int *installer);

/* Build the launch view: a copy-on-write clone of the original game folder
 * with enabled MOD packages laid over its Data folder, later packages winning.
 * On Apple platforms files are cloned (clonefile), so the view costs almost
 * no space and writes to it never reach the original. Elsewhere hard links or
 * copies are used (tests only).
 *
 * Built in view_dir + ".building" and swapped in by rename; the original game
 * folder and packages are only read. Original *.esm / *.bsa / executable
 * files are never replaced. Each replacement is appended to conflicts_path
 * as "relative-path<TAB>previous-owner<TAB>new-owner\n" (owner -1 = original
 * game, otherwise the package index). key is stored in the view so an
 * unchanged selection can be reused. Returns 0 or an errno-style code; err
 * receives a short description. */
int skyrim_deploy_build(const char *game_dir, const char *const *package_roots, size_t count,
                        const char *view_dir, const char *conflicts_path, const char *key,
                        char *err, size_t err_capacity);
/* 1 when view_dir holds a complete view built with exactly this key. */
int skyrim_deploy_matches(const char *view_dir, const char *key);
/* Stable 64-bit signature of a folder tree (names, sizes, mtimes), for the key. */
int skyrim_tree_signature(const char *dir, uint64_t *signature);
/* Recursive delete that never follows symlinks. 0 on success or absent. */
int skyrim_remove_tree(const char *path);
/* Copy-on-write clone where available, else hard link, else byte copy. */
int skyrim_clone_file(const char *source, const char *destination);
#ifdef __cplusplus
}
#endif
