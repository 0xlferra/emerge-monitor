#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# End-to-end test: run emon-server, feed it the sample log with emon-agent,
# then check the JSON API.
set -eu
cd "$(dirname "$0")/.."

tmp=$(mktemp -d)
port=$((20000 + $$ % 20000))
url="http://127.0.0.1:$port"
trap 'kill $srv 2>/dev/null || true; rm -rf "$tmp"' EXIT

echo secret > "$tmp/token"
./emon-server -l "127.0.0.1:$port" -d "$tmp/db" -t "$tmp/token" 2>"$tmp/server.log" &
srv=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	curl -fs "$url/api/v1/hosts" >/dev/null 2>&1 && break
	sleep 0.2
done

fail() { echo "integration: FAIL: $*" >&2; cat "$tmp/server.log" >&2; exit 1; }
agent() {
	./emon-agent -s "$url" -n testbox -t "$tmp/token" \
		-l "$tmp/emerge.log" -S "$tmp/state" --once
}

# Ship the log in two parts to exercise incremental reads.
head -n 15 tests/data/emerge.log > "$tmp/emerge.log"
agent || fail "agent run 1"
tail -n +16 tests/data/emerge.log >> "$tmp/emerge.log"
agent || fail "agent run 2"

# A re-send of everything (lost state file) must not duplicate anything.
rm "$tmp/state"
agent || fail "agent run 3"

# Wrong token is rejected.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Authorization: Bearer nope' \
	"$url/api/v1/ingest?host=testbox")
[ "$code" = 401 ] || fail "expected 401 for bad token, got $code"

hosts=$(curl -fs "$url/api/v1/hosts")
sessions=$(curl -fs "$url/api/v1/sessions?host=testbox")

echo "$hosts" | grep -q '"name":"testbox"' || fail "host missing: $hosts"
echo "$hosts" | grep -q '"last_sync":1700000060' || fail "last_sync: $hosts"
n=$(echo "$sessions" | grep -o '"id":' | wc -l)
[ "$n" -eq 4 ] || fail "expected 4 sessions, got $n: $sessions"
for st in '"status":"failed"' '"status":"success"' '"status":"interrupted"'; do
	echo "$sessions" | grep -q "$st" || fail "missing $st: $sessions"
done

# Session 2 (@world): zlib + python done, firefox failed.
pk=$(curl -fs "$url/api/v1/packages?session=2")
[ "$(echo "$pk" | grep -o '"status":"done"' | wc -l)" -eq 2 ] || fail "packages: $pk"
echo "$pk" | grep -q '"atom":"www-client/firefox-119.0.1".*"status":"failed"' || fail "firefox: $pk"
echo "$pk" | grep -q '"avg_duration":53' || fail "zlib average: $pk"

curl -fs "$url/" | grep -q '<title>emerge monitor</title>' || fail "dashboard"

echo "integration: ok"
