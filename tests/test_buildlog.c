/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/server/buildlog.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* Byte i of our fake log file. */
static char byte_at(long long i)
{
	return (char)('a' + i % 26);
}

static void fill(char *buf, long long off, size_t n)
{
	for (size_t i = 0; i < n; i++)
		buf[i] = byte_at(off + (long long)i);
}

/* Does the buffered data match the file from *start to *end? */
static int consistent(const char *data, size_t len, long long start, long long end)
{
	if ((long long)len != end - start)
		return 0;
	for (size_t i = 0; i < len; i++)
		if (data[i] != byte_at(start + (long long)i))
			return 0;
	return 1;
}

int main(void)
{
	const char *data;
	size_t len;
	long long start, end;
	static char buf[300 * 1024];

	CHECK(bl_read("h", "c/p-1", 0, &data, &len, &start, &end) < 0);

	/* Sequential appends past the cap keep exactly the last BL_CAP bytes. */
	long long off = 0;
	for (int i = 0; i < 5; i++) {
		fill(buf, off, sizeof(buf));
		bl_append("h", "c/p-1", 7, off, buf, sizeof(buf), 100);
		off += (long long)sizeof(buf);
	}
	CHECK(bl_read("h", "c/p-1", 0, &data, &len, &start, &end) == 0);
	CHECK(end == off && start == off - BL_CAP);
	CHECK(consistent(data, len, start, end));

	/* Reading from an offset inside the buffer returns only the rest. */
	CHECK(bl_read("h", "c/p-1", off - 10, &data, &len, &start, &end) == 0);
	CHECK(start == off - 10 && len == 10 && consistent(data, len, start, end));

	/* Re-sent overlapping data is not duplicated. */
	fill(buf, off - 100, 150);
	bl_append("h", "c/p-1", 7, off - 100, buf, 150, 101);
	off += 50;
	CHECK(bl_read("h", "c/p-1", 0, &data, &len, &start, &end) == 0);
	CHECK(end == off && consistent(data, len, start, end));

	/* A single chunk larger than the cap. */
	static char big[BL_CAP + 1000];
	fill(big, off, sizeof(big));
	bl_append("h", "c/p-1", 7, off, big, sizeof(big), 102);
	off += (long long)sizeof(big);
	CHECK(bl_read("h", "c/p-1", 0, &data, &len, &start, &end) == 0);
	CHECK(end == off && len == BL_CAP && consistent(data, len, start, end));

	/* A gap restarts the buffer at the new offset. */
	fill(buf, off + 5000, 10);
	bl_append("h", "c/p-1", 7, off + 5000, buf, 10, 103);
	CHECK(bl_read("h", "c/p-1", 0, &data, &len, &start, &end) == 0);
	CHECK(start == off + 5000 && len == 10 && consistent(data, len, start, end));

	/* A new inode (package rebuilt) starts over. */
	fill(buf, 0, 20);
	bl_append("h", "c/p-1", 8, 0, buf, 20, 104);
	CHECK(bl_read("h", "c/p-1", 0, &data, &len, &start, &end) == 0);
	CHECK(start == 0 && end == 20 && consistent(data, len, start, end));

	/* Logs are per host and atom, and expire after BL_TTL. */
	CHECK(bl_read("other", "c/p-1", 0, &data, &len, &start, &end) < 0);
	bl_append("h", "c/q-1", 1, 0, "x", 1, 104 + BL_TTL + 1);
	CHECK(bl_read("h", "c/p-1", 0, &data, &len, &start, &end) < 0);
	CHECK(bl_read("h", "c/q-1", 0, &data, &len, &start, &end) == 0);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_buildlog: ok\n");
	return 0;
}
