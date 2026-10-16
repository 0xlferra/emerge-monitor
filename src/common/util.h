/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EMON_UTIL_H
#define EMON_UTIL_H

#include <stddef.h>

#ifndef EMON_DEFAULT_DB
#define EMON_DEFAULT_DB "/var/lib/emerge-monitor/emon.db"
#endif
#ifndef EMON_DEFAULT_STATE
#define EMON_DEFAULT_STATE "/var/lib/emerge-monitor/agent.state"
#endif

#define HOSTNAME_MAX 64

/* write(2) until everything is written. Returns 0 or -1. */
int write_all(int fd, const void *buf, size_t len);

/* Compare two NUL-terminated secrets without early exit. */
int const_time_eq(const char *a, const char *b);

/* Host names are 1..HOSTNAME_MAX chars of [A-Za-z0-9._-]. */
int valid_hostname(const char *s);

/* Read a token from the first line of `path`, trimming whitespace. */
int read_token(const char *path, char *out, size_t outlen);

#endif
