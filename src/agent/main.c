/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * emon-agent: follows /var/log/emerge.log and ships new lines to emon-server.
 *
 * The agent does no parsing of its own: it sends complete lines together
 * with the log's inode and the byte offset they start at, so the server can
 * drop anything it already has. The last acknowledged position is kept in a
 * state file so restarts neither lose nor duplicate data.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "../common/util.h"

#define CHUNK_MAX (256 * 1024)
#define HEARTBEAT_SEC 60
#define BACKOFF_MAX_SEC 300

typedef struct {
	const char *server;
	const char *log_path;
	const char *state_path;
	char host[HOSTNAME_MAX + 1];
	char token[256];
	int interval;
	int from_end;
	int once;
	int verbose;
} config;

typedef struct {
	int fd;
	unsigned long long inode;
	long long offset;
} logpos;

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void sleep_sec(int sec)
{
	for (int i = 0; i < sec * 10 && !stop; i++) {
		struct timespec ts = {0, 100 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
}

/* ------------------------------------------------------------ state file */

static int load_state(const char *path, unsigned long long *inode,
		      long long *offset)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int ok = fscanf(f, "%llu %lld", inode, offset) == 2;
	fclose(f);
	return ok ? 0 : -1;
}

static int save_state(const char *path, unsigned long long inode,
		      long long offset)
{
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "emon-agent: cannot write %s: %s\n", tmp,
			strerror(errno));
		return -1;
	}
	fprintf(f, "%llu %lld\n", inode, offset);
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return rename(tmp, path);
}

/* ------------------------------------------------------------------ HTTP */

static size_t discard(char *ptr, size_t size, size_t n, void *ud)
{
	(void)ptr;
	(void)ud;
	return size * n;
}

static int post(const config *cfg, unsigned long long inode, long long offset,
		const char *data, size_t len)
{
	char url[2048], h_auth[300], h_inode[64], h_off[64];
	CURL *c = curl_easy_init();
	if (!c)
		return -1;

	char *host = curl_easy_escape(c, cfg->host, 0);
	snprintf(url, sizeof(url), "%s/api/v1/ingest?host=%s", cfg->server,
		 host);
	curl_free(host);

	struct curl_slist *hdrs = NULL;
	hdrs = curl_slist_append(hdrs, "Content-Type: text/plain");
	hdrs = curl_slist_append(hdrs, "Expect:");
	if (cfg->token[0]) {
		snprintf(h_auth, sizeof(h_auth), "Authorization: Bearer %s",
			 cfg->token);
		hdrs = curl_slist_append(hdrs, h_auth);
	}
	snprintf(h_inode, sizeof(h_inode), "X-Log-Inode: %llu", inode);
	snprintf(h_off, sizeof(h_off), "X-Log-Offset: %lld", offset);
	hdrs = curl_slist_append(hdrs, h_inode);
	hdrs = curl_slist_append(hdrs, h_off);

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, data ? data : "");
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)len);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);

	int rc = -1;
	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		fprintf(stderr, "emon-agent: %s: %s\n", cfg->server,
			curl_easy_strerror(res));
	} else {
		long status = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		if (status >= 200 && status < 300)
			rc = 0;
		else
			fprintf(stderr, "emon-agent: server replied HTTP %ld\n",
				status);
	}
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(c);
	return rc;
}

/* --------------------------------------------------------------- log I/O */

static int open_log(const config *cfg, logpos *lp)
{
	struct stat st;
	int fd = open(cfg->log_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return -1;
	}
	lp->fd = fd;
	if ((unsigned long long)st.st_ino != lp->inode) {
		/* Different file than last time: start at its beginning. */
		lp->inode = (unsigned long long)st.st_ino;
		lp->offset = 0;
	}
	return 0;
}

/*
 * Send everything between lp->offset and EOF that ends in a newline.
 * Returns 1 if anything was sent, 0 if there was nothing to do, -1 on a
 * failed upload.
 */
static int drain(const config *cfg, logpos *lp, char *buf)
{
	int sent = 0;
	struct stat st;

	if (fstat(lp->fd, &st) == 0 && st.st_size < lp->offset) {
		fprintf(stderr, "emon-agent: %s was truncated\n", cfg->log_path);
		lp->offset = 0;
	}

	while (!stop) {
		ssize_t n = pread(lp->fd, buf, CHUNK_MAX, lp->offset);
		if (n <= 0)
			break;
		size_t len = (size_t)n;
		char *nl = memrchr(buf, '\n', len);
		if (nl)
			len = (size_t)(nl - buf) + 1;
		else if (len < CHUNK_MAX)
			break; /* partial line still being written */

		if (post(cfg, lp->inode, lp->offset, buf, len) < 0)
			return -1;
		lp->offset += (long long)len;
		save_state(cfg->state_path, lp->inode, lp->offset);
		if (cfg->verbose)
			fprintf(stderr, "emon-agent: sent %zu bytes, offset %lld\n",
				len, lp->offset);
		sent = 1;
	}
	return sent;
}

