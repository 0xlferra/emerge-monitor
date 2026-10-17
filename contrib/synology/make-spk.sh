#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Assemble the DSM 6 package (.spk) around an emon-server binary.
#
# usage: contrib/synology/make-spk.sh EMON_SERVER_BINARY [OUT_DIR [ARCH]]
# ARCH is the Synology platform the binary is for (default qoriq).
set -eu

[ $# -ge 1 ] || { echo "usage: $0 EMON_SERVER_BINARY [OUT_DIR [ARCH]]" >&2; exit 2; }
bin=$1
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=$(mkdir -p "${2:-$top/build}" && cd "${2:-$top/build}" && pwd)
arch=${3:-qoriq}
spk=$here/spk

# DSM compares versions to offer upgrades: count commits so it only grows.
version="0.1-$(git -C "$top" rev-list --count HEAD 2>/dev/null || echo 0)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# package.tgz becomes /var/packages/emerge-monitor/target on the NAS
mkdir -p "$work/target/bin" "$work/target/app/images"
install -m 755 "$bin" "$work/target/bin/emon-server"
install -m 644 "$top/LICENSE" "$work/target/LICENSE"
install -m 644 "$spk/app/config" "$spk/app/emerge-monitor.sc" "$work/target/app/"
install -m 644 "$spk"/icons/emerge-monitor-*.png "$work/target/app/images/"

# Reproducible archives; word-split on purpose below.
tar_opts="--owner=0 --group=0 --numeric-owner --sort=name --mtime=@0"
mkdir -p "$work/spk"
# shellcheck disable=SC2086
(cd "$work/target" && tar cf - $tar_opts . | gzip -n -9) > "$work/spk/package.tgz"

cp -R "$spk/scripts" "$spk/conf" "$spk/WIZARD_UIFILES" "$work/spk/"
chmod 755 "$work/spk/scripts"/*
chmod 644 "$work/spk/scripts/common"
cp "$spk/icons/emerge-monitor-72.png" "$work/spk/PACKAGE_ICON.PNG"
cp "$spk/icons/emerge-monitor-256.png" "$work/spk/PACKAGE_ICON_256.PNG"

sed -e "s/@VERSION@/$version/" -e "s/@ARCH@/$arch/" "$spk/INFO.in" > "$work/spk/INFO"
echo "checksum=\"$(md5sum < "$work/spk/package.tgz" | cut -d' ' -f1)\"" >> "$work/spk/INFO"

name=emerge-monitor-$arch-$version.spk
# shellcheck disable=SC2086
(cd "$work/spk" && tar cf - $tar_opts INFO package.tgz scripts conf \
	WIZARD_UIFILES PACKAGE_ICON.PNG PACKAGE_ICON_256.PNG) > "$out/$name"
echo "$out/$name"
