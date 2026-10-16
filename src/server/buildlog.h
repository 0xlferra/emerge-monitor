/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EMON_BUILDLOG_H
#define EMON_BUILDLOG_H

#include <stddef.h>

/*
 * In-memory tails of the build.log files agents are following, keyed by
 * (host, atom). Only the last BL_CAP bytes of each log are kept, and logs
 * not updated for BL_TTL seconds are dropped: this is for watching builds
 * live, not for archiving them.
 */
#define BL_CAP (512 * 1024)
#define BL_TTL 3600

/* Add `len` bytes that start at byte `offset` of the agent's file. A new
 * `inode` means the file was recreated (a rebuild) and restarts the log. */
void bl_append(const char *host, const char *atom, unsigned long long inode,
	       long long offset, const char *data, size_t len, long long now);

/*
 * Get what is buffered from file offset `from` on. If `from` is outside the
 * buffered range the whole buffer is returned instead. On return the data
 * spans file offsets [*start, *end). Returns -1 if the log is unknown.
 */
int bl_read(const char *host, const char *atom, long long from,
	    const char **data, size_t *len, long long *start, long long *end);

#endif
