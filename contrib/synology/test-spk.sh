#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Exercise the DSM package without a NAS: build the .spk around a native
# emon-server, unpack it the way DSM does and run its scripts through
# install, start/stop, upgrade and a fresh install without a token.
#
# usage: contrib/synology/test-spk.sh [SHELL]
# SHELL defaults to sh; "busybox" runs the scripts with BusyBox's ash and
# applets (tr, od, cp, ...) only, which is closer to what DSM 6 has.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
sh=${1:-sh}

tmp=$(mktemp -d)
trap 'kill $(cat "$tmp"/target/var/emon-server.pid 2>/dev/null) 2>/dev/null || true; rm -rf "$tmp"' EXIT
fail() { echo "test-spk: FAIL: $*" >&2; exit 1; }

make -s -C "$top" emon-server
spk=$("$here/make-spk.sh" "$top/emon-server" "$tmp/out")

# --- the package itself ------------------------------------------------------
tar tf "$spk" | sort > "$tmp/list"
for f in INFO package.tgz scripts/postinst scripts/start-stop-status \
	conf/privilege conf/resource WIZARD_UIFILES/install_uifile \
	PACKAGE_ICON.PNG PACKAGE_ICON_256.PNG; do
	grep -qx "$f" "$tmp/list" || grep -qx "./$f" "$tmp/list" || fail "spk lacks $f"
done
mkdir "$tmp/spk"
tar xf "$spk" -C "$tmp/spk"
for f in conf/privilege conf/resource WIZARD_UIFILES/install_uifile; do
	python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$tmp/spk/$f" ||
		fail "$f is not valid JSON"
done
grep -q '^arch="qoriq"$' "$tmp/spk/INFO" || fail "INFO arch"
sum=$(md5sum < "$tmp/spk/package.tgz" | cut -d' ' -f1)
grep -q "^checksum=\"$sum\"$" "$tmp/spk/INFO" || fail "INFO checksum"

# --- DSM's view: target/ from package.tgz, scripts run with SYNOPKG_* ---------
unpack() {
	rm -rf "$tmp/target"
	mkdir "$tmp/target"
	tar xzf "$tmp/spk/package.tgz" -C "$tmp/target"
}
export SYNOPKG_PKGNAME=emerge-monitor SYNOPKG_PKGDEST="$tmp/target"
export SYNOPKG_TEMP_UPGRADE_FOLDER="$tmp/upgrade" SYNOPKG_TEMP_LOGFILE="$tmp/msg"
export PKG_USER
PKG_USER=$(id -un)
mkdir -p "$SYNOPKG_TEMP_UPGRADE_FOLDER"
if [ "$sh" = busybox ]; then
	mkdir "$tmp/bb"
	busybox --install -s "$tmp/bb"
	bb=$(command -v busybox) || fail "busybox not found"
	run() { PATH="$tmp/bb" "$bb" sh "$tmp/spk/scripts/$1" ${2:+"$2"}; }
else
	run() { "$sh" "$tmp/spk/scripts/$1" ${2:+"$2"}; }
fi
var=$tmp/target/var
port=$((20000 + $$ % 20000))
url="http://127.0.0.1:$port"

unpack
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$tmp/target/app/config" ||
	fail "app/config is not valid JSON"
[ -x "$tmp/target/bin/emon-server" ] || fail "no binary in package.tgz"

# install with the token from the wizard
SYNOPKG_PKG_STATUS=INSTALL wizard_token=s3cr3t-token-from-agents run postinst
[ "$(cat "$var/token")" = s3cr3t-token-from-agents ] || fail "wizard token not stored"
if [ "$(stat -c %a "$var")" != 700 ] || [ "$(stat -c %a "$var/token")" != 600 ]; then
	fail "permissions: $(stat -c '%n %a' "$var" "$var/token")"
fi
sed -i "s/^LISTEN=.*/LISTEN=\"127.0.0.1:$port\"/" "$var/emon.conf"

run start-stop-status status && fail "status: running before start"
run start-stop-status start || fail "start: $(cat "$var/emon-server.log")"
run start-stop-status status || fail "status: not running after start"
run start-stop-status start || fail "second start"
curl -fs "$url/" | grep -q '<title>emerge monitor</title>' || fail "dashboard"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
	-H 'Authorization: Bearer s3cr3t-token-from-agents' \
	--data-binary @"$top/tests/data/emerge.log" "$url/api/v1/ingest?host=gentoo1")
[ "$code" = 200 ] || fail "ingest with the token: HTTP $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Authorization: Bearer x' \
	"$url/api/v1/ingest?host=gentoo1")
[ "$code" = 401 ] || fail "ingest with a bad token: HTTP $code"
[ "$(run start-stop-status log)" = "$var/emon-server.log" ] || fail "log action"
run start-stop-status stop
run start-stop-status status && fail "status: running after stop"

# a stale pid file pointing at some other process
sleep 30 &
echo $! > "$var/emon-server.pid"
run start-stop-status status && fail "status: stale pid taken for emon-server"
kill $!

# upgrade: DSM stops the package, saves, replaces target/, reinstalls
run preupgrade
unpack
SYNOPKG_PKG_STATUS=UPGRADE run postinst
run postupgrade
[ "$(cat "$var/token")" = s3cr3t-token-from-agents ] || fail "token lost on upgrade"
grep -q "127.0.0.1:$port" "$var/emon.conf" || fail "settings lost on upgrade"
[ ! -f "$var/emon-server.pid" ] || fail "pid file carried over the upgrade"
[ ! -s "$tmp/msg" ] || fail "upgrade showed a message: $(cat "$tmp/msg")"
run start-stop-status start || fail "start after upgrade: $(cat "$var/emon-server.log")"
curl -fs "$url/api/v1/hosts" | grep -q '"name":"gentoo1"' || fail "database lost on upgrade"
run start-stop-status stop

# fresh install without a token: one is generated and the user is told
unpack
SYNOPKG_PKG_STATUS=INSTALL run postinst
grep -Eqx '[0-9a-f]{48}' "$var/token" || fail "generated token: $(cat "$var/token")"
grep -q "new agent token" "$tmp/msg" || fail "no message about the new token"

echo "test-spk ($sh): ok"
