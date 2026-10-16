/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>

#include "../src/common/emerge_log.h"
#include "../src/common/util.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void check_pn(const char *atom, const char *want)
{
	char pn[256];
	el_atom_pn(atom, pn, sizeof(pn));
	if (strcmp(pn, want)) {
		fprintf(stderr, "el_atom_pn(%s) = %s, want %s\n", atom, pn, want);
		failures++;
	}
}

static void check_lines(void)
{
	el_event ev;
	const char *l;

	l = "1700000100:  *** emerge --ask --update @world\n";
	CHECK(el_parse_line(l, strlen(l), &ev));
	CHECK(ev.type == EL_SESSION_START && ev.ts == 1700000100);
	CHECK(!strcmp(ev.text, "emerge --ask --update @world"));

	l = "1700000110:  >>> emerge (1 of 3) sys-libs/zlib-1.3-r1 to /";
	CHECK(el_parse_line(l, strlen(l), &ev));
	CHECK(ev.type == EL_PKG_START && ev.seq == 1 && ev.total == 3);
	CHECK(!strcmp(ev.atom, "sys-libs/zlib-1.3-r1"));

	l = "1700000157:  ::: completed emerge (12 of 30) dev-lang/python-3.11.6 to /";
	CHECK(el_parse_line(l, strlen(l), &ev));
	CHECK(ev.type == EL_PKG_DONE && ev.seq == 12 && ev.total == 30);

	l = "1700004000:  *** exiting unsuccessfully with status '1'.";
	CHECK(el_parse_line(l, strlen(l), &ev));
	CHECK(ev.type == EL_SESSION_END && ev.exit_code == 1);

	l = "1700010062:  *** exiting successfully.";
	CHECK(el_parse_line(l, strlen(l), &ev));
	CHECK(ev.type == EL_SESSION_END && ev.exit_code == 0);

	l = "1700000060: === Sync completed for gentoo";
	CHECK(el_parse_line(l, strlen(l), &ev));
	CHECK(ev.type == EL_SYNC_DONE && !strcmp(ev.text, "gentoo"));

	l = "1700000110:  === (1 of 3) Cleaning (sys-libs/zlib-1.3-r1::/x)";
	CHECK(!el_parse_line(l, strlen(l), &ev));
	l = "garbage";
	CHECK(!el_parse_line(l, strlen(l), &ev));
	l = "";
	CHECK(!el_parse_line(l, 0, &ev));
}

static int check_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		return 1;
	}
	char line[4096];
	int counts[8] = {0};
	el_event ev;
	while (fgets(line, sizeof(line), f))
		if (el_parse_line(line, strlen(line), &ev))
			counts[ev.type]++;
	fclose(f);
	CHECK(counts[EL_SESSION_START] == 4);
	CHECK(counts[EL_PKG_START] == 5);
	CHECK(counts[EL_PKG_DONE] == 3);
	CHECK(counts[EL_SESSION_END] == 2);
	CHECK(counts[EL_TERMINATING] == 4);
	CHECK(counts[EL_SYNC_DONE] == 1);
	return 0;
}

int main(int argc, char **argv)
{
	check_pn("sys-libs/zlib-1.3-r1", "sys-libs/zlib");
	check_pn("dev-lang/python-3.11.6", "dev-lang/python");
	check_pn("x11-libs/gtk+-3.24.38", "x11-libs/gtk+");
	check_pn("media-libs/libsdl2-2.28.5", "media-libs/libsdl2");
	check_pn("dev-qt/qtbase-6.6.0::gentoo", "dev-qt/qtbase");
	check_pn("app-misc/foo-1.0_rc2-r10", "app-misc/foo");
	check_lines();

	CHECK(valid_atom("sys-libs/zlib-1.3-r1"));
	CHECK(valid_atom("x11-libs/gtk+-3.24.38"));
	CHECK(!valid_atom("../etc/passwd"));
	CHECK(!valid_atom("sys-libs/../../x"));
	CHECK(!valid_atom("sys-libs/.hidden"));
	CHECK(!valid_atom("a/b/c"));
	CHECK(!valid_atom("noslash"));
	CHECK(!valid_atom("/abs"));
	CHECK(!valid_atom("cat/"));
	CHECK(!valid_atom("cat/pkg 1"));
	if (argc > 1)
		failures += check_file(argv[1]);
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_parser: ok\n");
	return 0;
}
