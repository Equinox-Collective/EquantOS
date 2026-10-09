// src/kernel/fs/procfs.h - Linux-compatible /proc pseudo filesystem
#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Resolve /proc magic links (/proc/<pid>/exe, /proc/<pid>/cwd, /proc/<pid>/fd/<n>).
 * @return Length written to buf, or a negative errno if the path is not a procfs link.
 */
int64_t procfs_readlink(const char *path, char *buf, size_t bufsiz);

#endif // PROCFS_H
