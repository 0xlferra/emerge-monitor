/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "store.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "../common/emerge_log.h"

/* A host is "online" if its agent reported within this many seconds. */
#define ONLINE_WINDOW 180
/* Build-time estimates average the last N successful builds of a package. */
#define ETA_SAMPLES 5

#define STR_(x) #x
#define STR(x) STR_(x)

struct store {
	sqlite3 *db;
	int err;
};

static const char SCHEMA[] =
	"PRAGMA journal_mode=WAL;"
	"PRAGMA foreign_keys=ON;"
	"CREATE TABLE IF NOT EXISTS hosts("
	"  id INTEGER PRIMARY KEY,"
	"  name TEXT UNIQUE NOT NULL,"
	"  last_seen INTEGER,"
	"  last_event INTEGER,"
	"  last_sync INTEGER,"
	"  log_inode INTEGER,"
	"  log_offset INTEGER NOT NULL DEFAULT 0);"
	"CREATE TABLE IF NOT EXISTS sessions("
	"  id INTEGER PRIMARY KEY,"
	"  host_id INTEGER NOT NULL REFERENCES hosts(id),"
	"  start_ts INTEGER NOT NULL,"
	"  end_ts INTEGER,"
	"  command TEXT,"
	"  status TEXT NOT NULL DEFAULT 'running',"
	"  exit_code INTEGER,"
	"  pkg_total INTEGER NOT NULL DEFAULT 0);"
	"CREATE INDEX IF NOT EXISTS sessions_host ON sessions(host_id, end_ts);"
	"CREATE TABLE IF NOT EXISTS packages("
	"  id INTEGER PRIMARY KEY,"
	"  session_id INTEGER NOT NULL REFERENCES sessions(id),"
	"  atom TEXT NOT NULL,"
	"  pn TEXT NOT NULL,"
	"  seq INTEGER,"
	"  total INTEGER,"
	"  start_ts INTEGER NOT NULL,"
	"  end_ts INTEGER,"
	"  status TEXT NOT NULL DEFAULT 'building');"
	"CREATE INDEX IF NOT EXISTS packages_session ON packages(session_id);"
	"CREATE INDEX IF NOT EXISTS packages_pn ON packages(pn, status);";

/*
 * Prepare `sql` and bind arguments described by `fmt`:
 *   i  long long
 *   I  long long, bound as NULL when negative
 *   t  const char * (NULL binds NULL)
 */
static sqlite3_stmt *vprep(store *s, const char *sql, const char *fmt,
			   va_list ap)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "sqlite: %s: %s\n", sqlite3_errmsg(s->db), sql);
		s->err = 1;
		return NULL;
	}
	for (int i = 0; fmt && fmt[i]; i++) {
		long long v;
		const char *t;
		switch (fmt[i]) {
		case 'i':
			sqlite3_bind_int64(st, i + 1, va_arg(ap, long long));
			break;
		case 'I':
			v = va_arg(ap, long long);
			if (v < 0)
				sqlite3_bind_null(st, i + 1);
			else
				sqlite3_bind_int64(st, i + 1, v);
			break;
		case 't':
			t = va_arg(ap, const char *);
			if (t)
				sqlite3_bind_text(st, i + 1, t, -1, SQLITE_TRANSIENT);
			else
				sqlite3_bind_null(st, i + 1);
			break;
		default:
			abort();
		}
	}
	return st;
}

static sqlite3_stmt *prep(store *s, const char *sql, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sqlite3_stmt *st = vprep(s, sql, fmt, ap);
	va_end(ap);
	return st;
}

static int q_exec(store *s, const char *sql, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sqlite3_stmt *st = vprep(s, sql, fmt, ap);
	va_end(ap);
	if (!st)
		return -1;
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW)
		;
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE) {
		fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(s->db));
		s->err = 1;
		return -1;
	}
	return 0;
}

/* First column of the first row, or `dflt` if there is no row or it is NULL. */
static long long q_int(store *s, long long dflt, const char *sql,
		       const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sqlite3_stmt *st = vprep(s, sql, fmt, ap);
	va_end(ap);
	if (!st)
		return dflt;
	long long v = dflt;
	int rc = sqlite3_step(st);
	if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
		v = sqlite3_column_int64(st, 0);
	else if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		s->err = 1;
	sqlite3_finalize(st);
	return v;
}

