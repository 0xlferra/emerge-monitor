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
with its build time.

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
ebuild is in [`contrib/gentoo`](contrib/gentoo/app-portage/emerge-monitor),
with `agent` and `server` USE flags.

## Setup

1. Generate a shared token and copy it to the server and every agent:

   ```sh
   mkdir -p /etc/emerge-monitor
   head -c 32 /dev/urandom | base64 > /etc/emerge-monitor/token
   chmod 600 /etc/emerge-monitor/token
   ```

2. On the server:

   ```sh
   emon-server -l 0.0.0.0:8080 -d /var/lib/emerge-monitor/emon.db \
               -t /etc/emerge-monitor/token
   ```

   or with OpenRC: install `contrib/openrc/emon-server.{initd,confd}`, then
   `rc-update add emon-server default && rc-service emon-server start`.

3. On each Gentoo machine:

   ```sh
   emon-agent -s http://monitor.lan:8080 -t /etc/emerge-monitor/token
   ```

   or with OpenRC: install `contrib/openrc/emon-agent.{initd,confd}`, set
   `EMON_SERVER` in `/etc/conf.d/emon-agent`, then add it to the default
   runlevel. systemd units are in `contrib/systemd`.

4. Open `http://monitor.lan:8080/`.

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
- The agent only needs read access to `/var/log/emerge.log` (group `portage`)
  and write access to its state file, so it does not have to run as root.

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

### API

| endpoint | |
|---|---|
| `POST /api/v1/ingest?host=NAME` | raw `emerge.log` lines (agent only, needs `Authorization: Bearer TOKEN`) |
| `GET /api/v1/hosts` | all hosts with running session and last result |
| `GET /api/v1/sessions?host=NAME&limit=N` | session history of a host |
| `GET /api/v1/packages?session=ID` | packages of a session |

## Roadmap ideas

- notifications (mail / webhook / ntfy) when a session fails
- parse `/var/tmp/portage/*/*/temp/build.log` to show the current phase
- `@world` update counts from `emerge -pvuDN @world`
- multiple tokens, one per host

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
