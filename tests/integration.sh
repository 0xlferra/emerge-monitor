#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# End-to-end test: run emon-server, feed it the sample log with emon-agent,
# then check the JSON API.
set -eu
cd "$(dirname "$0")/.."

tmp=$(mktemp -d)
port=$((20000 + $$ % 20000))
url="http://127.0.0.1:$port"
agent_pid=
trap 'kill $srv $agent_pid 2>/dev/null || true; rm -rf "$tmp"' EXIT

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
		-l "$tmp/emerge.log" -S "$tmp/state" -T "$tmp" "$@" --once
}
# wait_for CMD...: retry for up to 10 seconds
wait_for() {
	i=0
	until "$@"; do
		i=$((i + 1))
		[ $i -lt 50 ] || return 1
		sleep 0.2
	done
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

# --- live build.log -------------------------------------------------------
atom=dev-lang/rust-1.80.0
blog="$tmp/portage/$atom/temp/build.log"
mkdir -p "$(dirname "$blog")"
printf ' * Package:    %s\n\033[32;01m*\033[0m Compiling\n' "$atom" > "$blog"
cat >> "$tmp/emerge.log" <<EOL
1700030000:  *** emerge --oneshot dev-lang/rust
1700030001:  >>> emerge (1 of 1) $atom to /
EOL
./emon-agent -s "$url" -n testbox -t "$tmp/token" -l "$tmp/emerge.log" \
	-S "$tmp/state" -T "$tmp" -i 1 2>"$tmp/agent.log" &
agent_pid=$!

log_matches() {
	curl -fs "$url/api/v1/buildlog?host=testbox&atom=$atom" > "$tmp/got" 2>/dev/null &&
		cmp -s "$tmp/got" "$blog"
}
wait_for log_matches || fail "build.log not received: $(cat "$tmp/got" "$tmp/agent.log")"

seq 1 2000 | sed 's/^/compiling unit /' >> "$blog"
wait_for log_matches || fail "build.log not followed"

# A burst of output: the agent may skip ahead and the server keeps at most
# 512 KiB, but what it serves must be the exact end of the file.
seq 1 100000 | sed 's/^/a long build log line /' >> "$blog"
tail_matches() {
	curl -fs "$url/api/v1/buildlog?host=testbox&atom=$atom" > "$tmp/got" 2>/dev/null || return 1
	n=$(wc -c < "$tmp/got")
	[ "$n" -gt 0 ] && [ "$n" -le 524288 ] && tail -c "$n" "$blog" | cmp -s "$tmp/got" -
}
wait_for tail_matches || fail "large build.log: got $(wc -c < "$tmp/got") bytes"

# The build fails: its last lines must still arrive after it stops building.
echo "error: could not compile" >> "$blog"
cat >> "$tmp/emerge.log" <<EOL
1700030100:  *** exiting unsuccessfully with status '1'.
1700030100:  *** terminating.
EOL
wait_for tail_matches || fail "final build.log lines missing"
wait_for sh -c "! curl -fs '$url/api/v1/hosts' | grep -q '\"building\":\[{'" ||
	fail "package still shown as building"

# Incremental reads and their offsets.
size=$(wc -c < "$blog")
hdr=$(curl -fs -D - -o /dev/null "$url/api/v1/buildlog?host=testbox&atom=$atom&offset=$size")
echo "$hdr" | grep -qi "^X-Log-Start: $size" || fail "offset: $hdr"
echo "$hdr" | grep -qi "^X-Log-End: $size" || fail "end: $hdr"

# Atoms are used to build paths on the agent: reject anything odd.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Authorization: Bearer secret' \
	"$url/api/v1/buildlog?host=testbox&atom=../../etc/passwd")
[ "$code" = 400 ] || fail "expected 400 for bad atom, got $code"

echo "integration: ok"
