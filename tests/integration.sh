#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# End-to-end test: run emon-server, feed it the sample log with emon-agent,
# then check the JSON API.
#
# EMON_SERVER and EMON_AGENT override how the programs are run, e.g. to
# test a cross-built server under qemu:
#   EMON_SERVER="qemu-ppc -cpu e500v2 ./emon-server" tests/integration.sh
set -eu
cd "$(dirname "$0")/.."

EMON_SERVER=${EMON_SERVER:-./emon-server}
EMON_AGENT=${EMON_AGENT:-./emon-agent}
tmp=$(mktemp -d)
port=$((20000 + $$ % 20000))
url="http://127.0.0.1:$port"
agent_pid=
trap 'kill $srv $agent_pid 2>/dev/null || true; rm -rf "$tmp"' EXIT

echo secret > "$tmp/token"
$EMON_SERVER -l "127.0.0.1:$port" -d "$tmp/db" -t "$tmp/token" 2>"$tmp/server.log" &
srv=$!
for _ in $(seq 1 50); do
	curl -fs "$url/api/v1/hosts" >/dev/null 2>&1 && break
	sleep 0.2
done

fail() { echo "integration: FAIL: $*" >&2; cat "$tmp/server.log" >&2; exit 1; }
agent() {
	$EMON_AGENT -s "$url" -n testbox -t "$tmp/token" \
		-l "$tmp/emerge.log" -S "$tmp/state" -T "$tmp" -L "" "$@" --once \
		2>>"$tmp/agent-once.log"
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
$EMON_AGENT -s "$url" -n testbox -t "$tmp/token" -l "$tmp/emerge.log" \
	-S "$tmp/state" -T "$tmp" -L "" -i 1 2>"$tmp/agent.log" &
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

# --- build logs from PORTAGE_LOGDIR ----------------------------------------
# Portage keeps PORTAGE_TMPDIR/portage/<cat> at 0700, so with PORTAGE_LOGDIR
# set the agent reads the logs from there instead. Two parallel builds: one
# in the default layout, one with FEATURES=split-log.
kill $agent_pid
wait $agent_pid 2>/dev/null || true
plog="$tmp/plog"
mkdir -p "$plog/build/dev-libs"
# 1700040001 = 2023-11-15 09:20:01 UTC
echo "OLD failed attempt" > "$plog/dev-libs:foo-2.0:20231115-090000.log"
printf 'foo: configuring\n' > "$plog/dev-libs:foo-2.0:20231115-092005.log"
printf 'bar: compiling\n' > "$plog/build/dev-libs/bar-1.0:20231115-092003.log"
cat >> "$tmp/emerge.log" <<EOL
1700040000:  *** emerge --jobs=2 dev-libs/foo dev-libs/bar
1700040001:  >>> emerge (1 of 2) dev-libs/foo-2.0 to /
1700040001:  >>> emerge (2 of 2) dev-libs/bar-1.0 to /
EOL
$EMON_AGENT -s "$url" -n testbox -t "$tmp/token" -l "$tmp/emerge.log" \
	-S "$tmp/state" -T "$tmp" -L "$plog" -i 1 2>"$tmp/agent.log" &
agent_pid=$!

served() {
	curl -fs "$url/api/v1/buildlog?host=testbox&atom=$1" 2>/dev/null
}
foo_ok() { [ "$(served dev-libs/foo-2.0)" = "foo: configuring" ]; }
bar_ok() { [ "$(served dev-libs/bar-1.0)" = "bar: compiling" ]; }
wait_for foo_ok || fail "PORTAGE_LOGDIR log: '$(served dev-libs/foo-2.0)' $(cat "$tmp/agent.log")"
wait_for bar_ok || fail "split-log log: '$(served dev-libs/bar-1.0)' $(cat "$tmp/agent.log")"
grep -q "reading build logs from $plog" "$tmp/agent.log" || fail "agent.log: $(cat "$tmp/agent.log")"

# A PORTAGE_LOGDIR the agent cannot use is reported at startup.
startup_msg() {
	$EMON_AGENT -s "$url" -n testbox -t "$tmp/token" -l "$tmp/emerge.log" \
		-S "$tmp/state2" -T "$tmp" -L "$1" --once 2>&1 >/dev/null
}
startup_msg "$tmp/portage/logs" | grep -q "resets to mode 0700" ||
	fail "no warning for PORTAGE_LOGDIR inside PORTAGE_TMPDIR/portage"
startup_msg "$tmp/nonexistent" | grep -q "creates it at the next build" ||
	fail "no notice for missing PORTAGE_LOGDIR"
startup_msg "$tmp/token" | grep -q "not a directory" ||
	fail "no warning for PORTAGE_LOGDIR that is a file"
startup_msg "$plog" | grep -q "warning" &&
	fail "spurious warning for a good PORTAGE_LOGDIR"

# Without options or environment, both directories come from portageq.
mkdir -p "$tmp/bin"
cat > "$tmp/bin/portageq" <<EOL
#!/bin/sh
case "\$2" in
PORTAGE_TMPDIR) echo "$tmp/emerge" ;;
PORTAGE_LOGDIR) echo "$tmp/emerge/portage/logs" ;;
esac
EOL
chmod +x "$tmp/bin/portageq"
msg=$(env -u PORTAGE_TMPDIR -u PORTAGE_LOGDIR PATH="$tmp/bin:$PATH" \
	$EMON_AGENT -s "$url" -n testbox -t "$tmp/token" -l "$tmp/emerge.log" \
	-S "$tmp/state2" --once 2>&1 >/dev/null)
