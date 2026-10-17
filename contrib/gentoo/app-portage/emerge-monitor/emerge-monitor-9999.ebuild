# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit git-r3 systemd toolchain-funcs

DESCRIPTION="Agent + server to monitor emerge sessions across Gentoo machines"
HOMEPAGE="https://github.com/0xlferra/emerge-monitor"
EGIT_REPO_URI="https://github.com/0xlferra/emerge-monitor.git"

LICENSE="GPL-2+"
SLOT="0"
IUSE="+agent server"
REQUIRED_USE="|| ( agent server )"

DEPEND="
	agent? ( net-misc/curl )
	server? ( dev-db/sqlite:3 )
"
RDEPEND="${DEPEND}
	acct-user/emon
"
BDEPEND="virtual/pkgconfig"

src_compile() {
	local targets=()
	use agent && targets+=( emon-agent )
	use server && targets+=( emon-server )
	emake CC="$(tc-getCC)" PKG_CONFIG="$(tc-getPKG_CONFIG)" "${targets[@]}"
}

src_test() {
	emake CC="$(tc-getCC)" tests/test_parser
	./tests/test_parser tests/data/emerge.log || die
}

src_install() {
	if use agent; then
		dobin emon-agent
		newinitd contrib/openrc/emon-agent.initd emon-agent
		newconfd contrib/openrc/emon-agent.confd emon-agent
		systemd_dounit contrib/systemd/emon-agent.service
	fi
	if use server; then
		dobin emon-server
		newinitd contrib/openrc/emon-server.initd emon-server
		newconfd contrib/openrc/emon-server.confd emon-server
		systemd_dounit contrib/systemd/emon-server.service
	fi
	keepdir /var/lib/emerge-monitor
	fowners emon:emon /var/lib/emerge-monitor
	fperms 0750 /var/lib/emerge-monitor

	keepdir /etc/emerge-monitor
	fowners root:emon /etc/emerge-monitor
	fperms 0750 /etc/emerge-monitor
	if use agent; then
		insinto /etc/emerge-monitor
		newins contrib/systemd/agent.env.example agent.env.example
	fi

	einstalldocs
}

pkg_postinst() {
	if [[ ! -e ${EROOT}/etc/emerge-monitor/token ]]; then
		elog "Put the same shared token on the server and on every agent:"
		elog "  head -c 32 /dev/urandom | base64 > /etc/emerge-monitor/token"
		elog "  chown root:emon /etc/emerge-monitor/token"
		elog "  chmod 0640 /etc/emerge-monitor/token"
		elog "Both daemons run as the unprivileged user 'emon'."
	fi
	if use agent; then
		elog "To see live build logs in the dashboard, set PORTAGE_LOGDIR"
		elog "(e.g. /var/log/portage) in make.conf: Portage keeps its build"
		elog "directories readable by user portage only."
	fi
}
