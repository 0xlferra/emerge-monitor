/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * emon-agent: follows /var/log/emerge.log and ships new lines to emon-server.
 *
 * The agent does no parsing of its own: it sends complete lines together
 * with the log's inode and the byte offset they start at, so the server can
 * drop anything it already has. The last acknowledged position is kept in a
 * state file so restarts neither lose nor duplicate data.
 *
 * The server answers with the packages it believes are building on this
 * host; the agent follows their build logs and ships those too, so they can
 * be watched live from the dashboard. Logs are read from PORTAGE_LOGDIR when
 * that is set, because Portage keeps PORTAGE_TMPDIR/portage/<category>
 * readable by user portage only.
 */
#include <dirent.h>
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

#include "../common/emerge_log.h"
#include "../common/sbuf.h"
#include "../common/util.h"

#define CHUNK_MAX (256 * 1024)
#define HEARTBEAT_SEC 60
#define BACKOFF_MAX_SEC 300

#define MAX_TAILS 32
/* When starting to follow a build.log, only send this much of its end. */
#define TAIL_INITIAL (64 * 1024)
#define TAIL_CHUNK (64 * 1024)
/* Per poll and per log, so one chatty build cannot starve the others. */
#define TAIL_CHUNKS_PER_POLL 4
/* Further behind than this, skip ahead: the view is live, not an archive. */
#define TAIL_MAX_BEHIND (1024 * 1024)

typedef struct {
	const char *server;
	const char *log_path;
	const char *state_path;
	const char *portage_tmpdir;
	const char *portage_logdir;     /* NULL: read from PORTAGE_TMPDIR */
	char host[HOSTNAME_MAX + 1];
	char token[256];
	int interval;
	int from_end;
	int once;
	int verbose;
	int build_logs;
} config;

typedef struct {
	int fd;
	unsigned long long inode;
	long long offset;
} logpos;

/* A build log the server asked us to follow. */
typedef struct {
	char atom[256];
	long long start_ts;     /* when emerge started the package, 0 if unknown */
	char path[4096];        /* log found in PORTAGE_LOGDIR, "" until then */
	unsigned long long inode;
	long long offset;       /* -1: not opened yet */
	int wanted;             /* still in the server's list */
	int warned;
} tail;

static volatile sig_atomic_t stop;
static tail tails[MAX_TAILS];
static int ntails;

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

static size_t collect(char *ptr, size_t size, size_t n, void *ud)
{
	if (ud)
		sb_append(ud, ptr, size * n);
	return size * n;
}

/*
 * POST `data` to `endpoint` (e.g. "ingest"), tagged with the file position
 * it comes from. If `atom` is set it is passed as a query parameter. The
 * reply body is stored in `resp` when that is not NULL.
 */
static int post(const config *cfg, const char *endpoint, const char *atom,
		unsigned long long inode, long long offset,
		const char *data, size_t len, sbuf *resp)
{
	char url[2048], h_auth[300], h_inode[64], h_off[64];
	CURL *c = curl_easy_init();
	if (!c)
		return -1;

	char *host = curl_easy_escape(c, cfg->host, 0);
	char *eatom = atom ? curl_easy_escape(c, atom, 0) : NULL;
	snprintf(url, sizeof(url), "%s/api/v1/%s?host=%s%s%s", cfg->server,
		 endpoint, host, eatom ? "&atom=" : "", eatom ? eatom : "");
	curl_free(host);
	curl_free(eatom);
	if (resp)
		sb_reset(resp);

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
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, resp);

	int rc = -1;
	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		fprintf(stderr, "emon-agent: %s: %s\n", url,
			curl_easy_strerror(res));
	} else {
		long status = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		if (status >= 200 && status < 300)
			rc = 0;
		else
			fprintf(stderr, "emon-agent: %s: server replied HTTP %ld\n",
				endpoint, status);
	}
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(c);
	return rc;
}

/* ------------------------------------------------------------ build logs */

/*
 * Update the set of followed logs from an ingest reply, which carries
 * `"building":["cat/pf",...]`. Logs that dropped out of the list are kept
 * until their final lines have been sent (see follow_build_logs).
 */
