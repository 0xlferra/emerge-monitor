/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Diagnose how the clock reads on a NAS: what the kernel answers to the
 * time64 syscall musl tries first, the old 32-bit one it falls back to,
 * and what time() returns. Only makes calls musl's time() makes anyway.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Upstream numbers on 32-bit PowerPC, whatever the libc headers say. */
#define NR_CLOCK_GETTIME64 403
#define NR_CLOCK_GETTIME32 246

int main(void)
{
	long buf[4];
	long r;

	memset(buf, 0xab, sizeof(buf));
	errno = 0;
	r = syscall(NR_CLOCK_GETTIME64, CLOCK_REALTIME, buf);
	printf("clock_gettime64 (syscall %d): ret=%ld errno=%d (%s) buf=%08lx %08lx %08lx\n",
	       NR_CLOCK_GETTIME64, r, r < 0 ? errno : 0,
	       r < 0 ? strerror(errno) : "-", buf[0], buf[1], buf[2]);

	memset(buf, 0xab, sizeof(buf));
	errno = 0;
	r = syscall(NR_CLOCK_GETTIME32, CLOCK_REALTIME, buf);
	printf("clock_gettime   (syscall %d): ret=%ld errno=%d buf=%ld %ld\n",
	       NR_CLOCK_GETTIME32, r, r < 0 ? errno : 0, buf[0], buf[1]);

	printf("time() = %lld, again %lld\n", (long long)time(NULL),
	       (long long)time(NULL));
	return 0;
}
