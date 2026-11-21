/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Where a MOD archive keeps its game data. Scores every directory prefix (up
 * to four levels) by entries that look like Skyrim Data content (textures/,
 * meshes/, scripts/, *.esp, *.bsa, ...) case-insensitively. Returns the best
 * score, shortest on ties. Marked Data takes precedence over a sibling src
 * directory or a wrapper that only contains tool-related markers.
 * Returns 1 and the prefix ("" or "Folder/Data/") on success, 0 when no
 * game data was found. *installer is set when fomod/ModuleConfig.xml exists. */
int skyrim_mod_data_root(const char *const *paths, size_t count,
                         char *prefix, size_t capacity, int *installer);

/* Build the launch view: a copy-on-write clone of the original game folder
 * with enabled MOD packages laid over its Data folder, later packages winning.
 * On Apple platforms files are cloned (clonefile), so the view costs almost
 * no space and writes to it never reach the original. Elsewhere independent
 * byte copies are used (tests only).
 *
 * Built in view_dir + ".building" and swapped in by rename; the original game
 * folder and packages are only read. Original *.esm / *.bsa / executable
 * files are never replaced. The view and conflict report carry the same key
 * so a complete generation can be reused. After the report header, each
 * replacement is "relative-path<TAB>losing-owner<TAB>winning-owner\n"
 * (owner -1 = original game, otherwise the package index).
 * Returns 0 or an errno-style code; err receives a short description. */
int skyrim_deploy_build(const char *game_dir, const char *const *package_roots, size_t count,
                        const char *view_dir, const char *conflicts_path, const char *key,
                        char *err, size_t err_capacity);
/* 1 when the view and optional report have the same build key. A NULL key
 * accepts any coherent generation, for recovery of an interrupted swap. */
int skyrim_deploy_matches(const char *view_dir, const char *conflicts_path, const char *key);
/* Stable 64-bit signature of a folder tree (names, sizes, mtimes), for the key. */
int skyrim_tree_signature(const char *dir, uint64_t *signature);
/* Recursive delete that never follows symlinks. 0 on success or absent. */
int skyrim_remove_tree(const char *path);
/* Copy-on-write clone where available, else an independent byte copy. */
int skyrim_clone_file(const char *source, const char *destination);
/* Copy a selected .jslot into the CharGen preset directory of a fresh staged
 * package Data folder, leaving both source and existing files intact. */
int skyrim_mod_stage_racemenu_preset(const char *source, const char *name, const char *data_dir);
/* Publish an app-owned Inbox snapshot of an ECE 1–40 .slot to an open profile
 * root on the same volume. No overwrite. The caller removes its Inbox snapshot
 * after the attempt. The original file chosen by the user is never linked.
 * slot receives the valid filename's number, or 0 for an unrecognized name. */
int skyrim_mod_publish_ece_slot(int profile, const char *staged, const char *name, unsigned *slot);
#ifdef __cplusplus
}
#endif
