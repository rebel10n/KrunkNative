#pragma once
// Small cross-platform file system layer for the launcher (UTF-8 paths everywhere; wide-char
// Win32 calls underneath on Windows). '/' works as the separator on every platform.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

FILE *kn_fs_fopen(const char *path, const char *mode); // _wfopen on Windows

int kn_fs_exists(const char *path);
int kn_fs_is_dir(const char *path);
int64_t kn_fs_size(const char *path);           // -1 = missing / not a regular file
int kn_fs_mkdirs(const char *path);             // mkdir -p; 1 ok
int kn_fs_mkdirs_parent(const char *file_path); // the parent directory of a file
int kn_fs_rmtree(const char *path);             // rm -rf; 1 ok (also when missing)
int kn_fs_remove(const char *path);             // one file or empty dir; 1 ok (also when missing)
// rename(2) replacing an existing file (MoveFileEx MOVEFILE_REPLACE_EXISTING on Windows; a
// running .exe/.dll may be the source but not the replaced target there). 1 ok.
int kn_fs_rename(const char *from, const char *to);
int kn_fs_dir_writable(const char *dir);        // creates and deletes a probe file
int kn_fs_read(const char *path, char **data, size_t *len, size_t max); // NUL-terminated
int kn_fs_write_atomic(const char *path, const void *data, size_t len); // tmp + rename
int kn_fs_sha256(const char *path, char out_hex[65]); // 1 ok
int kn_fs_set_executable(const char *path, int on);   // chmod +x (no-op on Windows)
int kn_fs_symlink(const char *target, const char *link_path); // POSIX only (0 on Windows)
int kn_fs_is_symlink(const char *path);
// Recursive walk below root: fn(rel_path, is_dir, user) in pre-order (directories before
// their content; symlinks are not followed and reported as files). 0 from fn stops (returns 0).
typedef int (*KnFsWalkFn)(const char *rel, int is_dir, void *user);
int kn_fs_walk(const char *root, KnFsWalkFn fn, void *user);

// The running executable (symlinks resolved), its directory.
int kn_fs_self_exe(char *out, size_t n);
void kn_fs_dirname(char *path); // in place, no trailing '/'
// a + "/" + b into out. 1 when it fit.
int kn_fs_join(char *out, size_t n, const char *a, const char *b);
// Absolute, normalised ("x/../y" collapsed) version of path. 1 ok.
int kn_fs_absolute(const char *path, char *out, size_t n);
