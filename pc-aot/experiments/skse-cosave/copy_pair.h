/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* Copy one selected .ess and its optional, opaque same-stem .skse into an
 * existing distinct profile directory. All descriptors are borrowed. The
 * source file descriptors must be read-only and identify nonempty, singly
 * linked regular files in source_dir. Use side_fd == -1 only when the .skse is
 * absent. Source offsets are preserved.
 *
 * The caller owns both directories and serializes destination writers. The
 * source pair's provenance belongs to the caller. This function copies bytes
 * without interpreting the save formats. It refuses ASCII-case collisions
 * with either extension and publishes the .ess last, after syncing both files.
 *
 * Returns zero or an errno value (EEXIST for a collision, ESTALE for a changed
 * source, EINVAL for invalid arguments, EIO if rollback could not finish).
 * After process interruption, the same explicit selection can finish an
 * app-owned pair when its private stage and published .skse carry matching
 * proof and both copies match the stable source. Unverified leftovers return
 * EEXIST. This is process recovery and makes no power-loss durability promise. */
int skse_copy_save_pair(int source_dir, int base_fd, int side_fd,
                        int destination_dir, const char *selected_ess);

/* The app supplies vetted relative save-folder paths within an already opened
 * profile root. Walk directory components without following links, open the
 * selected save and its discovered companion read-only, and create missing
 * destination directories. copied_side is 0 or 1 on success, 0 on error. */
int skse_copy_named_save(int profile_root, const char *source_path, const char *destination_path,
                         const char *selected_ess, int *copied_side);

#ifdef __cplusplus
}
#endif
