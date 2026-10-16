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
RDEPEND="${DEPEND}"
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
	einstalldocs
}

pkg_postinst() {
	elog "Create a shared token on every machine, e.g.:"
	elog "  mkdir -p /etc/emerge-monitor"
	elog "  head -c 32 /dev/urandom | base64 > /etc/emerge-monitor/token"
	elog "  chmod 600 /etc/emerge-monitor/token"
}
