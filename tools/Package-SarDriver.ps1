<#
.SYNOPSIS
    Stage, version-stamp, catalog and hash the built SynchronousAudioRouter driver
    into a submission-ready, verifiable package.

.DESCRIPTION
    Run AFTER msbuild has produced SynchronousAudioRouter.sys. This script:

      1. locates the freshly built .sys,
      2. stamps a valid DriverVer into a staged copy of the INF
         (the committed INF still carries the placeholder "DriverVer=0.1",
         which is illegal for a real package),
      3. generates the security catalog (.cat) with Inf2Cat,
      4. packs .sys/.inf/.cat into a .cab ready to submit for
         Microsoft attestation signing (Partner Center) or to OSSign,
      5. writes SHA256SUMS.txt and BUILD-INFO.txt so the binary is
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

    # Four-field DriverVer version (x.y.z.w), e.g. 0.13.2.137
    [Parameter(Mandatory)] [string]$DriverVersion,

    # Human-readable build label (e.g. `git describe` output), recorded in BUILD-INFO.
    [string]$Describe = 'dev',

    [string]$OutDir = 'artifacts',
    [string]$InfSource = 'SynchronousAudioRouter/SynchronousAudioRouter.inf'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo  = (Resolve-Path '.').Path
$stage = Join-Path $repo (Join-Path $OutDir $Platform)
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# 1. Locate the freshly built .sys (prefer the linker output under <Platform>\<Config>).
$candidates = Get-ChildItem -Path $repo -Recurse -Filter 'SynchronousAudioRouter.sys' -ErrorAction SilentlyContinue
$sys = $candidates | Where-Object { $_.FullName -match "\\$Platform\\$Configuration\\" } |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $sys) { $sys = $candidates | Sort-Object LastWriteTime -Descending | Select-Object -First 1 }
if (-not $sys) { throw "SynchronousAudioRouter.sys not found - did the driver build succeed?" }
Write-Host "==> Driver binary: $($sys.FullName)"
Copy-Item $sys.FullName (Join-Path $stage 'SynchronousAudioRouter.sys') -Force

# 2. Stamp a valid DriverVer into a staged copy of the INF.
$today = (Get-Date).ToString('MM/dd/yyyy')
$inf = Get-Content -Raw -Path (Join-Path $repo $InfSource)
$inf = [regex]::Replace($inf, '(?m)^\s*DriverVer\s*=.*$', "DriverVer=$today,$DriverVersion")
$infOut = Join-Path $stage 'SynchronousAudioRouter.inf'
Set-Content -Path $infOut -Value $inf -Encoding ASCII
Write-Host "==> Stamped DriverVer=$today,$DriverVersion"

# 3. Generate the security catalog (.cat) with Inf2Cat.
$inf2cat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" `
               -Recurse -Filter 'Inf2Cat.exe' -ErrorAction SilentlyContinue |
           Sort-Object FullName -Descending | Select-Object -First 1
if (-not $inf2cat) { throw "Inf2Cat.exe not found - is the WDK installed?" }
$osMap  = @{ 'x64' = '10_X64'; 'ARM64' = '10_ARM64' }
$osList = $osMap[$Platform]
Write-Host "==> Inf2Cat (/os:$osList) ..."
& $inf2cat.FullName "/driver:$stage" "/os:$osList" /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed (exit $LASTEXITCODE)." }

# 4. Build a .cab for attestation / OSSign submission.
$cabName = "SynchronousAudioRouter_$Platform.cab"
$ddf = Join-Path $stage '_package.ddf'
@"
.OPTION EXPLICIT
.Set CabinetNameTemplate=$cabName
.Set DiskDirectory1=$stage
.Set CompressionType=MSZIP
.Set Cabinet=on
.Set Compress=on
"$stage\SynchronousAudioRouter.sys"
"$stage\SynchronousAudioRouter.inf"
"$stage\SynchronousAudioRouter.cat"
"@ | Set-Content -Path $ddf -Encoding ASCII
Write-Host "==> makecab -> $cabName"
& makecab /f $ddf | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makecab failed (exit $LASTEXITCODE)." }
Remove-Item $ddf, (Join-Path $stage 'setup.inf'), (Join-Path $stage 'setup.rpt') -ErrorAction SilentlyContinue

# 5. Hashes + provenance, so the package is traceable to its source commit.
$payload = Get-ChildItem $stage -File |
           Where-Object { $_.Extension -in '.sys', '.inf', '.cat', '.cab' }
$payload | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash, (Split-Path $_.Path -Leaf) } |
    Out-File (Join-Path $stage 'SHA256SUMS.txt') -Encoding ascii

$commit = (& git rev-parse HEAD 2>$null)
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
