/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "buildlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/util.h"

#define BL_MAX 64

typedef struct {
	char host[HOSTNAME_MAX + 1];
	char atom[256];
	unsigned long long inode;
	long long base;      /* file offset of buf[0] */
	char *buf;           /* NULL: slot unused */
	size_t len;
	long long updated;
} blog;

static blog logs[BL_MAX];

static void drop(blog *b)
{
	free(b->buf);
	memset(b, 0, sizeof(*b));
}

static blog *find(const char *host, const char *atom)
{
	for (int i = 0; i < BL_MAX; i++)
		if (logs[i].buf && !strcmp(logs[i].host, host) &&
		    !strcmp(logs[i].atom, atom))
			return &logs[i];
	return NULL;
}

static blog *create(const char *host, const char *atom, long long now)
{
	blog *slot = NULL;
	for (int i = 0; i < BL_MAX; i++) {
		blog *b = &logs[i];
		if (b->buf && b->updated < now - BL_TTL)
			drop(b);
		if (!b->buf) {
			if (!slot || slot->buf)
				slot = b;
		} else if (!slot || (slot->buf && b->updated < slot->updated)) {
			slot = b; /* oldest so far, evicted if nothing is free */
		}
	}
	drop(slot);
	slot->buf = malloc(BL_CAP);
	if (!slot->buf)
		abort();
	snprintf(slot->host, sizeof(slot->host), "%s", host);
	snprintf(slot->atom, sizeof(slot->atom), "%s", atom);
	slot->base = -1;
	return slot;
}

void bl_append(const char *host, const char *atom, unsigned long long inode,
	       long long offset, const char *data, size_t len, long long now)
{
	blog *b = find(host, atom);
	if (!b)
		b = create(host, atom, now);
	b->updated = now;

	long long end = b->base + (long long)b->len;
	if (b->base < 0 || b->inode != inode || offset > end) {
		/* new file, or a gap we cannot fill: restart at offset */
		b->inode = inode;
		b->base = offset;
		b->len = 0;
	} else if (offset < end) {
		/* overlap with what we have (agent restarted): skip it */
		size_t skip = (size_t)(end - offset);
		if (skip >= len)
			return;
		data += skip;
		len -= skip;
	}

	if (b->len + len > BL_CAP) {
		size_t excess = b->len + len - BL_CAP;
		if (excess >= b->len) {
			size_t from_new = excess - b->len;
			b->base += (long long)(b->len + from_new);
			b->len = 0;
			data += from_new;
			len -= from_new;
		} else {
			memmove(b->buf, b->buf + excess, b->len - excess);
			b->len -= excess;
			b->base += (long long)excess;
		}
	}
	memcpy(b->buf + b->len, data, len);
	b->len += len;
}

int bl_read(const char *host, const char *atom, long long from,
	    const char **data, size_t *len, long long *start, long long *end)
{
	blog *b = find(host, atom);
	if (!b)
		return -1;
	*end = b->base + (long long)b->len;
	if (from < b->base || from > *end)
		from = b->base;
	*start = from;
	*data = b->buf + (from - b->base);
	*len = (size_t)(*end - from);
	return 0;
}
