#!/usr/bin/env bash
# Copyright (c) 2026 Itty Bitty Binaries, Inc.
# SPDX-License-Identifier: MIT
# Runs the Windows-side cargo on a mirror of this workspace, for example
#     ./wcargo.sh check --target x86_64-pc-windows-msvc -p ntk
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$("$here/scripts/sync-to-windows.sh")"
cd "$src"
status=0
powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$(wslpath -w "$src/scripts/cargo-windows.ps1")" "$@" || status=$?
if [ -f "$src/Cargo.lock" ] && ! cmp -s "$src/Cargo.lock" "$here/Cargo.lock" 2>/dev/null; then
    cp "$src/Cargo.lock" "$here/Cargo.lock"
fi
exit $status
