# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit acct-user

DESCRIPTION="User for emerge-monitor"
ACCT_USER_ID=-1
# portage: lets emon-agent read /var/log/emerge.log
ACCT_USER_GROUPS=( emon portage )

acct-user_add_deps
