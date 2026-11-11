# Copyright (c) 2026 Itty Bitty Binaries, Inc.
# SPDX-License-Identifier: MIT

# Runs the Windows-side cargo in the build directory (a copy of the workspace
# on the C: drive) with the arguments given. Used by build.sh and wcargo.sh.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot\..

$cargo = Join-Path $env:USERPROFILE '.cargo\bin\cargo.exe'
$rustc = Join-Path $env:USERPROFILE '.cargo\bin\rustc.exe'

# bindgen needs a libclang.dll of the build host's architecture. Visual
# Studio's own LLVM is used; nothing is installed for this build.
if (-not $env:LIBCLANG_PATH) {
    $hostTriple = (& $rustc -vV | Select-String '^host: ').ToString().Substring(6)
    $archDir = if ($hostTriple -like 'aarch64*') { 'ARM64' } else { 'x64' }
    foreach ($edition in 'Community', 'Professional', 'Enterprise', 'BuildTools') {
        $candidate = "${env:ProgramFiles}\Microsoft Visual Studio\2022\$edition\VC\Tools\Llvm\$archDir\bin"
        if (Test-Path (Join-Path $candidate 'libclang.dll')) {
            $env:LIBCLANG_PATH = $candidate
            break
        }
    }
    if (-not $env:LIBCLANG_PATH) { throw 'libclang.dll not found; set LIBCLANG_PATH' }
}

$ErrorActionPreference = 'Continue'
& $cargo @args 2>&1 | ForEach-Object { "$_" }
exit $LASTEXITCODE