static void set_wanted(const char *reply)
{
	const char *p = reply ? strstr(reply, "\"building\":[") : NULL;

	for (int i = 0; i < ntails; i++)
		tails[i].wanted = 0;
	if (!p)
		return;
	p += strlen("\"building\":[");

	/* [{"atom":"cat/pf","start_ts":N},...] */
	while ((p = strchr(p, '{')) != NULL) {
		const char *end = strchr(p, '}');
		const char *a = strstr(p, "\"atom\":\"");
		const char *ts = strstr(p, "\"start_ts\":");
		if (!end || !a || a > end)
			break;
		a += strlen("\"atom\":\"");
		const char *q = memchr(a, '"', (size_t)(end - a));
		char atom[256];
		size_t n = q ? (size_t)(q - a) : sizeof(atom);
		p = end + 1;
		if (n >= sizeof(atom))
			continue;
		memcpy(atom, a, n);
		atom[n] = '\0';
		if (!valid_atom(atom))
			continue; /* never let the server pick arbitrary paths */

		int i;
		for (i = 0; i < ntails && strcmp(tails[i].atom, atom); i++)
			;
		if (i == ntails) {
			if (ntails == MAX_TAILS)
				continue;
			memset(&tails[i], 0, sizeof(tails[i]));
			snprintf(tails[i].atom, sizeof(tails[i].atom), "%s", atom);
			if (ts && ts < end)
				tails[i].start_ts = atoll(ts + strlen("\"start_ts\":"));
			tails[i].offset = -1;
			ntails++;
		}
		tails[i].wanted = 1;
	}
}

/* Keep the newest log in `dir` named for cat/pf (cat NULL: split-log). */
static void scan_logdir(const char *dir, const char *cat, const char *pf,
			long long min_ts, long long *best, char *out,
			size_t outlen)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	if (!d)
		return;
	while ((de = readdir(d)) != NULL) {
		long long ts = el_logdir_match(de->d_name, cat, pf);
		if (ts >= min_ts && ts > *best) {
			*best = ts;
			snprintf(out, outlen, "%s/%s", dir, de->d_name);
		}
	}
	closedir(d);
}

/*
 * Find the log of the current build in PORTAGE_LOGDIR. Older logs of the
 * same version (an earlier failed attempt, say) are skipped by comparing
 * the build start in the file name with when emerge started the package.
 */
static int find_logdir_log(const config *cfg, tail *t)
{
	char cat[256], dir[4096], path[4096];
	const char *pf = strchr(t->atom, '/') + 1;
	long long best = -1;
	/* Name times have one-second resolution; allow a little slack. */
	long long min_ts = t->start_ts > 0 ? t->start_ts - 60 : 0;

	snprintf(cat, sizeof(cat), "%.*s", (int)(pf - 1 - t->atom), t->atom);
	scan_logdir(cfg->portage_logdir, cat, pf, min_ts, &best, path,
		    sizeof(path));
	snprintf(dir, sizeof(dir), "%s/build/%s", cfg->portage_logdir, cat);
	scan_logdir(dir, NULL, pf, min_ts, &best, path, sizeof(path));
	if (best < 0)
		return -1;
	snprintf(t->path, sizeof(t->path), "%s", path);
	return 0;
}

static void follow_one(const config *cfg, tail *t, char *buf)
{
	char tmp_path[4096];
	const char *path;
	struct stat st;

	if (cfg->portage_logdir) {
		/* not written yet (still fetching), or not there at all */
		if (!t->path[0] && find_logdir_log(cfg, t) < 0)
			return;
		path = t->path;
	} else {
		snprintf(tmp_path, sizeof(tmp_path), "%s/portage/%s/temp/build.log",
			 cfg->portage_tmpdir, t->atom);
		path = tmp_path;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		/* ENOENT is normal: not created yet, or cleaned after merge */
		int err = errno;
		if (err == ENOENT || t->warned)
			return;
		fprintf(stderr, "emon-agent: %s: %s\n", path, strerror(err));
		if (err == EACCES && !cfg->portage_logdir)
			fprintf(stderr, "emon-agent: Portage keeps %s/portage/<category> "
				"at mode 0700; set PORTAGE_LOGDIR in make.conf so "
				"build logs are readable by group portage (see README)\n",
				cfg->portage_tmpdir);
		t->warned = 1;
		return;
	}
	if (fstat(fd, &st) < 0)
		goto out;
	if (t->offset < 0 || (unsigned long long)st.st_ino != t->inode) {
		t->inode = (unsigned long long)st.st_ino;
		t->offset = st.st_size > TAIL_INITIAL ? st.st_size - TAIL_INITIAL : 0;
	} else if (st.st_size < t->offset) {
		t->offset = 0;
	} else if (st.st_size - t->offset > TAIL_MAX_BEHIND) {
		t->offset = st.st_size - TAIL_INITIAL;
	}

	for (int i = 0; i < TAIL_CHUNKS_PER_POLL && !stop; i++) {
		ssize_t n = pread(fd, buf, TAIL_CHUNK, t->offset);
		if (n <= 0)
			break;
		if (post(cfg, "buildlog", t->atom, t->inode, t->offset, buf,
			 (size_t)n, NULL) < 0)
			break;
		t->offset += n;
	}
out:
	close(fd);
}