echo "$msg" | grep -q "PORTAGE_TMPDIR=$tmp/emerge (portageq)" ||
	fail "PORTAGE_TMPDIR source not reported: $msg"
echo "$msg" | grep -q "reading build logs from $tmp/emerge/portage/logs" ||
	fail "PORTAGE_LOGDIR not taken from portageq: $msg"
echo "$msg" | grep -q "inside $tmp/emerge/portage, which Portage resets" ||
	fail "PORTAGE_TMPDIR not taken from portageq: $msg"

# portageq failing (here: a repository it cannot stat) is reported, once,
# with its error message, and the agent falls back to the defaults.
cat > "$tmp/bin/portageq" <<'EOL'
#!/bin/sh
echo "Permission denied: 'stat('/home/someone/overlay')'" >&2
exit 13
EOL
msg=$(env -u PORTAGE_TMPDIR -u PORTAGE_LOGDIR PATH="$tmp/bin:$PATH" \
	$EMON_AGENT -s "$url" -n testbox -t "$tmp/token" -l "$tmp/emerge.log" \
	-S "$tmp/state2" --once 2>&1 >/dev/null)
echo "$msg" | grep -q "portageq envvar PORTAGE_TMPDIR failed (exit 13)" ||
	fail "portageq failure not reported: $msg"
echo "$msg" | grep -q "Permission denied: 'stat('/home/someone/overlay')'" ||
	fail "portageq error message not shown: $msg"
[ "$(echo "$msg" | grep -c "failed (exit")" -eq 1 ] ||
	fail "portageq failure reported more than once: $msg"
echo "$msg" | grep -q "PORTAGE_TMPDIR=/var/tmp (portageq failed)" ||
	fail "fallback not reported: $msg"
echo "$msg" | grep -q "PORTAGE_LOGDIR not used (portageq failed)" ||
	fail "PORTAGE_LOGDIR fallback not reported: $msg"

# An unset PORTAGE_LOGDIR (empty line, exit 1) is not an error.
cat > "$tmp/bin/portageq" <<EOL
#!/bin/sh
case "\$2" in
PORTAGE_TMPDIR) echo "$tmp/emerge" ;;
*) echo; exit 1 ;;
esac
EOL
msg=$(env -u PORTAGE_TMPDIR -u PORTAGE_LOGDIR PATH="$tmp/bin:$PATH" \
	$EMON_AGENT -s "$url" -n testbox -t "$tmp/token" -l "$tmp/emerge.log" \
	-S "$tmp/state2" --once 2>&1 >/dev/null)
echo "$msg" | grep -q "PORTAGE_LOGDIR not used (not set in Portage's configuration)" ||
	fail "unset PORTAGE_LOGDIR: $msg"
echo "$msg" | grep -q "failed" && fail "unset PORTAGE_LOGDIR reported as failure: $msg"

# Atoms are used to build paths on the agent: reject anything odd.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Authorization: Bearer secret' \
	"$url/api/v1/buildlog?host=testbox&atom=../../etc/passwd")
[ "$code" = 400 ] || fail "expected 400 for bad atom, got $code"

echo "integration: ok"
