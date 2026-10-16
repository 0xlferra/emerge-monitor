/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * emon-server: collects emerge.log data from emon-agent instances and serves
 * a small JSON API plus a web dashboard.
 *
 * The HTTP implementation is deliberately minimal (HTTP/1.0, one request per
 * connection, single threaded). Put it behind a reverse proxy for TLS.
 */
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../common/sbuf.h"
#include "../common/util.h"
#include "buildlog.h"
#include "store.h"

#define MAX_HEADER (16 * 1024)
#define MAX_BODY (8 * 1024 * 1024)
#define IO_TIMEOUT_SEC 15

extern const char dashboard_html[];

typedef struct {
	char method[8];
	char path[1024];
	char query[512];
	char auth[256];
	unsigned long long inode;
	long long offset;
	char *body;
	size_t body_len;
} request;

static volatile sig_atomic_t stop;
static const char *token;
static store *db;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void send_response_hdrs(int fd, int code, const char *ctype,
			       const char *extra, const char *body, size_t len)
{
	const char *reason = code == 200 ? "OK" :
			     code == 400 ? "Bad Request" :
			     code == 401 ? "Unauthorized" :
			     code == 404 ? "Not Found" :
			     code == 405 ? "Method Not Allowed" :
			     code == 413 ? "Payload Too Large" :
			     "Internal Server Error";
	sbuf h;
	sb_init(&h);
	sb_printf(&h, "HTTP/1.0 %d %s\r\n"
		      "Content-Type: %s\r\n"
		      "Content-Length: %zu\r\n"
		      "Cache-Control: no-store\r\n"
		      "X-Content-Type-Options: nosniff\r\n"
		      "Connection: close\r\n%s\r\n",
		  code, reason, ctype, len, extra ? extra : "");
	write_all(fd, h.data, h.len);
	write_all(fd, body, len);
	sb_free(&h);
}

static void send_response(int fd, int code, const char *ctype,
			  const char *body, size_t len)
{
	send_response_hdrs(fd, code, ctype, NULL, body, len);
}

static void send_error(int fd, int code, const char *msg)
{
	sbuf b;
	sb_init(&b);
	sb_puts(&b, "{\"error\":");
	sb_json_str(&b, msg);
	sb_puts(&b, "}\n");
	send_response(fd, code, "application/json", b.data, b.len);
	sb_free(&b);
}

static void send_json(int fd, sbuf *b)
{
	sb_puts(b, "\n");
	send_response(fd, 200, "application/json", b->data, b->len);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Look up `key` in an application/x-www-form-urlencoded query string. */
static int query_param(const char *query, const char *key, char *out,
		       size_t outlen)
{
	size_t klen = strlen(key);
	const char *p = query;
	while (p && *p) {
		const char *amp = strchr(p, '&');
		size_t n = amp ? (size_t)(amp - p) : strlen(p);
		if (n > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
			size_t o = 0;
			for (size_t i = klen + 1; i < n && o + 1 < outlen; i++) {
				int hi, lo;
				if (p[i] == '+') {
					out[o++] = ' ';
				} else if (p[i] == '%' && i + 2 < n &&
					   (hi = hexval(p[i + 1])) >= 0 &&
					   (lo = hexval(p[i + 2])) >= 0) {
					out[o++] = (char)(hi * 16 + lo);
					i += 2;
				} else {
					out[o++] = p[i];
				}
			}
			out[o] = '\0';
			return 1;
		}
		p = amp ? amp + 1 : NULL;
	}
	return 0;
}

static int read_request(int fd, request *rq, sbuf *raw, int *err)
{
	char buf[4096];
	char *hdr_end = NULL;

	memset(rq, 0, sizeof(*rq));
	rq->offset = -1;
	*err = 400;

	while (!hdr_end) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			return -1;
		sb_append(raw, buf, (size_t)n);
		hdr_end = strstr(raw->data, "\r\n\r\n");
		if (!hdr_end && raw->len > MAX_HEADER)
			return -1;
	}
	*hdr_end = '\0';
	size_t body_start = (size_t)(hdr_end - raw->data) + 4;

	char target[1024];
	if (sscanf(raw->data, "%7s %1023s", rq->method, target) != 2)
		return -1;
	char *q = strchr(target, '?');
	if (q) {
		*q = '\0';
		snprintf(rq->query, sizeof(rq->query), "%s", q + 1);
	}
	snprintf(rq->path, sizeof(rq->path), "%s", target);

	size_t content_length = 0;
	char *line = strstr(raw->data, "\r\n");
	while (line) {
		line += 2;
		char *next = strstr(line, "\r\n");
		if (next)
			*next = '\0';
		char *colon = strchr(line, ':');
		if (colon) {
			*colon = '\0';
			char *v = colon + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			if (!strcasecmp(line, "Content-Length"))
				content_length = strtoull(v, NULL, 10);
			else if (!strcasecmp(line, "Authorization"))
				snprintf(rq->auth, sizeof(rq->auth), "%s", v);
			else if (!strcasecmp(line, "X-Log-Inode"))
				rq->inode = strtoull(v, NULL, 10);
			else if (!strcasecmp(line, "X-Log-Offset"))
				rq->offset = strtoll(v, NULL, 10);
		}
		line = next;
	}

	if (content_length > MAX_BODY) {
		*err = 413;
		return -1;
	}
	while (raw->len - body_start < content_length) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			return -1;
		sb_append(raw, buf, (size_t)n);
	}
	rq->body = raw->data + body_start;
	rq->body_len = content_length;
	return 0;
}

