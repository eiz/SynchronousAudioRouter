<#
.SYNOPSIS
    Ensure the WDK kernel-mode platform toolset is available to MSBuild.

.DESCRIPTION
    GitHub's windows-2022 image ships the WDK (22H2) + VS2022, but a known
    runner-images bug (actions/runner-images#5970) can leave the
    "WindowsKernelModeDriver10.0" VS platform toolset *unregistered*, so the
    driver project fails to load. This script is defensive and fast:

      1. if the kernel toolset is already registered -> do nothing,
      2. else install the WDK.vsix that already ships on the image,
      3. only if no WDK is present at all -> download + install it (fallback).

    Keep $SdkVersion / $WdkFwlink in lockstep with the driver .vcxproj and the
    workflow's SDK_VERSION when you bump the target WDK.

.NOTES
    Idempotent. Works under Windows PowerShell 5.1 and pwsh 7+.
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$SdkVersion = '10.0.22621.0',
    # WDK for Windows 11, version 22H2 (fallback download only).
    [string]$WdkFwlink  = 'https://go.microsoft.com/fwlink/?linkid=2196230',
    [string]$WorkDir    = $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP })
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$VsixSearchRoots = @(
    'C:\Program Files (x86)\Windows Kits\10\Vsix',
    'C:\Program Files\Windows Kits\10\Vsix'
)

function Find-VsInstall {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: $vswhere" }
    $p = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
    if (-not $p) { throw "No Visual Studio installation found by vswhere." }
    return $p.Trim()
}

function Test-KernelToolset([string]$vs) {
    # The WDK VS extension registers the kernel-mode toolset here.
    $tp = Join-Path $vs 'MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0'
    return (Test-Path $tp)
}

function Find-WdkVsix {
    Get-ChildItem -Path $VsixSearchRoots -Recurse -Filter 'WDK.vsix' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
}

function Install-Vsix([string]$vs, [string]$vsixPath) {
    $vsixInstaller = Join-Path $vs 'Common7\IDE\VSIXInstaller.exe'
    if (-not (Test-Path $vsixInstaller)) { throw "VSIXInstaller.exe not found: $vsixInstaller" }
    Write-Host "==> Installing extension: $vsixPath"
    $proc = Start-Process -FilePath $vsixInstaller `
        -ArgumentList '/admin', '/quiet', "`"$vsixPath`"" -PassThru -Wait
    # 0 = installed, 1001 = already installed.
    if ($proc.ExitCode -notin 0, 1001) { throw "VSIXInstaller failed (exit $($proc.ExitCode))." }
}

$vs = Find-VsInstall
Write-Host "==> Visual Studio: $vs"

if (Test-KernelToolset $vs) {
    Write-Host "==> WindowsKernelModeDriver10.0 toolset already registered - nothing to do."
    return
}

Write-Host "==> Kernel toolset missing; looking for the WDK extension on the image..."
$vsix = Find-WdkVsix

if (-not $vsix) {
    Write-Host "==> No WDK found on image; downloading (fallback) from $WdkFwlink ..."
    $wdkSetup = Join-Path $WorkDir 'wdksetup.exe'
    Invoke-WebRequest -Uri $WdkFwlink -OutFile $wdkSetup -UseBasicParsing
    $proc = Start-Process -FilePath $wdkSetup `
        -ArgumentList '/q', '/norestart', '/ceip', 'off' -PassThru -Wait
    if ($proc.ExitCode -notin 0, 3010) { throw "WDK install failed (exit $($proc.ExitCode))." }
    $vsix = Find-WdkVsix
    if (-not $vsix) { throw "WDK.vsix not found even after install." }
}

Install-Vsix $vs $vsix.FullName

if (-not (Test-KernelToolset $vs)) {
    throw "WindowsKernelModeDriver10.0 still not registered after installing the WDK extension."
}
Write-Host "==> Kernel toolset registered. WDK build environment ready."
