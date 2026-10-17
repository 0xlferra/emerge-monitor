# emerge-monitor

Monitor `emerge` sessions across all your Gentoo machines from one web dashboard.

- **emon-agent** runs on each Gentoo box, follows `/var/log/emerge.log` and
  ships new lines to the server. It is a small C program depending only on
  libcurl.
- **emon-server** parses the logs, stores sessions and packages in SQLite and
  serves a JSON API plus a dashboard. It is a small C program depending only
  on SQLite.

The dashboard shows, for every host:

- whether an emerge is running, its command line, progress (`n of m`) and the
  packages currently building, with elapsed time vs. the average of the last
  builds of the same package on that host
- the result of the last session (success / failed / interrupted)
- the last `emerge --sync` and when the agent last reported

Click a host to see its session history, and a session to see every package
with its build time. The **log** button next to a package that is building
opens its `build.log`, updated live while it compiles (also kept for an hour
after a failed build, so you can see why it failed).

```
 gentoo-a: emon-agent ─┐
 gentoo-b: emon-agent ─┼── HTTP POST /api/v1/ingest ──► emon-server ──► SQLite
 gentoo-c: emon-agent ─┘                                     │
                                              browser ◄──────┘ GET /
```

## Building

Dependencies: a C11 compiler, `make`, `pkg-config`, `dev-db/sqlite` (server)
and `net-misc/curl` (agent).

```sh
make                 # builds emon-server and emon-agent
make check           # parser unit tests + end-to-end test
make install PREFIX=/usr
```