static int authorized(const request *rq)
{
	if (!token)
		return 1;
	const char *p = rq->auth;
	if (strncasecmp(p, "Bearer ", 7) != 0)
		return 0;
	return const_time_eq(p + 7, token);
}

static void handle(int fd, const request *rq)
{
	sbuf out;
	char host[HOSTNAME_MAX + 1], atom[256], val[32];
	long long now = (long long)time(NULL);

	sb_init(&out);

	if (!strcmp(rq->path, "/") || !strcmp(rq->path, "/index.html")) {
		if (strcmp(rq->method, "GET"))
			send_error(fd, 405, "method not allowed");
		else
			send_response(fd, 200, "text/html; charset=utf-8",
				      dashboard_html, strlen(dashboard_html));

	} else if (!strcmp(rq->path, "/api/v1/ingest")) {
		if (strcmp(rq->method, "POST")) {
			send_error(fd, 405, "method not allowed");
		} else if (!authorized(rq)) {
			send_error(fd, 401, "bad or missing token");
		} else if (!query_param(rq->query, "host", host, sizeof(host)) ||
			   !valid_hostname(host)) {
			send_error(fd, 400, "missing or invalid host");
		} else {
			int n = store_ingest(db, host, rq->inode, rq->offset,
					     rq->body, rq->body_len, now);
			/* Tell the agent which build logs to follow. */
			sb_printf(&out, "{\"applied\":%d,\"building\":", n);
			if (n < 0 || store_json_building(db, host, &out) < 0) {
				send_error(fd, 500, "database error");
			} else {
				sb_puts(&out, "}");
				send_json(fd, &out);
			}
		}

	} else if (!strcmp(rq->path, "/api/v1/hosts")) {
		if (store_json_hosts(db, &out, now) < 0)
			send_error(fd, 500, "database error");
		else
			send_json(fd, &out);

	} else if (!strcmp(rq->path, "/api/v1/sessions")) {
		int limit = 50;
		if (query_param(rq->query, "limit", val, sizeof(val)))
			limit = atoi(val);
		if (limit < 1 || limit > 1000)
			limit = 50;
		if (!query_param(rq->query, "host", host, sizeof(host)))
			send_error(fd, 400, "missing host");
		else if (store_json_sessions(db, host, limit, &out) < 0)
			send_error(fd, 500, "database error");
		else
			send_json(fd, &out);

	} else if (!strcmp(rq->path, "/api/v1/buildlog")) {
		if (!query_param(rq->query, "host", host, sizeof(host)) ||
		    !valid_hostname(host) ||
		    !query_param(rq->query, "atom", atom, sizeof(atom)) ||
		    !valid_atom(atom)) {
			send_error(fd, 400, "missing or invalid host/atom");
		} else if (!strcmp(rq->method, "POST")) {
			if (!authorized(rq)) {
				send_error(fd, 401, "bad or missing token");
			} else {
				bl_append(host, atom, rq->inode,
					  rq->offset < 0 ? 0 : rq->offset,
					  rq->body, rq->body_len, now);
				sb_puts(&out, "{\"ok\":true}");
				send_json(fd, &out);
			}
		} else {
			long long from = 0, start, end;
			const char *data;
			size_t len;
			char hdrs[128];
			if (query_param(rq->query, "offset", val, sizeof(val)))
				from = atoll(val);
			if (bl_read(host, atom, from, &data, &len, &start, &end) < 0) {
				send_error(fd, 404, "no build log for this package");
			} else {
				snprintf(hdrs, sizeof(hdrs),
					 "X-Log-Start: %lld\r\nX-Log-End: %lld\r\n",
					 start, end);
				send_response_hdrs(fd, 200, "text/plain; charset=utf-8",
						   hdrs, data, len);
			}
		}

	} else if (!strcmp(rq->path, "/api/v1/packages")) {
		if (!query_param(rq->query, "session", val, sizeof(val)))
			send_error(fd, 400, "missing session");
		else if (store_json_packages(db, atoll(val), &out) < 0)
			send_error(fd, 500, "database error");
		else
			send_json(fd, &out);

	} else {
		send_error(fd, 404, "not found");
	}
	sb_free(&out);
}

