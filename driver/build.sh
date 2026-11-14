#!/usr/bin/env bash
# Copyright (c) 2026 Itty Bitty Binaries, Inc.
# SPDX-License-Identifier: MIT
# Builds SynchronousAudioRouter.sys from a clean checkout and assembles the
# package directory the test harness takes:
#
#     ./build.sh            release build  -> ./package
#     ./build.sh debug      debug build    -> ./package
#
# The kernel crates are built by the Windows-side cargo (Microsoft's wdk-sys
# can only be built on a Windows host), from a mirror of this workspace on
# the C: drive. The unit tests of the kernel-independent logic run first, on
# this machine.
set -euo pipefail

profile="${1:-release}"
case "$profile" in release|debug) ;; *) echo "usage: $0 [release|debug]" >&2; exit 2;; esac

here="$(cd "$(dirname "$0")" && pwd)"
package="$here/package"

(cd "$here" && cargo test --quiet -p ntstatus -p sar-core)

args=(build --target x86_64-pc-windows-msvc -p sar-driver)
if [ "$profile" = release ]; then args+=(--release); fi
"$here/wcargo.sh" "${args[@]}"

src="$("$here/scripts/sync-to-windows.sh")"
out="$src/target/x86_64-pc-windows-msvc/$profile"
rm -rf "$package"
mkdir -p "$package"
# cargo names a cdylib <name>.dll; a driver image is the same file as .sys.
cp "$out/SynchronousAudioRouter.dll" "$package/SynchronousAudioRouter.sys"
cp "$out/SynchronousAudioRouter.pdb" "$package/SynchronousAudioRouter.pdb"
cp "$here/inf/SynchronousAudioRouter.inf" "$package/SynchronousAudioRouter.inf"
echo "package: $package"
ls -l "$package"
