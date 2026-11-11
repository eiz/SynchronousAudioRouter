<#
.SYNOPSIS
    Stage, version-stamp, catalog and hash the built SynchronousAudioRouter driver
    into a submission-ready, verifiable package.

.DESCRIPTION
    Run AFTER cargo has built the driver (driver/README.md). This script:

      1. takes the driver image cargo built and its .pdb,
      2. stamps a valid DriverVer into a staged copy of the INF
         (the committed INF still carries the placeholder "DriverVer=0.1",
         which is illegal for a real package),
      3. checks that INF with InfVerif /h, which Partner Center requires
         attestation submissions to pass,
      4. generates the security catalog (.cat) with Inf2Cat,
      5. packs .sys/.inf/.cat/.pdb into a .cab laid out the way Partner
         Center expects for attestation signing: one folder per driver
         package, nothing at the root,
      6. writes SHA256SUMS.txt and BUILD-INFO.txt so the binary is
         traceable back to its exact source commit.

    The build is intentionally UNSIGNED (SignMode=Off). Signing is a separate,
    later step performed by the attestation service - see BUILDING.md.

.NOTES
    Designed for Windows PowerShell 5.1 and pwsh 7+.
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('x64', 'ARM64')] [string]$Platform,
    [string]$Configuration = 'Release',

    # Four-field DriverVer version (x.y.z.w), e.g. 0.14.0.137
    [Parameter(Mandatory)] [string]$DriverVersion,

    # Human-readable build label (e.g. `git describe` output), recorded in BUILD-INFO.
    [string]$Describe = 'dev',

    [string]$OutDir = 'artifacts',
    [string]$InfSource = 'driver/inf/SynchronousAudioRouter.inf',

    # Cargo's output directory for the driver, if it isn't the default
    # driver\target\<target triple>\<profile>.
    [string]$BuiltDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo  = (Resolve-Path '.').Path
$stage = Join-Path $repo (Join-Path $OutDir $Platform)
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# 1. The driver image. Cargo builds it as a cdylib, so it is named .dll; a
#    driver image is the same file with the extension .sys.
if (-not $BuiltDir) {
    $triple  = @{ 'x64' = 'x86_64-pc-windows-msvc'; 'ARM64' = 'aarch64-pc-windows-msvc' }[$Platform]
    $profile = if ($Configuration -eq 'Release') { 'release' } else { 'debug' }
    $BuiltDir = Join-Path $repo "driver\target\$triple\$profile"
}
$built = Join-Path $BuiltDir 'SynchronousAudioRouter.dll'
if (-not (Test-Path $built)) { throw "$built not found - did the driver build succeed?" }
Write-Host "==> Driver binary: $built"
Copy-Item $built (Join-Path $stage 'SynchronousAudioRouter.sys') -Force

# The symbols go into the submission too: Microsoft's crash analysis uses them.
$pdb = Join-Path $BuiltDir 'SynchronousAudioRouter.pdb'
if (-not (Test-Path $pdb)) { throw "$pdb not found - did the driver build succeed?" }
Copy-Item $pdb (Join-Path $stage 'SynchronousAudioRouter.pdb') -Force

# 2. Stamp a valid DriverVer into a staged copy of the INF.
$today = (Get-Date).ToString('MM/dd/yyyy')
$inf = Get-Content -Raw -Path (Join-Path $repo $InfSource)
$inf = [regex]::Replace($inf, '(?m)^\s*DriverVer\s*=.*$', "DriverVer=$today,$DriverVersion")
$infOut = Join-Path $stage 'SynchronousAudioRouter.inf'
Set-Content -Path $infOut -Value $inf -Encoding ASCII
Write-Host "==> Stamped DriverVer=$today,$DriverVersion"

# 3. Check the INF against the rules Partner Center applies to attestation
#    submissions, so a bad INF fails here and not after an upload.
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$infverif = Get-ChildItem "$kits\Tools", "$kits\bin" -Recurse -Filter 'infverif.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object FullName -Descending | Select-Object -First 1
if (-not $infverif) { throw "infverif.exe not found - is the WDK installed?" }
Write-Host "==> InfVerif /h ($($infverif.FullName)) ..."
& $infverif.FullName /v /h $infOut
if ($LASTEXITCODE -ne 0) { throw "InfVerif /h failed (exit $LASTEXITCODE)." }

# 4. Generate the security catalog (.cat) with Inf2Cat.
$inf2cat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" `
               -Recurse -Filter 'Inf2Cat.exe' -ErrorAction SilentlyContinue |
           Sort-Object FullName -Descending | Select-Object -First 1
if (-not $inf2cat) { throw "Inf2Cat.exe not found - is the WDK installed?" }
$osMap  = @{ 'x64' = '10_X64'; 'ARM64' = '10_ARM64' }
$osList = $osMap[$Platform]
Write-Host "==> Inf2Cat (/os:$osList) ..."
& $inf2cat.FullName "/driver:$stage" "/os:$osList" /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed (exit $LASTEXITCODE)." }

# 5. Build the .cab for attestation signing. Partner Center wants each driver
#    package in its own folder inside the cabinet, with no files at the root.
$cabName = "SynchronousAudioRouter_$Platform.cab"
$ddf = Join-Path $stage '_package.ddf'
@"
.OPTION EXPLICIT
.Set CabinetNameTemplate=$cabName
.Set DiskDirectory1=$stage
.Set CompressionType=MSZIP
.Set Cabinet=on
.Set Compress=on
.Set DestinationDir=SynchronousAudioRouter
"$stage\SynchronousAudioRouter.sys"
"$stage\SynchronousAudioRouter.inf"
"$stage\SynchronousAudioRouter.cat"
"$stage\SynchronousAudioRouter.pdb"
"@ | Set-Content -Path $ddf -Encoding ASCII
Write-Host "==> makecab -> $cabName"
& makecab /f $ddf | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makecab failed (exit $LASTEXITCODE)." }
Remove-Item $ddf, (Join-Path $stage 'setup.inf'), (Join-Path $stage 'setup.rpt') -ErrorAction SilentlyContinue

# 6. Hashes + provenance, so the package is traceable to its source commit.
$payload = Get-ChildItem $stage -File |
           Where-Object { $_.Extension -in '.sys', '.inf', '.cat', '.pdb', '.cab' }
$payload | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash, (Split-Path $_.Path -Leaf) } |
    Out-File (Join-Path $stage 'SHA256SUMS.txt') -Encoding ascii

$commit = (& cmd.exe /c 'git rev-parse HEAD 2>nul')
if ($LASTEXITCODE -ne 0) { $commit = '(not a git checkout)' }
@"
SynchronousAudioRouter driver package
=====================================
describe      : $Describe
commit        : $commit
platform      : $Platform
configuration : $Configuration
DriverVer     : $today,$DriverVersion
built (UTC)   : $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
sign mode     : Off (UNSIGNED - submit the .cab to attestation/OSSign to sign)
"@ | Out-File (Join-Path $stage 'BUILD-INFO.txt') -Encoding ascii

Write-Host "==> Package staged at: $stage"
Get-Content (Join-Path $stage 'SHA256SUMS.txt')
