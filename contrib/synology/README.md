# emon-server on a Synology NAS

A DSM 6 package (`.spk`) that runs `emon-server` on a Synology NAS, with an
entry in the DSM main menu that opens the dashboard. The agents keep running
on the Gentoo machines; only their `-s` URL changes.

Built and tested for the **qoriq** platform (DS213+ and other Freescale
P1022 models: PowerPC e500v2, 32-bit big-endian, DSM 6.2, Linux 2.6.32).
Other platforms need their own binary, see [Other models](#other-models).

## Install

1. Get `emerge-monitor-qoriq-<version>.spk`: from the artifacts of the
   **CI** workflow on GitHub (job *synology-qoriq*), or build it (below).
2. DSM → **Package Center** → **Manual Install** → pick the file. DSM warns
   that the package has no Synology signature; that is expected for
   third-party packages.
3. The wizard asks for the **agent token**: paste the content of
   `/etc/emerge-monitor/token` from your Gentoo machines, or leave it empty
   to have one generated (Package Center then says where to read it).
4. On every Gentoo machine point the agent at the NAS, e.g.
   `EMON_SERVER=http://magrathea.lan.home:8080`, and restart it.
5. Open **emerge monitor** from the DSM main menu (or `http://NAS:8080/`).

The server runs as the unprivileged user `emerge-monitor`. Its data lives in
`/var/packages/emerge-monitor/target/var/`:

| file | |
|---|---|
| `emon.db` | the database (SQLite) |
| `token` | the agents' shared token |
| `emon.conf` | `LISTEN` (address:port) and `EXTRA_OPTS`; restart the package after editing |
| `emon-server.log` | server log, also shown by Package Center → *View Log* |

DSM 6 replaces the package directory on upgrades; the package saves this
directory before and restores it after, so installing a newer `.spk` keeps
the history and the token. Uninstalling deletes it.

To move from another server, stop both, copy the old `emon.db` over
`/var/packages/emerge-monitor/target/var/emon.db` (as root, then
`chown emerge-monitor` it) and start the package again.

### HTTPS

DSM can put the dashboard behind HTTPS with its own certificate: Control
Panel → **Application Portal** → **Reverse Proxy**, from
`https://NAS:<port>` to `http://localhost:8080`. Agents can then use the
`https://` URL. Keep the dashboard on your LAN: it has no login of its own,
and DSM 6.2 no longer gets regular security updates.

The firewall rule (Control Panel → Security → Firewall) can select
"emerge monitor (HTTP)" from the list of applications.

## Build

On Debian/Ubuntu (what CI uses):

```sh
sudo apt-get install gcc-powerpc-linux-gnu tcl git make \
     qemu-user libcurl4-openssl-dev libsqlite3-dev pkg-config
contrib/synology/build-qoriq.sh --test
contrib/synology/make-spk.sh build/qoriq/emon-server.stripped
```

`build-qoriq.sh` builds, from pinned and verified sources:

- **musl** 1.2.5 and a compiler runtime from LLVM's **compiler-rt**, both
  soft-float: the e500v2 has no classic FPU (it has SPE instead), so a normal
  PowerPC binary dies with *Illegal instruction* on the first floating point
  operation, and Debian's PowerPC `libgcc` is hard-float. A static musl binary
  also runs on DSM 6's 2.6.32 kernel, which current glibc refuses.
- **SQLite** 3.46.1, built into the binary (`make SQLITE_SRC=...`).
- `emon-server`, then checks that it contains no FPU instructions.

With `--test` it runs the unit tests and the full integration test against
the PowerPC server under `qemu-ppc -cpu e500v2`, which faults on the same
instructions the real CPU lacks.

`test-spk.sh` checks the package itself: it unpacks the `.spk` the way DSM
does and runs its scripts (install, start/stop, upgrade with data carried
over, install without a token), also with BusyBox's shell and tools only.

## Other models

The package is not tied to qoriq beyond the binary. For another platform,
build a static `emon-server` for its CPU and pass the platform name:
`make-spk.sh path/to/emon-server OUT_DIR <platform>` (the names are listed
by Synology, e.g. `apollolake`, `rtd1296`). The package scripts target DSM 6;
DSM 7 packages differ (run-as rules, `var` directory) and would need changes.
