// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Compiles the structured-exception shim with the MSVC compiler against the
//! WDK kernel-mode headers.

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let config = wdk_build::Config::from_env_auto()?;

    println!("cargo::rerun-if-changed=shim/seh.c");
    let mut build = cc::Build::new();
    build.file("shim/seh.c");
    for path in config.include_paths()? {
        build.include(path);
    }
    for (name, value) in config.preprocessor_definitions() {
        // The compiler defines _KERNEL_MODE itself under /kernel.
        if name != "_KERNEL_MODE" {
            build.define(&name, value.as_deref());
        }
    }
    build
        // Windows 10 version 2004 is the oldest system the driver supports.
        .define("NTDDI_VERSION", "0x0A000008")
        .define("_WIN32_WINNT", "0x0A00")
        .define("WINVER", "0x0A00")
        .define("WINNT", "1")
        // Kernel-mode code generation; also defines _KERNEL_MODE.
        .flag("/kernel")
        // Stack cookies, as in every WDK driver build.
        .flag("/GS")
        .flag("/W4")
        .flag("/WX")
        // No default library names in the object: the driver links no CRT.
        .flag("/Zl")
        .cargo_metadata(true)
        .compile("sar_seh_shim");
    Ok(())
}
