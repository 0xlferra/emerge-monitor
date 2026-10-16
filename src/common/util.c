/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int const_time_eq(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char diff = la != lb;
	for (size_t i = 0; i < la; i++)
		diff |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
	return diff == 0;
}

int valid_hostname(const char *s)
{
	size_t n = 0;
	for (; s[n]; n++) {
		unsigned char c = (unsigned char)s[n];
		if (!isalnum(c) && c != '.' && c != '_' && c != '-')
			return 0;
	}
	return n > 0 && n <= HOSTNAME_MAX;
}

int valid_atom(const char *s)
{
	const char *slash = strchr(s, '/');
	size_t n = strlen(s);

	if (!slash || slash == s || !slash[1] || strchr(slash + 1, '/'))
		return 0;
	if (n >= 256 || s[0] == '.' || slash[1] == '.' || strstr(s, ".."))
		return 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (!isalnum(c) && c != '/' && c != '+' && c != '_' && c != '.' &&
		    c != '-')
			return 0;
	}
	return 1;
}

int read_token(const char *path, char *out, size_t outlen)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cannot open token file %s: %s\n", path,
			strerror(errno));
		return -1;
	}
	if (!fgets(out, (int)outlen, f))
		out[0] = '\0';
	fclose(f);
	size_t n = strlen(out);
	while (n > 0 && isspace((unsigned char)out[n - 1]))
		out[--n] = '\0';
	if (n == 0) {
		fprintf(stderr, "token file %s is empty\n", path);
		return -1;
	}
	return 0;
}