/* Has the path been replaced (log rotation) since we opened it? */
static int rotated(const config *cfg, const logpos *lp)
{
	struct stat st;
	if (stat(cfg->log_path, &st) < 0)
		return 0; /* gone for now; keep reading the old one */
	return (unsigned long long)st.st_ino != lp->inode;
}

static int run(const config *cfg)
{
	logpos lp = {.fd = -1, .offset = 0};
	int have_state = load_state(cfg->state_path, &lp.inode, &lp.offset) == 0;
	char *buf = malloc(CHUNK_MAX);
	time_t last_post = 0;
	int backoff = cfg->interval;

	if (!buf)
		return 1;

	while (!stop) {
		if (lp.fd < 0) {
			if (open_log(cfg, &lp) == 0 && !have_state && cfg->from_end) {
				struct stat st;
				fstat(lp.fd, &st);
				lp.offset = st.st_size;
			}
			have_state = 1;
		}

		int r = 0;
		if (lp.fd >= 0) {
			r = drain(cfg, &lp, buf);
			if (r >= 0 && rotated(cfg, &lp)) {
				/* old file fully shipped; switch to the new one */
				close(lp.fd);
				lp.fd = -1;
				continue;
			}
		}

		if (r == 0 && time(NULL) - last_post >= HEARTBEAT_SEC)
			r = post(cfg, lp.inode, lp.offset, NULL, 0) < 0 ? -1 : 1;
		if (r > 0)
			last_post = time(NULL);

		if (cfg->once) {
			free(buf);
			return r < 0 ? 1 : 0;
		}
		if (r < 0) {
			sleep_sec(backoff);
			backoff = backoff * 2 > BACKOFF_MAX_SEC ? BACKOFF_MAX_SEC
							       : backoff * 2;
		} else {
			backoff = cfg->interval;
			sleep_sec(cfg->interval);
		}
	}
	if (lp.fd >= 0)
		close(lp.fd);
	free(buf);
	return 0;
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: emon-agent -s URL [options]\n"
		"  -s, --server URL        emon-server base URL, e.g. http://mon:8080\n"
		"  -n, --name NAME         host name to report (default: hostname)\n"
		"  -t, --token-file PATH   file holding the shared token (or EMON_TOKEN)\n"
		"  -l, --log PATH          emerge log (default /var/log/emerge.log)\n"
		"  -S, --state PATH        state file (default %s)\n"
		"  -i, --interval SEC      poll interval (default 2)\n"
		"  -e, --from-end          on first run skip existing history\n"
		"  -1, --once              ship what is there, then exit\n"
		"  -v, --verbose           log every upload\n"
		"  -h, --help              show this help\n",
		EMON_DEFAULT_STATE);
}

int main(int argc, char **argv)
{
	config cfg = {
		.log_path = "/var/log/emerge.log",
		.state_path = EMON_DEFAULT_STATE,
		.interval = 2,
	};
	const char *token_file = NULL;
	static const struct option opts[] = {
		{"server", required_argument, NULL, 's'},
		{"name", required_argument, NULL, 'n'},
		{"token-file", required_argument, NULL, 't'},
		{"log", required_argument, NULL, 'l'},
		{"state", required_argument, NULL, 'S'},
		{"interval", required_argument, NULL, 'i'},
		{"from-end", no_argument, NULL, 'e'},
		{"once", no_argument, NULL, '1'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "s:n:t:l:S:i:e1vh", opts, NULL)) != -1) {
		switch (c) {
		case 's': cfg.server = optarg; break;
		case 'n': snprintf(cfg.host, sizeof(cfg.host), "%s", optarg); break;
		case 't': token_file = optarg; break;
		case 'l': cfg.log_path = optarg; break;
		case 'S': cfg.state_path = optarg; break;
		case 'i': cfg.interval = atoi(optarg); break;
		case 'e': cfg.from_end = 1; break;
		case '1': cfg.once = 1; break;
		case 'v': cfg.verbose = 1; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (!cfg.server) {
		usage(stderr);
		return 2;
	}
	if (cfg.interval < 1)
		cfg.interval = 1;

	if (!cfg.host[0]) {
		gethostname(cfg.host, sizeof(cfg.host) - 1);
		char *dot = strchr(cfg.host, '.');
		if (dot)
			*dot = '\0';
	}
	if (!valid_hostname(cfg.host)) {
		fprintf(stderr, "emon-agent: invalid host name '%s'\n", cfg.host);
		return 2;
	}

	if (token_file) {
		if (read_token(token_file, cfg.token, sizeof(cfg.token)) < 0)
			return 1;
	} else if (getenv("EMON_TOKEN")) {
		snprintf(cfg.token, sizeof(cfg.token), "%s", getenv("EMON_TOKEN"));
	}

	/* strip trailing slashes from the URL */
	static char server[1024];
	snprintf(server, sizeof(server), "%s", cfg.server);
	for (size_t n = strlen(server); n > 0 && server[n - 1] == '/'; n--)
		server[n - 1] = '\0';
	cfg.server = server;

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	int rc = run(&cfg);
	curl_global_cleanup();
	return rc;
}
