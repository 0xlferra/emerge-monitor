/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EMON_EMERGE_LOG_H
#define EMON_EMERGE_LOG_H

#include <stddef.h>

/* Events recognised in /var/log/emerge.log. */
typedef enum {
	EL_NONE = 0,
	EL_SESSION_START,  /* "*** emerge --ask --update @world"            */
	EL_PKG_START,      /* ">>> emerge (1 of 5) cat/pkg-1.0 to /"         */
	EL_PKG_DONE,       /* "::: completed emerge (1 of 5) cat/pkg-1.0 to /" */
	EL_SESSION_END,    /* "*** exiting successfully." / "unsuccessfully" */
	EL_TERMINATING,    /* "*** terminating."                              */
	EL_SYNC_DONE,      /* "=== Sync completed for gentoo"                 */
} el_type;

typedef struct {
	el_type type;
	long long ts;          /* unix timestamp of the log line            */
	int seq;               /* EL_PKG_*: position in the merge list      */
	int total;             /* EL_PKG_*: length of the merge list        */
	int exit_code;         /* EL_SESSION_END: 0 on success              */
	char atom[256];        /* EL_PKG_*: cat/pkg-version                 */
	char text[1024];       /* EL_SESSION_START: command, EL_SYNC_DONE: repo */
} el_event;

/* Parse one log line (trailing newline optional). Returns 1 and fills ev
 * if the line is an event we care about, 0 otherwise. */
int el_parse_line(const char *line, size_t len, el_event *ev);

/* Strip version (and revision / ::repo) from cat/pkg-ver, giving cat/pkg. */
void el_atom_pn(const char *atom, char *out, size_t outlen);

#endif
