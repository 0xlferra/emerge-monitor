/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "emerge_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int starts(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void copy_trim(char *dst, size_t dstlen, const char *src)
{
	size_t n = strlen(src);
	while (n > 0 && isspace((unsigned char)src[n - 1]))
		n--;
	if (n >= dstlen)
		n = dstlen - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* ">>> emerge (1 of 5) cat/pkg-1.0 to /" style lines, after the prefix. */
static int parse_pkg(const char *p, el_event *ev)
{
	return sscanf(p, "(%d of %d) %255s", &ev->seq, &ev->total, ev->atom) == 3;
}

int el_parse_line(const char *line, size_t len, el_event *ev)
{
	char buf[2048];

	memset(ev, 0, sizeof(*ev));
	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, line, len);
	buf[len] = '\0';

	char *end;
	long long ts = strtoll(buf, &end, 10);
	if (end == buf || *end != ':')
		return 0;
	const char *p = end + 1;
	while (*p == ' ')
		p++;
	ev->ts = ts;

	if (starts(p, ">>> emerge ")) {
		if (!parse_pkg(p + strlen(">>> emerge "), ev))
			return 0;
		ev->type = EL_PKG_START;
	} else if (starts(p, "::: completed emerge ")) {
		if (!parse_pkg(p + strlen("::: completed emerge "), ev))
			return 0;
		ev->type = EL_PKG_DONE;
	} else if (starts(p, "*** exiting successfully")) {
		ev->type = EL_SESSION_END;
		ev->exit_code = 0;
	} else if (starts(p, "*** exiting unsuccessfully")) {
		const char *q = strstr(p, "status '");
		ev->type = EL_SESSION_END;
		ev->exit_code = q ? atoi(q + strlen("status '")) : 1;
		if (ev->exit_code == 0)
			ev->exit_code = 1;
	} else if (starts(p, "*** terminating")) {
		ev->type = EL_TERMINATING;
	} else if (starts(p, "*** emerge ")) {
		ev->type = EL_SESSION_START;
		copy_trim(ev->text, sizeof(ev->text), p + strlen("*** "));
	} else if (starts(p, "=== Sync completed")) {
		const char *q = strstr(p, " for ");
		ev->type = EL_SYNC_DONE;
		copy_trim(ev->text, sizeof(ev->text), q ? q + strlen(" for ") : "");
	} else {
		return 0;
	}
	return 1;
}

/* "YYYYMMDD-HHMMSS.log" -> unix time, or -1 */
static long long parse_log_stamp(const char *s)
{
	struct tm tm = {0};
	for (int i = 0; i < 15; i++)
		if (i == 8 ? s[i] != '-' : !isdigit((unsigned char)s[i]))
			return -1;
	if (strcmp(s + 15, ".log"))
		return -1;
	if (sscanf(s, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon,
		   &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
		return -1;
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	return (long long)timegm(&tm);
}

long long el_logdir_match(const char *name, const char *cat, const char *pf)
{
	const size_t stamp = strlen("YYYYMMDD-HHMMSS.log");
	size_t n = strlen(name), lc = cat ? strlen(cat) : 0, lp = strlen(pf);
	size_t nseps = cat ? 2 : 1;

	/* The separator is not known, but every other length is. */
	if (n < lc + lp + stamp + nseps || (n - lc - lp - stamp) % nseps)
		return -1;
	size_t ls = (n - lc - lp - stamp) / nseps;
	const char *sep = name + (cat ? lc : lp);

	if (cat) {
		if (strncmp(name, cat, lc) || strncmp(name + lc + ls, pf, lp) ||
		    strncmp(name + lc + ls + lp, sep, ls))
			return -1;
	} else if (strncmp(name, pf, lp)) {
		return -1;
	}
	return parse_log_stamp(name + n - stamp);
}

void el_atom_pn(const char *atom, char *out, size_t outlen)
{
	if (outlen == 0)
		return;
	snprintf(out, outlen, "%s", atom);

	char *repo = strstr(out, "::");
	if (repo)
		*repo = '\0';

	/* drop -rN revision */
	char *dash = strrchr(out, '-');
	if (dash && dash[1] == 'r' && isdigit((unsigned char)dash[2])) {
		char *q = dash + 2;
		while (isdigit((unsigned char)*q))
			q++;
		if (*q == '\0')
			*dash = '\0';
	}

	/* drop -version: the last '-' followed by a digit */
	dash = strrchr(out, '-');
	const char *slash = strchr(out, '/');
	if (dash && isdigit((unsigned char)dash[1]) && (!slash || slash < dash))
		*dash = '\0';
}