`make emon-agent` or `make emon-server` builds only one of them. A live
ebuild is in [`contrib/gentoo`](contrib/gentoo), see [Setup](#setup).

## Setup

Neither program needs root. The service files shipped in `contrib/` run both
daemons as the unprivileged user `emon`:

| | needs |
|---|---|
| emon-agent | read `/var/log/emerge.log` (`root:portage`, mode `0660`, so `emon` must be in group `portage`) and the build logs in `PORTAGE_LOGDIR` (also via group `portage`, see [Build logs](#build-logs)), read the token, write `/var/lib/emerge-monitor/agent.state` |
| emon-server | read the token, write `/var/lib/emerge-monitor/emon.db` |

### With the ebuild

`contrib/gentoo` is a small overlay holding `app-portage/emerge-monitor` plus
`acct-user/emon` and `acct-group/emon`, which create the user (with
`portage` as a supplementary group). Clone it somewhere outside `/home`,
e.g. `git clone https://github.com/0xlferra/emerge-monitor.git
/var/db/repos/emerge-monitor-src`, and add it as a local repository in
`/etc/portage/repos.conf/emerge-monitor.conf`:

```ini
[emerge-monitor]
location = /var/db/repos/emerge-monitor-src/contrib/gentoo
```

Keep every repository readable by the agent user: the agent runs
`portageq`, which reads all of them, and the systemd unit hides `/home`
(`ProtectHome=`). A repository under a home directory makes `portageq` fail;
the agent then says so at startup and falls back to defaults.

then `emerge app-portage/emerge-monitor` (USE `agent`, `server` or both;
it is a live `9999` ebuild, so it needs `**` keywords). The ebuild creates
`/var/lib/emerge-monitor` (`emon:emon`, `0750`) and `/etc/emerge-monitor`
(`root:emon`, `0750`).

### By hand

```sh
make && make install PREFIX=/usr
groupadd -r emon
useradd -r -g emon -G portage -d /dev/null -s /sbin/nologin emon
install -d -o emon -g emon -m 0750 /var/lib/emerge-monitor
install -d -o root -g emon -m 0750 /etc/emerge-monitor
```

On the server machine you can drop `-G portage` if no agent runs there.

### Build logs

To show the live build log, the agent has to read it, and with the default
Portage setup it cannot: Portage forces `$PORTAGE_TMPDIR/portage` and
`$PORTAGE_TMPDIR/portage/<category>` to mode `0700 portage:portage`, so
`temp/build.log` is out of reach for anyone but `portage` and root, even
though the file itself is group-readable.

Set `PORTAGE_LOGDIR` in `/etc/portage/make.conf` on every machine running an
agent:

```sh
PORTAGE_LOGDIR="/var/log/portage"
```

Portage then writes each build log there while the package builds (the
directory is created `2770 portage:portage`, so group `portage` can read it)
and `temp/build.log` becomes a symlink to it. Both the default layout and
`FEATURES="split-log"` work; `FEATURES="compress-build-logs"` does not, since a
log that is being gzipped cannot be followed. The logs are kept after the
build, so you may want to clean old ones up, e.g. with `PORTAGE_LOGDIR_CLEAN`
(see `make.conf(5)`).

The agent finds the directory by itself (`$PORTAGE_LOGDIR`, else
`portageq envvar PORTAGE_LOGDIR` at startup) and prints where it reads build
logs from; restart it after changing `make.conf`. It warns at startup if the
directory is not readable, or if it lies inside `$PORTAGE_TMPDIR/portage`,
which Portage resets to `0700` at every build and so can never work.

To keep the logs in RAM, give them a tmpfs of their own (mount options set
the permissions, because Portage only sets them on a directory it creates;
`250` is usually the `portage` uid/gid, check with `id portage`):

```
tmpfs  /var/log/portage  tmpfs  size=1G,mode=2770,uid=250,gid=250,noatime  0 0
```

or mount one tmpfs a level higher and put both builds and logs on it, with
`PORTAGE_TMPDIR="/var/tmp/emerge"` and `PORTAGE_LOGDIR="/var/tmp/emerge/logs"`. Without `PORTAGE_LOGDIR` it
falls back to `$PORTAGE_TMPDIR/portage/<cat>/<pf>/temp/build.log`, which only
works if the agent runs as `portage` or root. Running it as `portage` is
possible but not recommended: that user owns the build directories whose
contents get installed as root, so a compromised network-facing agent could
tamper with builds.

### Configuration

1. Generate a shared token once and put the same file on the server and on
   every agent, readable by `emon` but not by other users:

   ```sh
   head -c 32 /dev/urandom | base64 > /etc/emerge-monitor/token
   chown root:emon /etc/emerge-monitor/token
   chmod 0640 /etc/emerge-monitor/token
   ```

   With systemd the token may stay `root:root 0600`: the units read it with
   `LoadCredential=` and pass the service a private copy.

2. On the server, with OpenRC (`contrib/openrc/emon-server.{initd,confd}`):

   ```sh
   rc-update add emon-server default && rc-service emon-server start
   ```

   or systemd: `systemctl enable --now emon-server`.

3. On each Gentoo machine, set `EMON_SERVER` in `/etc/conf.d/emon-agent`
   (OpenRC) or `/etc/emerge-monitor/agent.env` (systemd, see
   `contrib/systemd/agent.env.example`), then start `emon-agent` the same way.

4. Open `http://monitor.lan:8080/`.

To try it out as a normal user, point every path somewhere you can write:

```sh
./emon-server -l 127.0.0.1:8080 -d ~/emon.db -t ~/emon.token
./emon-agent  -s http://127.0.0.1:8080 -t ~/emon.token -S ~/emon-agent.state
```

(the agent then needs your user to be in group `portage`).

On its first run the agent uploads the whole existing `emerge.log`, so the
history (and the build-time averages) are available right away. Use
`--from-end` to skip it.

### Options

| emon-agent | |
|---|---|
| `-s, --server URL` | server base URL (required) |
| `-n, --name NAME` | host name to report (default: short hostname) |
| `-t, --token-file PATH` | shared token file (or `EMON_TOKEN` env var) |
| `-l, --log PATH` | log to follow (default `/var/log/emerge.log`) |
| `-S, --state PATH` | where the read position is kept (default `/var/lib/emerge-monitor/agent.state`) |
| `-i, --interval SEC` | poll interval (default 2) |
| `-e, --from-end` | on first run, skip existing history |
| `-L, --portage-logdir DIR` | Portage's `PORTAGE_LOGDIR`, where build logs are read from (default: `$PORTAGE_LOGDIR`, else asked to `portageq`; `""` to use `PORTAGE_TMPDIR`) |
| `-T, --portage-tmpdir DIR` | Portage's `PORTAGE_TMPDIR`, used when there is no `PORTAGE_LOGDIR` and to check that `PORTAGE_LOGDIR` is not inside it (default: `$PORTAGE_TMPDIR`, else asked to `portageq`, else `/var/tmp`) |
| `-B, --no-build-logs` | do not send the `build.log` of running builds |
| `-1, --once` | upload what is there and exit (useful from cron) |

| emon-server | |
|---|---|
| `-l, --listen ADDR` | listen address (default `0.0.0.0:8080`, `[::]:8080` for IPv6) |
| `-d, --db PATH` | SQLite database (default `/var/lib/emerge-monitor/emon.db`) |
| `-t, --token-file PATH` | shared token file (or `EMON_TOKEN` env var) |

## Security notes

- The token only protects **ingest**; the dashboard and read API are open to
  anyone who can reach the port. Bind to a trusted network or put the server
  behind a reverse proxy (nginx, caddy) with TLS and authentication.
- The built-in HTTP server speaks plain HTTP. Use `https://` URLs on the agent
  side when a TLS reverse proxy is in front; libcurl handles it.
- The dashboard shows build logs to anyone who can open it. They hold
  compiler output, paths and parts of the build environment; use
  `--no-build-logs` on an agent if that is a concern.
- Do not run either daemon as root; see [Setup](#setup) for the permissions
  they need. Membership in group `portage` gives read access to
  `emerge.log` and some of Portage's state, nothing more.

## How it works

The agent never parses anything: it sends complete lines along with the log's
inode and the byte offset of the chunk (`X-Log-Inode`, `X-Log-Offset`). The
server remembers how far it has read each host's log, so a chunk re-sent after
an agent crash is de-duplicated. On log rotation the agent finishes the old
file before switching to the new one. An empty POST every 60 seconds acts as a
heartbeat; a host is shown offline after 3 minutes of silence.

Recognised log lines (see [`src/common/emerge_log.c`](src/common/emerge_log.c)):

| line | meaning |
|---|---|
| `*** emerge …` | session starts |
| `>>> emerge (i of n) cat/pkg-ver to /` | package build starts |
| `::: completed emerge (i of n) cat/pkg-ver to /` | package merged |
| `*** exiting successfully.` / `unsuccessfully with status 'N'.` | session ends |
| `*** terminating.` | session ends (interrupted, if still open) |
| `=== Sync completed for REPO` | repository synced |

A package still building when its session ends is marked *failed* (session
failed) or *aborted* (session interrupted).

### Live build logs

The reply to every ingest request lists the packages the server thinks are
building on that host, with the time emerge started each one. The agent
follows each package's build log, starting with its last 64 KiB, and posts new
output to `/api/v1/buildlog`. In `PORTAGE_LOGDIR` the log of the current build
is told apart from older logs of the same version (an earlier failed attempt)
by the start time Portage puts in the file name. Atoms are validated on both sides so the server cannot make
the agent read anything outside that directory. When a package leaves the list
the agent sends the file's last lines, then stops.

The server keeps the last 512 KiB of each log **in memory only** (at most 64
logs, dropped after an hour without updates), so restarting the server loses
them. If a build writes faster than the agent uploads and it falls more than
1 MiB behind, the agent skips ahead to the end; the viewer then shows
"earlier output not available".

### API

| endpoint | |
|---|---|
| `POST /api/v1/ingest?host=NAME` | raw `emerge.log` lines (agent only, needs `Authorization: Bearer TOKEN`) |
| `GET /api/v1/hosts` | all hosts with running session and last result |
| `GET /api/v1/sessions?host=NAME&limit=N` | session history of a host |
| `GET /api/v1/packages?session=ID` | packages of a session |
| `POST /api/v1/buildlog?host=NAME&atom=CAT/PF` | `build.log` bytes starting at `X-Log-Offset` (agent only) |
| `GET /api/v1/buildlog?host=NAME&atom=CAT/PF&offset=N` | buffered `build.log` text from offset `N`; `X-Log-Start`/`X-Log-End` give the file offsets returned |

## Roadmap ideas

- notifications (mail / webhook / ntfy) when a session fails
- show the current ebuild phase (configure / compile / install) in the card
- `@world` update counts from `emerge -pvuDN @world`
- multiple tokens, one per host

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