store *store_open(const char *path)
{
	store *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	if (sqlite3_open(path, &s->db) != SQLITE_OK) {
		fprintf(stderr, "sqlite: cannot open %s: %s\n", path,
			sqlite3_errmsg(s->db));
		goto fail;
	}
	sqlite3_busy_timeout(s->db, 5000);
	char *msg = NULL;
	if (sqlite3_exec(s->db, SCHEMA, NULL, NULL, &msg) != SQLITE_OK) {
		fprintf(stderr, "sqlite: schema: %s\n", msg);
		sqlite3_free(msg);
		goto fail;
	}
	return s;
fail:
	sqlite3_close(s->db);
	free(s);
	return NULL;
}

void store_close(store *s)
{
	if (!s)
		return;
	sqlite3_close(s->db);
	free(s);
}

/* ---------------------------------------------------------------- ingest */

static void close_session(store *s, long long sid, long long ts,
			  const char *status, long long exit_code)
{
	q_exec(s, "UPDATE sessions SET end_ts=?, status=?, exit_code=? WHERE id=?",
	       "itIi", ts, status, exit_code, sid);
	/* Whatever was still building when the session ended did not finish. */
	q_exec(s, "UPDATE packages SET end_ts=?, status=? "
		  "WHERE session_id=? AND end_ts IS NULL",
	       "iti", ts, strcmp(status, "failed") == 0 ? "failed" : "aborted",
	       sid);
}

static void apply_event(store *s, long long hid, const el_event *ev)
{
	char pn[256];
	long long open = q_int(s, -1,
		"SELECT id FROM sessions WHERE host_id=? AND end_ts IS NULL "
		"ORDER BY id DESC LIMIT 1", "i", hid);

	q_exec(s, "UPDATE hosts SET last_event=MAX(COALESCE(last_event,0),?) "
		  "WHERE id=?", "ii", ev->ts, hid);

	switch (ev->type) {
	case EL_SESSION_START:
		if (open >= 0)
			close_session(s, open, ev->ts, "interrupted", -1);
		q_exec(s, "INSERT INTO sessions(host_id,start_ts,command) "
			  "VALUES(?,?,?)", "iit", hid, ev->ts, ev->text);
		break;

	case EL_PKG_START:
		if (open < 0) {
			/* Log started mid-session (e.g. rotated): synthesise one. */
			q_exec(s, "INSERT INTO sessions(host_id,start_ts) VALUES(?,?)",
			       "ii", hid, ev->ts);
			open = sqlite3_last_insert_rowid(s->db);
		}
		el_atom_pn(ev->atom, pn, sizeof(pn));
		q_exec(s, "INSERT INTO packages(session_id,atom,pn,seq,total,start_ts) "
			  "VALUES(?,?,?,?,?,?)", "ittiii",
		       open, ev->atom, pn, (long long)ev->seq,
		       (long long)ev->total, ev->ts);
		q_exec(s, "UPDATE sessions SET pkg_total=MAX(pkg_total,?) WHERE id=?",
		       "ii", (long long)ev->total, open);
		break;

	case EL_PKG_DONE:
		if (open < 0)
			break;
		q_exec(s, "UPDATE packages SET end_ts=?, status='done' WHERE id=("
			  " SELECT id FROM packages WHERE session_id=? AND atom=?"
			  " AND end_ts IS NULL ORDER BY id DESC LIMIT 1)",
		       "iit", ev->ts, open, ev->atom);
		break;

	case EL_SESSION_END:
		if (open >= 0)
			close_session(s, open, ev->ts,
				      ev->exit_code == 0 ? "success" : "failed",
				      ev->exit_code);
		break;

	case EL_TERMINATING:
		/* Normally preceded by "exiting ...", so only matters on ^C. */
		if (open >= 0)
			close_session(s, open, ev->ts, "interrupted", -1);
		break;

	case EL_SYNC_DONE:
		q_exec(s, "UPDATE hosts SET last_sync=MAX(COALESCE(last_sync,0),?) "
			  "WHERE id=?", "ii", ev->ts, hid);
		break;

	case EL_NONE:
		break;
	}
}

