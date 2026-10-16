/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EMON_STORE_H
#define EMON_STORE_H

#include <stddef.h>

#include "../common/sbuf.h"

typedef struct store store;

store *store_open(const char *path);
void store_close(store *s);

/*
 * Apply a chunk of raw emerge.log lines sent by an agent.
 *
 * `inode`/`offset` identify where in the agent's log the chunk starts, so
 * chunks re-sent after an agent restart are de-duplicated. Pass offset < 0
 * to disable de-duplication. An empty chunk just refreshes last_seen.
 * Returns the number of events applied, or -1 on database error.
 */
int store_ingest(store *s, const char *host, unsigned long long inode,
		 long long offset, const char *data, size_t len, long long now);

/* Append a JSON array with the atoms currently building on `host`. */
int store_json_building(store *s, const char *host, sbuf *out);

/* JSON renderers for the HTTP API. Return 0 on success, -1 on error. */
int store_json_hosts(store *s, sbuf *out, long long now);
int store_json_sessions(store *s, const char *host, int limit, sbuf *out);
int store_json_packages(store *s, long long session_id, sbuf *out);

#endif
