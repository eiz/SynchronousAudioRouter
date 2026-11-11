#!/usr/bin/env bash
# Copyright (c) 2026 Itty Bitty Binaries, Inc.
# SPDX-License-Identifier: MIT
# Mirrors the workspace sources into the Windows-side build directory and
# prints that directory. Sourced by build.sh and wcargo.sh.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
win_user="$(cd /mnt/c && cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')"
win_build="${SAR_WIN_BUILD_DIR:-/mnt/c/Users/$win_user/AppData/Local/Temp/sar-driver-build}"
src="$win_build/src"
mkdir -p "$src"
# Sources only: no build products, tools or results.
rsync -a --delete \
    --exclude '/target/' --exclude '/tools/' --exclude '/package/' \
    --exclude '/rig-results*' --exclude '/scratch/' --exclude '/.git/' \
    --exclude '/Cargo.lock' \
    "$here/" "$src/"
# The lock file is versioned in the workspace but maintained by the
# Windows-side cargo, which is the one that resolves the kernel crates.
if [ -f "$here/Cargo.lock" ] && ! cmp -s "$here/Cargo.lock" "$src/Cargo.lock" 2>/dev/null; then
    cp "$here/Cargo.lock" "$src/Cargo.lock"
fi
echo "$src"
