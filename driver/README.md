<!-- Copyright (c) 2026 Itty Bitty Binaries, Inc. SPDX-License-Identifier: MIT -->

# SynchronousAudioRouter.sys

The kernel driver of Synchronous Audio Router, written in Rust.

It was written from a behavioral specification, [docs/behavior.md](docs/behavior.md),
without reference to the C++ driver it replaces. Appendix A of that document
is the binary interface the driver shares with SAR's user-mode software; the
user-mode side of the same interface is `SynchronousAudioRouter/sar.h`.

Unlike the rest of this repository, everything under `driver/` is licensed
under the MIT license ([LICENSE](LICENSE)).

## Layout

| Crate | Role |
|---|---|
| `sar-core` | Everything that needs no kernel API: wire formats, validation, section geometry, the register update rule, format matching, the notification queue's logic. No `unsafe`; unit-tested on the development machine. |
| `ntstatus` | Status value types. |
| `ks-sys` | Raw kernel streaming bindings, generated from `ks.h` and `ksmedia.h`. |
| `ntx-sys` | The C shim for structured exception handling (`shim/seh.c`) and declarations no WDK header carries. |
| `ntk` | Safe wrappers over the kernel APIs the driver uses. |
| `ks` | Safe wrappers over kernel streaming (AVStream). |
| `sar-driver` | The driver. Only its entry point contains `unsafe`. |

`unsafe` is confined to `ntk`, `ks` and the `-sys` crates. The only C is
`ntx-sys/shim/seh.c`: four functions, each one `__try` block around an access
to user memory, which Rust cannot express.

## Building

The kernel crates build on a Windows host only (Microsoft's `wdk-sys` needs
one), with the Windows Driver Kit, a Rust toolchain for
`x86_64-pc-windows-msvc`, and a `libclang.dll` for bindgen (Visual Studio's
"C++ Clang tools" component provides one; otherwise set `LIBCLANG_PATH`).

On Windows, from this directory:

    cargo test
    cargo build --release --target x86_64-pc-windows-msvc -p sar-driver

The image is `target\x86_64-pc-windows-msvc\release\SynchronousAudioRouter.dll`;
`tools\Package-SarDriver.ps1` at the top of the repository turns it into a
driver package.

From WSL, `./build.sh` runs the unit tests there, mirrors the sources to the
Windows side, builds with the Windows cargo and leaves the image, its symbols
and the INF in `./package`.

## Documents

- [docs/behavior.md](docs/behavior.md): what the driver does, and its binary
  interface.
- [docs/SPEC-GAPS.md](docs/SPEC-GAPS.md): what that document left open when
  the driver was written from it, and how each point was settled.
- [docs/PROVENANCE.md](docs/PROVENANCE.md): the sources consulted while
  writing it.
