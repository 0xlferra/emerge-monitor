#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a static emon-server for Synology's "qoriq" platform (DS213+ and
# friends: Freescale P1022, PowerPC e500v2, 32-bit big-endian, DSM 6.2,
# Linux 2.6.32).
#
# The e500v2 has no classic FPU (it has SPE instead), so everything is built
# soft-float: musl and a compiler runtime from LLVM's compiler-rt, both
# compiled here, plus SQLite built into the binary. A static musl binary also
# sidesteps the old kernel, which a current glibc refuses to run on.
#
# Needs: git, make, a native cc, tclsh (to generate SQLite's amalgamation),
# a 32-bit PowerPC cross gcc and binutils (Debian/Ubuntu:
# gcc-powerpc-linux-gnu; Gentoo: crossdev -t powerpc-unknown-linux-gnu, then
# run with CROSS=powerpc-unknown-linux-gnu-). Its libc and libgcc are not
# used, so any 32-bit PowerPC Linux gcc will do.
# With --test also qemu-ppc (qemu-user), curl and libcurl/sqlite3 dev files
# to build the native agent the tests talk to.
#
# usage: contrib/synology/build-qoriq.sh [--test] [BUILD_DIR]
# The binary ends up in BUILD_DIR/emon-server (default build/qoriq).
set -eu

MUSL_TAG=v1.2.5
MUSL_COMMIT=0784374d561435f7c787a555aeab8ede699ed298
LLVM_TAG=llvmorg-18.1.8
LLVM_COMMIT=3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff
SQLITE_TAG=version-3.46.1
SQLITE_COMMIT=f3d536d37825302e31ed0eddd811c689f38f85a3

# Prefix of the PowerPC cross tools: Debian/Ubuntu name them
# powerpc-linux-gnu-*, Gentoo's crossdev powerpc-unknown-linux-gnu-*.
CROSS=${CROSS:-powerpc-linux-gnu-}
CPU_FLAGS="-msoft-float -mlong-double-64 -mcpu=powerpc"

run_tests=0
if [ "${1:-}" = --test ]; then
	run_tests=1
	shift
fi
top=$(cd "$(dirname "$0")/../.." && pwd)
B=$(mkdir -p "${1:-$top/build/qoriq}" && cd "${1:-$top/build/qoriq}" && pwd)
jobs=$(nproc 2>/dev/null || echo 2)

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'build-qoriq: %s\n' "$*" >&2; exit 1; }

for tool in git make tclsh "${CROSS}gcc" "${CROSS}ar" "${CROSS}objdump"; do
	command -v "$tool" >/dev/null || die "missing $tool"
done

# fetch NAME URL TAG COMMIT [SPARSE_PATH]: shallow clone, checked against
# the expected commit so a tampered mirror is caught.
fetch() {
	dir="$B/src/$1"
	if [ ! -d "$dir/.git" ]; then
		log "fetching $1 $3"
		if [ -n "${5:-}" ]; then
			git -c advice.detachedHead=false clone -q --depth 1 \
				--branch "$3" --filter=blob:none --sparse "$2" "$dir"
			git -C "$dir" sparse-checkout set "$5"
		else
			git -c advice.detachedHead=false clone -q --depth 1 \
				--branch "$3" "$2" "$dir"
		fi
	fi
	got=$(git -C "$dir" rev-parse HEAD)
	[ "$got" = "$4" ] || die "$1: expected commit $4, got $got"
}

mkdir -p "$B/src"
# musl.libc.org is the upstream; any mirror works since the commit is checked
fetch musl https://github.com/kraj/musl.git "$MUSL_TAG" "$MUSL_COMMIT"
fetch llvm https://github.com/llvm/llvm-project.git "$LLVM_TAG" "$LLVM_COMMIT" \
	compiler-rt/lib/builtins
fetch sqlite https://github.com/sqlite/sqlite.git "$SQLITE_TAG" "$SQLITE_COMMIT"

sysroot=$B/sysroot

# --- musl, soft-float ------------------------------------------------------
# Synology's DSM 6 kernels are Linux 2.6.32 plus syscalls of their own,
# numbered past the end of the upstream table. musl tries newer syscalls
# first (clock_gettime64 for time(), statx for stat()) and falls back to the
# old ones only on ENOSYS; on DSM those numbers can be Synology syscalls
# that fail differently, and time() then returned garbage. Nothing from
# statx (383) on exists in 2.6.32, so move those numbers out of range: the
# kernel answers ENOSYS and musl goes straight to the old calls. The
# network syscalls (326-344), which DSM's kernel does have, are kept.
musl_stamp=$sysroot/.musl-syscalls-ge383-disabled
if [ ! -f "$musl_stamp" ]; then
	log "building musl"
	table=$B/src/musl/arch/powerpc/bits/syscall.h.in
	git -C "$B/src/musl" checkout -q -- arch/powerpc/bits/syscall.h.in
	awk '$1 == "#define" && $3 + 0 >= 383 { $3 = $3 + 65536 } { print }' \
		"$table" > "$table.new"
	mv "$table.new" "$table"
	grep -q '__NR_clock_gettime64 65939$' "$table" ||
		die "could not renumber musl's syscall table"
	rm -rf "$sysroot"
	rm -rf "$B/musl-build"
	mkdir -p "$B/musl-build"
	cd "$B/musl-build"
	"$B/src/musl/configure" --target=powerpc-linux-musl --prefix="$sysroot" \
		--disable-shared CC="${CROSS}gcc" CFLAGS="$CPU_FLAGS -O2" >/dev/null
	grep -q '^SUBARCH = -sf' config.mak ||
		die "musl did not configure for soft-float"
	make -s -j"$jobs" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" >../musl-build.log 2>&1 ||
		die "musl build failed, see $B/musl-build.log"
	make -s install AR="${CROSS}ar" RANLIB="${CROSS}ranlib" >/dev/null
	touch "$musl_stamp"
	cd "$top"
