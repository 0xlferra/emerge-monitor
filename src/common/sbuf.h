/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EMON_SBUF_H
#define EMON_SBUF_H

#include <stddef.h>

/* Growable string buffer. Allocation failures abort the process. */
typedef struct {
	char *data;
	size_t len;
	size_t cap;
} sbuf;

void sb_init(sbuf *sb);
void sb_free(sbuf *sb);
void sb_reset(sbuf *sb);
void sb_append(sbuf *sb, const char *s, size_t n);
void sb_puts(sbuf *sb, const char *s);
void sb_printf(sbuf *sb, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* Append s as a quoted JSON string, or `null` if s is NULL. */
void sb_json_str(sbuf *sb, const char *s);

#endif
