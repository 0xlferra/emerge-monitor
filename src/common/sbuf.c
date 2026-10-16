/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sb_grow(sbuf *sb, size_t extra)
{
	size_t need = sb->len + extra + 1;
	if (need <= sb->cap)
		return;
	size_t cap = sb->cap ? sb->cap : 256;
	while (cap < need)
		cap *= 2;
	char *p = realloc(sb->data, cap);
	if (!p)
		abort();
	sb->data = p;
	sb->cap = cap;
}

void sb_init(sbuf *sb)
{
	sb->data = NULL;
	sb->len = sb->cap = 0;
	sb_grow(sb, 0);
	sb->data[0] = '\0';
}

void sb_free(sbuf *sb)
{
	free(sb->data);
	sb->data = NULL;
	sb->len = sb->cap = 0;
}

void sb_reset(sbuf *sb)
{
	sb->len = 0;
	if (sb->data)
		sb->data[0] = '\0';
}

void sb_append(sbuf *sb, const char *s, size_t n)
{
	sb_grow(sb, n);
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = '\0';
}

void sb_puts(sbuf *sb, const char *s)
{
	sb_append(sb, s, strlen(s));
}

void sb_printf(sbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	sb_grow(sb, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	sb->len += (size_t)n;
}

void sb_json_str(sbuf *sb, const char *s)
{
	if (!s) {
		sb_puts(sb, "null");
		return;
	}
	sb_append(sb, "\"", 1);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':  sb_puts(sb, "\\\""); break;
		case '\\': sb_puts(sb, "\\\\"); break;
		case '\n': sb_puts(sb, "\\n"); break;
		case '\r': sb_puts(sb, "\\r"); break;
		case '\t': sb_puts(sb, "\\t"); break;
		default:
			if (*p < 0x20)
				sb_printf(sb, "\\u%04x", *p);
			else
				sb_append(sb, (const char *)p, 1);
		}
	}
	sb_append(sb, "\"", 1);
}