fi

# Replace libgcc, built hard-float on Debian/Ubuntu, by our runtime.
specs=$sysroot/lib/e500.specs
sed "s|^libgcc.a%s %:if-exists(libgcc_eh.a%s)\$|$sysroot/lib/libsoftfp.a|" \
	"$sysroot/lib/musl-gcc.specs" > "$specs"
grep -q libsoftfp "$specs" || die "could not adapt musl-gcc.specs"

cc=$B/e500-cc
cat > "$cc" <<EOF
#!/bin/sh
exec ${CROSS}gcc -specs "$specs" $CPU_FLAGS -static "\$@"
EOF
chmod +x "$cc"

# --- compiler runtime, soft-float -------------------------------------------
if [ ! -f "$sysroot/lib/libsoftfp.a" ]; then
	log "building soft-float compiler runtime"
	rt=$B/rt
	rm -rf "$rt"
	mkdir -p "$rt"
	builtins=$B/src/llvm/compiler-rt/lib/builtins
	# Not every file applies to 32-bit PowerPC; those that fail to build are
	# not needed (the link below would fail otherwise).
	for f in "$builtins"/*.c; do
		"$cc" -c -O2 -fno-builtin -I"$builtins" \
			-o "$rt/$(basename "$f" .c).o" "$f" 2>/dev/null || true
	done
	"${CROSS}ar" rcs "$sysroot/lib/libsoftfp.a" "$rt"/*.o
fi

# --- SQLite amalgamation -----------------------------------------------------
if [ ! -f "$B/sqlite-build/sqlite3.c" ]; then
	log "generating sqlite3.c"
	rm -rf "$B/sqlite-build"
	mkdir -p "$B/sqlite-build"
	(cd "$B/sqlite-build" &&
	 "$B/src/sqlite/configure" --disable-tcl >/dev/null &&
	 make -s sqlite3.c >/dev/null)
fi

# --- emon-server ---------------------------------------------------------------
# Build from a copy so the cross objects never mix with a native build.
log "building emon-server"
tree=$B/tree
rm -rf "$tree"
mkdir -p "$tree"
(cd "$top" && cp -R Makefile src tests tools LICENSE "$tree"/)
make -s -C "$tree" emon-server CC="$cc" CFLAGS="-O2" \
	SQLITE_SRC="$B/sqlite-build/sqlite3.c"
cp "$tree/emon-server" "$B/emon-server"
"${CROSS}strip" -o "$B/emon-server.stripped" "$B/emon-server"

# The e500v2 traps on classic FPU instructions: make sure there are none.
log "checking for instructions the e500v2 lacks"
bad=$("${CROSS}objdump" -d "$B/emon-server" |
	awk -F'\t' 'NF >= 3 { split($3, a, " "); print a[1] }' | sort -u |
	grep -E '^(f[a-z]+|lf[sd]u?x?|stf[sd]u?x?|stfiwx|mffs.?|mtfs.*|lwsync)$' || true)
[ -z "$bad" ] || die "unsupported instructions in emon-server: $bad"

if [ $run_tests = 1 ]; then
	command -v qemu-ppc >/dev/null || die "--test needs qemu-ppc (qemu-user)"
	qemu="qemu-ppc -cpu e500v2"
	log "running unit tests on an emulated e500v2"
	make -s -C "$tree" tests/test_parser tests/test_buildlog CC="$cc" CFLAGS="-O2"
	(cd "$tree" && $qemu tests/test_parser tests/data/emerge.log &&
	 $qemu tests/test_buildlog)

	log "running the integration test against the e500v2 server"
	native=$B/native
	rm -rf "$native"
	mkdir -p "$native"
	(cd "$top" && cp -R Makefile src tests tools "$native"/)
	make -s -C "$native" emon-agent
	(cd "$tree" && EMON_SERVER="$qemu ./emon-server" \
		EMON_AGENT="$native/emon-agent" tests/integration.sh)
fi

log "done: $B/emon-server ($(wc -c < "$B/emon-server.stripped") bytes stripped)"