int store_ingest(store *s, const char *host, unsigned long long inode,
		 long long offset, const char *data, size_t len, long long now)
{
	int applied = 0;

	s->err = 0;
	if (q_exec(s, "BEGIN IMMEDIATE", NULL) < 0)
		return -1;

	q_exec(s, "INSERT OR IGNORE INTO hosts(name) VALUES(?)", "t", host);
	long long hid = q_int(s, -1, "SELECT id FROM hosts WHERE name=?", "t", host);
	long long cur_inode = q_int(s, -1,
		"SELECT log_inode FROM hosts WHERE id=?", "i", hid);
	long long cur_off = q_int(s, 0,
		"SELECT log_offset FROM hosts WHERE id=?", "i", hid);

	size_t skip = 0;
	int same_file = cur_inode == (long long)inode;
	if (offset >= 0 && len > 0 && same_file && offset < cur_off) {
		/* Agent re-sent data we already have; both ends are line breaks. */
		skip = (size_t)(cur_off - offset);
		if (skip > len)
			skip = len;
	}

	const char *p = data + skip, *end = data + len;
	while (p < end && !s->err) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
		el_event ev;
		if (el_parse_line(p, n, &ev)) {
			apply_event(s, hid, &ev);
			applied++;
		}
		p += n + (nl ? 1 : 0);
	}

	q_exec(s, "UPDATE hosts SET last_seen=? WHERE id=?", "ii", now, hid);
	if (offset >= 0 && len > 0 &&
	    (!same_file || offset + (long long)len > cur_off))
		q_exec(s, "UPDATE hosts SET log_inode=?, log_offset=? WHERE id=?",
		       "iii", (long long)inode, offset + (long long)len, hid);

	if (s->err) {
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
		return -1;
	}
	if (q_exec(s, "COMMIT", NULL) < 0) {
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
		return -1;
	}
	return applied;
}

/* ------------------------------------------------------------------ JSON */

static void json_col(sbuf *out, sqlite3_stmt *st, int col)
{
	switch (sqlite3_column_type(st, col)) {
	case SQLITE_NULL:
		sb_puts(out, "null");
		break;
	case SQLITE_INTEGER:
		sb_printf(out, "%lld", (long long)sqlite3_column_int64(st, col));
		break;
	case SQLITE_FLOAT:
		sb_printf(out, "%lld", (long long)sqlite3_column_double(st, col));
		break;
	default:
		sb_json_str(out, (const char *)sqlite3_column_text(st, col));
	}
}

/* Emit the current row as `"name":value,...` using the column names. */
static void json_fields(sbuf *out, sqlite3_stmt *st, int first_col)
{
	int n = sqlite3_column_count(st);
	for (int i = first_col; i < n; i++) {
		if (i > first_col)
			sb_puts(out, ",");
		sb_json_str(out, sqlite3_column_name(st, i));
		sb_puts(out, ":");
		json_col(out, st, i);
	}
}

static long long avg_duration(store *s, long long hid, const char *pn)
{
	return q_int(s, -1,
		"SELECT CAST(AVG(d) AS INTEGER) FROM ("
		" SELECT p.end_ts - p.start_ts AS d FROM packages p"
		" JOIN sessions s ON s.id = p.session_id"
		" WHERE s.host_id=? AND p.pn=? AND p.status='done'"
		" ORDER BY p.id DESC LIMIT " STR(ETA_SAMPLES) ")",
		"it", hid, pn);
}

static void json_running(store *s, sbuf *out, long long hid, long long now)
{
	sqlite3_stmt *st = prep(s,
		"SELECT s.id, s.start_ts, s.command, s.pkg_total,"
		" (SELECT COUNT(*) FROM packages p WHERE p.session_id=s.id"
		"  AND p.status='done') AS pkg_done "
		"FROM sessions s WHERE s.host_id=? AND s.end_ts IS NULL "
		"ORDER BY s.id DESC LIMIT 1", "i", hid);
	if (!st || sqlite3_step(st) != SQLITE_ROW) {
		sb_puts(out, "null");
		sqlite3_finalize(st);
		return;
	}
	long long sid = sqlite3_column_int64(st, 0);
	sb_puts(out, "{");
	json_fields(out, st, 0);
	sqlite3_finalize(st);

	sb_puts(out, ",\"building\":[");
	st = prep(s, "SELECT atom, pn, seq, total, start_ts FROM packages "
		     "WHERE session_id=? AND status='building' ORDER BY id",
		  "i", sid);
	for (int i = 0; st && sqlite3_step(st) == SQLITE_ROW; i++) {
		const char *pn = (const char *)sqlite3_column_text(st, 1);
		long long start = sqlite3_column_int64(st, 4);
		long long avg = avg_duration(s, hid, pn);
		sb_puts(out, i ? ",{" : "{");
		json_fields(out, st, 0);
		sb_printf(out, ",\"elapsed\":%lld", now - start);
		if (avg >= 0)
			sb_printf(out, ",\"avg_duration\":%lld", avg);
		else
			sb_puts(out, ",\"avg_duration\":null");
		sb_puts(out, "}");
	}
	sqlite3_finalize(st);
	sb_puts(out, "]}");
}