static void follow_build_logs(const config *cfg, char *buf)
{
	for (int i = 0; i < ntails;) {
		follow_one(cfg, &tails[i], buf);
		if (tails[i].wanted) {
			i++;
		} else {
			/* finished: its last lines were sent just above */
			tails[i] = tails[--ntails];
		}
	}
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
static int drain(const config *cfg, logpos *lp, char *buf, sbuf *resp)
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

		if (post(cfg, "ingest", NULL, lp->inode, lp->offset, buf, len,
			 resp) < 0)
			return -1;
		set_wanted(resp->data);
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
	sbuf resp;

	if (!buf)
		return 1;
	sb_init(&resp);

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
			r = drain(cfg, &lp, buf, &resp);
			if (r >= 0 && rotated(cfg, &lp)) {
				/* old file fully shipped; switch to the new one */
				close(lp.fd);
				lp.fd = -1;
				continue;
			}
		}

		if (r == 0 && time(NULL) - last_post >= HEARTBEAT_SEC) {
			r = post(cfg, "ingest", NULL, lp.inode, lp.offset, NULL, 0,
				 &resp) < 0 ? -1 : 1;
			if (r > 0)
				set_wanted(resp.data);
		}
		if (r > 0)
			last_post = time(NULL);

		if (r >= 0 && cfg->build_logs)
			follow_build_logs(cfg, buf);

		if (cfg->once) {
			free(buf);
			sb_free(&resp);
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
	sb_free(&resp);
	return 0;
}

/* PORTAGE_LOGDIR from the environment, else from Portage's configuration. */
static const char *detect_logdir(char *buf, size_t len)
{
	const char *env = getenv("PORTAGE_LOGDIR");
	if (env)
		return env;

	FILE *p = popen("portageq envvar PORTAGE_LOGDIR 2>/dev/null", "r");
	if (!p)
		return NULL;
	if (!fgets(buf, (int)len, p))
		buf[0] = '\0';
	pclose(p);
	buf[strcspn(buf, "\n")] = '\0';
	return buf[0] ? buf : NULL;
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
		"  -T, --portage-tmpdir DIR  PORTAGE_TMPDIR (default $PORTAGE_TMPDIR\n"
		"                          or /var/tmp)\n"
		"  -L, --portage-logdir DIR  PORTAGE_LOGDIR to read build logs from\n"
		"                          (default $PORTAGE_LOGDIR, else asks portageq;\n"
		"                          \"\" to read them from PORTAGE_TMPDIR)\n"
		"  -B, --no-build-logs     do not send build.log of running builds\n"
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
		.build_logs = 1,
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
		{"portage-tmpdir", required_argument, NULL, 'T'},
		{"portage-logdir", required_argument, NULL, 'L'},
		{"no-build-logs", no_argument, NULL, 'B'},
		{"once", no_argument, NULL, '1'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "s:n:t:l:S:i:eT:L:B1vh", opts, NULL)) != -1) {
		switch (c) {
		case 's': cfg.server = optarg; break;
		case 'n': snprintf(cfg.host, sizeof(cfg.host), "%s", optarg); break;
		case 't': token_file = optarg; break;
		case 'l': cfg.log_path = optarg; break;
		case 'S': cfg.state_path = optarg; break;
		case 'i': cfg.interval = atoi(optarg); break;
		case 'e': cfg.from_end = 1; break;
		case 'T': cfg.portage_tmpdir = optarg; break;
		case 'L': cfg.portage_logdir = optarg; break;
		case 'B': cfg.build_logs = 0; break;
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
	if (!cfg.portage_tmpdir) {
		const char *t = getenv("PORTAGE_TMPDIR");
		cfg.portage_tmpdir = t && *t ? t : "/var/tmp";
	}
	if (cfg.build_logs) {
		static char logdir[4096];
		if (!cfg.portage_logdir)
			cfg.portage_logdir = detect_logdir(logdir, sizeof(logdir));
		if (cfg.portage_logdir && !*cfg.portage_logdir)
			cfg.portage_logdir = NULL;
		fprintf(stderr, "emon-agent: reading build logs from %s%s\n",
			cfg.portage_logdir ? cfg.portage_logdir : cfg.portage_tmpdir,
			cfg.portage_logdir ? "" : "/portage");
	}

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