static int listen_on(const char *addr)
{
	char host[256] = "", port[16] = "8080";
	const char *colon = strrchr(addr, ':');
	if (colon) {
		snprintf(host, sizeof(host), "%.*s", (int)(colon - addr), addr);
		snprintf(port, sizeof(port), "%s", colon + 1);
	} else {
		snprintf(port, sizeof(port), "%s", addr);
	}
	/* allow "[::]:8080" */
	if (host[0] == '[') {
		size_t n = strlen(host);
		memmove(host, host + 1, n);
		if (n >= 2 && host[n - 2] == ']')
			host[n - 2] = '\0';
	}

	struct addrinfo hints = {0}, *res, *ai;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
	if (rc) {
		fprintf(stderr, "emon-server: %s: %s\n", addr, gai_strerror(rc));
		return -1;
	}
	int fd = -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(fd, 64) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		fprintf(stderr, "emon-server: cannot listen on %s: %s\n", addr,
			strerror(errno));
	return fd;
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: emon-server [options]\n"
		"  -l, --listen ADDR       address to listen on (default 0.0.0.0:8080)\n"
		"  -d, --db PATH           SQLite database (default %s)\n"
		"  -t, --token-file PATH   file holding the agents' shared token\n"
		"                          (or set EMON_TOKEN; without a token ingest\n"
		"                          is open to anyone who can reach the port)\n"
		"  -h, --help              show this help\n",
		EMON_DEFAULT_DB);
}

int main(int argc, char **argv)
{
	const char *listen_addr = "0.0.0.0:8080";
	const char *db_path = EMON_DEFAULT_DB;
	const char *token_file = NULL;
	static const struct option opts[] = {
		{"listen", required_argument, NULL, 'l'},
		{"db", required_argument, NULL, 'd'},
		{"token-file", required_argument, NULL, 't'},
		{"help", no_argument, NULL, 'h'},
		{0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "l:d:t:h", opts, NULL)) != -1) {
		switch (c) {
		case 'l': listen_addr = optarg; break;
		case 'd': db_path = optarg; break;
		case 't': token_file = optarg; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	static char token_buf[256];
	if (token_file) {
		if (read_token(token_file, token_buf, sizeof(token_buf)) < 0)
			return 1;
		token = token_buf;
	} else if (getenv("EMON_TOKEN") && *getenv("EMON_TOKEN")) {
		token = getenv("EMON_TOKEN");
	} else {
		fprintf(stderr, "emon-server: warning: no token configured, "
				"ingest is unauthenticated\n");
	}

	db = store_open(db_path);
	if (!db)
		return 1;
	int lfd = listen_on(listen_addr);
	if (lfd < 0)
		return 1;

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	fprintf(stderr, "emon-server: listening on %s, db %s\n", listen_addr,
		db_path);

	while (!stop) {
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0) {
			if (errno != EINTR)
				perror("accept");
			continue;
		}
		struct timeval tv = {.tv_sec = IO_TIMEOUT_SEC};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		request rq;
		sbuf raw;
		int err;
		sb_init(&raw);
		if (read_request(fd, &rq, &raw, &err) == 0)
			handle(fd, &rq);
		else if (raw.len > 0)
			send_error(fd, err, err == 413 ? "body too large"
						       : "bad request");
		sb_free(&raw);
		close(fd);
	}

	close(lfd);
	store_close(db);
	return 0;
}