int store_json_hosts(store *s, sbuf *out, long long now)
{
	s->err = 0;
	sqlite3_stmt *st = prep(s,
		"SELECT id, name, last_seen, last_event, last_sync FROM hosts "
		"ORDER BY name", NULL);
	if (!st)
		return -1;

	sb_printf(out, "{\"now\":%lld,\"hosts\":[", now);
	for (int i = 0; sqlite3_step(st) == SQLITE_ROW; i++) {
		long long hid = sqlite3_column_int64(st, 0);
		long long seen = sqlite3_column_int64(st, 2);
		sb_puts(out, i ? ",{" : "{");
		json_fields(out, st, 1);
		sb_printf(out, ",\"online\":%s",
			  now - seen <= ONLINE_WINDOW ? "true" : "false");

		sb_puts(out, ",\"current\":");
		json_running(s, out, hid, now);

		sb_puts(out, ",\"last\":");
		sqlite3_stmt *ls = prep(s,
			"SELECT id, start_ts, end_ts, command, status, exit_code "
			"FROM sessions WHERE host_id=? AND end_ts IS NOT NULL "
			"ORDER BY end_ts DESC, id DESC LIMIT 1", "i", hid);
		if (ls && sqlite3_step(ls) == SQLITE_ROW) {
			sb_puts(out, "{");
			json_fields(out, ls, 0);
			sb_puts(out, "}");
		} else {
			sb_puts(out, "null");
		}
		sqlite3_finalize(ls);
		sb_puts(out, "}");
	}
	sqlite3_finalize(st);
	sb_puts(out, "]}");
	return s->err ? -1 : 0;
}

int store_json_sessions(store *s, const char *host, int limit, sbuf *out)
{
	s->err = 0;
	sqlite3_stmt *st = prep(s,
		"SELECT s.id, s.start_ts, s.end_ts, s.command, s.status,"
		" s.exit_code, s.pkg_total,"
		" (SELECT COUNT(*) FROM packages p WHERE p.session_id=s.id"
		"  AND p.status='done') AS pkg_done,"
		" (SELECT COUNT(*) FROM packages p WHERE p.session_id=s.id"
		"  AND p.status='failed') AS pkg_failed "
		"FROM sessions s JOIN hosts h ON h.id=s.host_id "
		"WHERE h.name=? ORDER BY s.id DESC LIMIT ?",
		"ti", host, (long long)limit);
	if (!st)
		return -1;
	sb_puts(out, "{\"sessions\":[");
	for (int i = 0; sqlite3_step(st) == SQLITE_ROW; i++) {
		sb_puts(out, i ? ",{" : "{");
		json_fields(out, st, 0);
		sb_puts(out, "}");
	}
	sqlite3_finalize(st);
	sb_puts(out, "]}");
	return s->err ? -1 : 0;
}

int store_json_packages(store *s, long long session_id, sbuf *out)
{
	s->err = 0;
	long long hid = q_int(s, -1, "SELECT host_id FROM sessions WHERE id=?",
			      "i", session_id);
	sqlite3_stmt *st = prep(s,
		"SELECT pn, atom, seq, total, start_ts, end_ts, status "
		"FROM packages WHERE session_id=? ORDER BY id", "i", session_id);
	if (!st)
		return -1;
	sb_puts(out, "{\"packages\":[");
	for (int i = 0; sqlite3_step(st) == SQLITE_ROW; i++) {
		const char *pn = (const char *)sqlite3_column_text(st, 0);
		long long avg = avg_duration(s, hid, pn);
		sb_puts(out, i ? ",{" : "{");
		json_fields(out, st, 1);
		if (avg >= 0)
			sb_printf(out, ",\"avg_duration\":%lld", avg);
		else
			sb_puts(out, ",\"avg_duration\":null");
		sb_puts(out, "}");
	}
	sqlite3_finalize(st);
	sb_puts(out, "]}");
	return s->err ? -1 : 0;
}
