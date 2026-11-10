<#
.SYNOPSIS
    Build the SAR installer (MSI) from binaries that are already built.

.DESCRIPTION
    Two steps, so that a release can sign the custom action DLL in between:

      -Actions   Fetch the pinned WiX 3.14 binaries and build
                 SarInstallerActions.dll into -Bin64.
      -Msi       Build the MSI from the driver package in -DriverDir and the
                 user-mode binaries in -Bin64 and -Bin32, and write it to
                 -OutFile.

    Both can be given at once. msbuild has to be on the PATH.

    -Bin64 needs SarAsio.dll, SarConfigure.exe and SarInstallerActions.dll,
    -Bin32 needs SarAsio.dll, and -DriverDir needs SynchronousAudioRouter.sys,
    .inf and .cat. The driver has to be signed for the machines the MSI is
    meant for: Windows refuses to install an unsigned driver package.

.NOTES
    Designed for Windows PowerShell 5.1 and pwsh 7+.
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Actions,
    [switch]$Msi,

    [string]$Bin64,
    [string]$Bin32,
    [string]$DriverDir,

    # Four-field product version (x.y.z.w). Windows Installer compares the
    # first three; the fourth only labels the build.
    [string]$Version,
    [string]$OutFile,

    [string]$WixDir = (Join-Path ([IO.Path]::GetTempPath()) 'sar-wix314')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Pinned binaries zip rather than whatever the build machine carries.
$WixUrl = 'https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip'
$WixSha256 = '6ac824e1642d6f7277d0ed7ea09411a508f6116ba6fae0aa5f2c7daa2ff43d31'

$repo = Split-Path $PSScriptRoot -Parent

function Resolve-Dir([string]$Path, [string]$Name) {
    if (-not $Path) { throw "-$Name is required." }
    return (Resolve-Path $Path).ProviderPath
}

if (-not ($Actions -or $Msi)) { throw "Give -Actions, -Msi or both." }

if (-not (Test-Path (Join-Path $WixDir 'candle.exe'))) {
    $zip = Join-Path ([IO.Path]::GetTempPath()) 'sar-wix314.zip'
    Write-Host "==> Downloading WiX 3.14"
    Invoke-WebRequest $WixUrl -OutFile $zip
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($hash -ne $WixSha256) { throw "WiX zip hash mismatch: $hash" }
    Expand-Archive $zip $WixDir -Force
}

# SarInstallerActions.vcxproj expects $(WIX) with a trailing slash.
$env:WIX = "$WixDir\"

if ($Actions) {
    $bin64Dir = Resolve-Dir $Bin64 'Bin64'

    Write-Host "==> Building SarInstallerActions"
    & msbuild (Join-Path $repo 'SynchronousAudioRouter.sln') /t:SarInstallerActions `
        /p:Configuration=Release /p:Platform=x64 /m /verbosity:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed (exit $LASTEXITCODE)" }

    Copy-Item (Join-Path $repo 'x64\Release\SarInstallerActions.dll') $bin64Dir -Force
}

if ($Msi) {
    $bin64Dir = Resolve-Dir $Bin64 'Bin64'
    $bin32Dir = Resolve-Dir $Bin32 'Bin32'
    $driver = Resolve-Dir $DriverDir 'DriverDir'

    if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "-Version must be x.y.z.w, got '$Version'." }
    if (-not $OutFile) { throw "-OutFile is required." }

    # A response file keeps the semicolons in DefineConstants away from
    # PowerShell's argument quoting; msbuild splits them into -d defines.
    $rsp = Join-Path ([IO.Path]::GetTempPath()) 'sar-installer.rsp'
    @(
        "/p:Configuration=Release"
        "/p:Platform=x64"
        "/p:WixInstallPath=$env:WIX"
        "/p:WixTargetsPath=$env:WIX\wix.targets"
        "/p:DefineConstants=`"ProductVersion=$Version;Bin64=$bin64Dir;Bin32=$bin32Dir;DriverDir=$driver`""
    ) | Out-File $rsp -Encoding ascii

    Write-Host "==> Building the MSI ($Version)"
    & msbuild (Join-Path $repo 'SarInstaller\SarInstaller.wixproj') "@$rsp" /t:Rebuild /verbosity:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed (exit $LASTEXITCODE)" }

    $outDir = Split-Path $OutFile -Parent
    if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    Copy-Item (Join-Path $repo 'SarInstaller\bin\x64\Release\SynchronousAudioRouter.msi') $OutFile -Force
    Write-Host "==> $OutFile"
}
