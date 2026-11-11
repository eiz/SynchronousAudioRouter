// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Link configuration for the driver image.
//!
//! This is the argument list of Microsoft's
//! `wdk_build::configure_wdk_binary_build()` for a WDM driver (wdk-build
//! 0.5.1, itself derived from the WDK's `WindowsDriver.KernelMode.props`),
//! written out by hand because three things have to differ and that function
//! offers no switch for them:
//!
//! * no `/INTEGRITYCHECK`. wdk-build forces it; a WDK driver project does
//!   not set it and nothing in this driver needs it. A later
//!   `/INTEGRITYCHECK:NO` does not undo it (tried; the image kept the flag).
//! * `/ENTRY:GsDriverEntry` rather than `DriverEntry`, so that the stack
//!   cookie used by the `/GS`-compiled C shim is initialised before any code
//!   runs. That is the entry point of every WDK-built driver.
//! * `/RELEASE` (image checksum) and a Windows 10 subsystem version.

fn main() -> Result<(), wdk_build::ConfigError> {
    let config = wdk_build::Config::from_env_auto()?;

    for path in config.library_paths()? {
        println!("cargo::rustc-link-search={}", path.display());
    }
    for lib in ["BufferOverflowFastFailK", "ntoskrnl", "hal", "wmilib"] {
        println!("cargo::rustc-link-lib=static={lib}");
    }

    for arg in [
        "/DRIVER",
        "/NODEFAULTLIB",
        "/SUBSYSTEM:NATIVE,10.00",
        "/KERNEL",
        "/ENTRY:GsDriverEntry",
        // LNK4257: object file was not compiled for kernel mode (rustc has
        // no /kernel). LNK4216: exported entry point (Rust cannot name a
        // symbol without exporting it from a cdylib).
        "/IGNORE:4257",
        "/IGNORE:4216",
        "/NXCOMPAT",
        "/DYNAMICBASE",
        "/MAP",
        "/MAPINFO:EXPORTS",
        "/OPT:REF,ICF",
        "/MANIFEST:NO",
        "/RELEASE",
        // The INIT section (GsDriverEntry) is only needed during load.
        "/SECTION:INIT,d",
    ] {
        println!("cargo::rustc-cdylib-link-arg={arg}");
    }

    // The `cfg`s Microsoft's wdk crates key their kernel-mode code on.
    config.configure_library_build()
}
