<#
.SYNOPSIS
    Runs the SynchronousAudioRouter test harness on this machine.

.DESCRIPTION
    Installs the driver package, the software clock and the SarAsio test
    configuration, then exercises endpoint creation and audio loopback across
    a matrix of endpoint counts, plus a scenario where the ASIO host is killed
    while WASAPI clients are streaming. Writes one JSON and one log per
    scenario to -ResultsDir, a summary.json, and copies the SarAsio logs.

    Intended for a disposable VM with test signing enabled (see README.md).
    Must run elevated. Exit code 0 when every scenario passed, 1 otherwise,
    2 when a scenario hung (SarTest.exe did not exit in time - usually a
    kernel-side hang; look at the scenario log's last line).

.EXAMPLE
    .\Invoke-SarTest.ps1 -PackageDir C:\sar\driver -ToolsDir C:\sar\tools -CertPath C:\sar\test.cer
#>
[CmdletBinding()]
param(
    # Directory with SynchronousAudioRouter.sys/.inf/.cat (the CI driver package).
    [Parameter(Mandatory)] [string]$PackageDir,
    # Directory with SarTest.exe, SarTestClock.dll and SarAsio.dll (the CI usermode package).
    [Parameter(Mandatory)] [string]$ToolsDir,
    # Certificate (.cer) the package was test-signed with; imported into Root and TrustedPublisher.
    [string]$CertPath,
    [string]$ResultsDir = (Join-Path (Get-Location) 'sartest-results'),
    [int[]]$EndpointCounts = @(1, 4, 8, 16),
    [int]$Channels = 2,
    [int]$Iterations = 3,
    [double]$Duration = 5,
    # Per-scenario wall clock limit before it is declared hung.
    [int]$TimeoutSeconds = 600,
    [switch]$SkipInstall,
    [switch]$SkipKillTest
)

$ErrorActionPreference = 'Stop'

# Any terminating error ends up in the log with its location, and the
# script exits non-zero instead of letting the exception escape the caller's
# output redirection.
trap {
    Write-Host "FATAL: $_"
    Write-Host $_.ScriptStackTrace
    exit 1
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal $identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Invoke-SarTest.ps1 must run elevated.'
}

$sarTest = Join-Path $ToolsDir 'SarTest.exe'
$sarAsio = Join-Path $ToolsDir 'SarAsio.dll'
$clock = Join-Path $ToolsDir 'SarTestClock.dll'
$inf = Join-Path $PackageDir 'SynchronousAudioRouter.inf'
foreach ($required in $sarTest, $sarAsio, $clock) {
    if (-not (Test-Path $required)) { throw "Missing: $required" }
}
if (-not $SkipInstall -and -not (Test-Path $inf)) { throw "Missing: $inf" }

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
$ResultsDir = (Resolve-Path $ResultsDir).Path
$summary = [ordered]@{
    started = (Get-Date).ToString('o')
    machine = $env:COMPUTERNAME
    os = (Get-CimInstance Win32_OperatingSystem).Version
    scenarios = @()
}

# Runs one SarTest scenario, capturing stdout and the results JSON.
function Invoke-Scenario {
    param([string]$Name, [string[]]$Arguments, [int]$Timeout = $TimeoutSeconds)

    $log = Join-Path $ResultsDir "$Name.log"
    $json = Join-Path $ResultsDir "$Name.json"
    $argList = @($Arguments) + @('--results', "`"$json`"")
    Write-Host "==> $Name : SarTest $($argList -join ' ')"

    $proc = Start-Process -FilePath $sarTest -ArgumentList $argList -NoNewWindow -PassThru `
        -RedirectStandardOutput $log
    # Without touching Handle first, ExitCode reads as null after WaitForExit.
    $null = $proc.Handle
    $exited = $proc.WaitForExit($Timeout * 1000)
    $record = [ordered]@{ name = $Name; arguments = ($argList -join ' '); log = $log; results = $json }

    if (-not $exited) {
        Write-Warning "$Name did not exit within $Timeout s - treating as hung"
        try { $proc.Kill() } catch { }
        $record.outcome = 'hung'
        $record.exitCode = $null
        if (Test-Path $log) { $record.lastLine = (Get-Content $log -Tail 1) }
    } else {
        $record.exitCode = $proc.ExitCode
        $record.outcome = if ($proc.ExitCode -eq 0) { 'passed' } else { 'failed' }
    }

    if (Test-Path $json) {
        try { $record.summary = (Get-Content $json -Raw | ConvertFrom-Json) } catch { }
    }

    Write-Host "    -> $($record.outcome)"
    return $record
}

# Windows Server images ship with the audio stack disabled.
foreach ($service in 'AudioEndpointBuilder', 'Audiosrv') {
    try {
        Set-Service -Name $service -StartupType Automatic
        Start-Service -Name $service
    } catch {
        Write-Warning "Couldn't start $service : $_"
    }
}

if (-not $SkipInstall) {
    if ($CertPath) {
        Write-Host "==> Trusting $CertPath"
        foreach ($store in 'Root', 'TrustedPublisher') {
            $out = & certutil.exe -addstore -f $store $CertPath 2>&1
            if ($LASTEXITCODE -ne 0) { throw "certutil -addstore $store failed: $($out -join ' ')" }
        }
    }

    $install = Invoke-Scenario 'install' @('install', '--inf', "`"$inf`"", '--clock', "`"$clock`"",
        '--endpoints', $EndpointCounts[0], '--channels', $Channels)
    $summary.scenarios += $install

    if ($install.outcome -ne 'passed') {
        $summary.finished = (Get-Date).ToString('o')
        $summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $ResultsDir 'summary.json')
        throw 'Installation failed; see install.log'
    }
} else {
    # Still make sure the clock is registered and the configuration is ours.
    $summary.scenarios += Invoke-Scenario 'install' @('install', '--clock', "`"$clock`"",
        '--endpoints', $EndpointCounts[0], '--channels', $Channels)
}

$common = @('--sarasio', "`"$sarAsio`"", '--channels', $Channels)

foreach ($count in $EndpointCounts) {
    $summary.scenarios += Invoke-Scenario "run-${count}x${Channels}" (
        @('run', '--endpoints', $count, '--iterations', $Iterations, '--duration', $Duration) + $common)
}

if (-not $SkipKillTest) {
    # Kill the ASIO host while WASAPI clients are streaming, then check that a
    # fresh host can still create endpoints afterwards.
    $count = $EndpointCounts[-1]
    $hostLog = Join-Path $ResultsDir 'kill-host.log'
    $hostJson = Join-Path $ResultsDir 'kill-host.json'
    Write-Host "==> kill-host : starting a host with $count endpoint pairs"
    $hostProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $hostLog `
        -ArgumentList (@('host', '--endpoints', $count, '--duration', 120, '--results', "`"$hostJson`"") + $common)
    $null = $hostProc.Handle
    Start-Sleep -Seconds 5

    $wasapiLog = Join-Path $ResultsDir 'kill-host-wasapi.log'
    $wasapiJson = Join-Path $ResultsDir 'kill-host-wasapi.json'
    $wasapiProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $wasapiLog `
        -ArgumentList @('wasapi', '--endpoints', $count, '--channels', $Channels, '--duration', 15,
            '--expect-invalidation', '--results', "`"$wasapiJson`"")
    $null = $wasapiProc.Handle
    Start-Sleep -Seconds 5

    Write-Host "==> kill-host : killing the host mid-stream"
    try { Stop-Process -Id $hostProc.Id -Force } catch { }
    $hostExited = $hostProc.WaitForExit(60000)
    $wasapiExited = $wasapiProc.WaitForExit(60000)
    if (-not $wasapiExited) { try { $wasapiProc.Kill() } catch { } }

    $summary.scenarios += [ordered]@{
        name = 'kill-host'
        outcome = if ($hostExited -and $wasapiExited) { 'passed' } else { 'hung' }
        hostExited = $hostExited
        wasapiExited = $wasapiExited
        wasapiExitCode = if ($wasapiExited) { $wasapiProc.ExitCode } else { $null }
        log = $hostLog
        wasapiLog = $wasapiLog
    }

    $summary.scenarios += Invoke-Scenario 'recovery-after-kill' (
        @('run', '--endpoints', $count, '--iterations', 1, '--duration', $Duration) + $common)
}

$sarLogs = Join-Path $env:APPDATA 'SynchronousAudioRouter\logs'
if (Test-Path $sarLogs) {
    $dest = Join-Path $ResultsDir 'sarasio-logs'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item (Join-Path $sarLogs '*') $dest -Force -ErrorAction SilentlyContinue
}

$summary.finished = (Get-Date).ToString('o')
$outcomes = $summary.scenarios | ForEach-Object { $_.outcome }
$summary.passed = -not ($outcomes | Where-Object { $_ -ne 'passed' })
$summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $ResultsDir 'summary.json')

Write-Host ''
Write-Host 'Scenario summary:'
$summary.scenarios | ForEach-Object { Write-Host ("  {0,-24} {1}" -f $_.name, $_.outcome) }

if ($outcomes -contains 'hung') { exit 2 }
if ($summary.passed) { exit 0 } else { exit 1 }
