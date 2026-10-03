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
    [switch]$SkipKillTest,
    # Any of issues, matrix, race, kill, browser. Installation always runs.
    [string[]]$Scenarios = @('issues', 'matrix', 'race', 'kill', 'browser'),
    [int]$RaceCycles = 10,
    # Extra idle time between the race and the kill scenario, on top of
    # waiting for the endpoint builder to go idle (diagnostics).
    [int]$PostRaceDelaySeconds = 0
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

# Registers or unregisters SarAsio's COM classes. The driver's application
# routing only redirects to SarAsio when they are registered.
function Set-SarAsioRegistered {
    param([bool]$Registered)

    $regArgs = @('/s') + $(if ($Registered) { @() } else { @('/u') }) + @("`"$sarAsio`"")
    $proc = Start-Process regsvr32.exe -ArgumentList $regArgs -Wait -PassThru
    if ($proc.ExitCode -ne 0) { Write-Warning "regsvr32 $($regArgs -join ' ') exited with $($proc.ExitCode)" }
}

# Plays tools\test\tone.html in Edge with application routing on and Edge's
# audio sandbox on or off, and checks that Edge's audio session is heard.
# Also records what the sandboxed audio service is allowed to load.
function Invoke-BrowserScenario {
    param([string]$Name, [bool]$Sandbox)

    $record = [ordered]@{ name = $Name; sandbox = $Sandbox }
    $edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
              "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $edge) {
        Write-Host "==> $Name : skipped, Edge is not installed"
        $record.outcome = 'skipped'
        return $record
    }

    $policy = 'HKLM:\SOFTWARE\Policies\Microsoft\Edge'
    New-Item -Path $policy -Force | Out-Null
    Set-ItemProperty $policy HideFirstRunExperience 1 -Type DWord
    Set-ItemProperty $policy AutoplayAllowed 1 -Type DWord
    Set-ItemProperty $policy AudioSandboxEnabled ([int]$Sandbox) -Type DWord
    Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force

    # An Edge started while its installer replaces it exits without playing
    # (seen in long runs before Edge updates were turned off at the start).
    # Wait for an Edge installer still running.
    $updateWait = Get-Date
    $installers = @()
    while (((Get-Date) - $updateWait).TotalMinutes -lt 2) {
        $installers = @(Get-Process setup -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -like '*\Microsoft\Edge\Application\*' })
        if (-not $installers) { break }
        Start-Sleep -Seconds 2
    }
    $record.edgeInstallerWaitSeconds = [math]::Round(((Get-Date) - $updateWait).TotalSeconds, 1)
    if ($installers) { $record.edgeInstallers = @($installers | ForEach-Object { $_.Path }) }
    $started = Get-Date

    Set-SarAsioRegistered $true

    $hostLog = Join-Path $ResultsDir "$Name-host.log"
    $hostJson = Join-Path $ResultsDir "$Name-host.json"
    Write-Host "==> $Name : host with application routing, Edge audio sandbox $(if ($Sandbox) { 'on' } else { 'off' })"
    $hostProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $hostLog `
        -ArgumentList (@('host', '--endpoints', 1, '--duration', 120, '--app-routing',
            '--results', "`"$hostJson`"") + $common)
    $null = $hostProc.Handle
    Start-Sleep -Seconds 8

    # Edge as the logged-on user without elevation, like a user starts it.
    $page = 'file:///' + ((Join-Path $PSScriptRoot 'tone.html') -replace '\\', '/')
    $userData = Join-Path $env:TEMP "sartest-edge-$Name"
    $edgeArgs = "--user-data-dir=`"$userData`" --no-first-run --no-default-browser-check " +
        "--autoplay-policy=no-user-gesture-required --enable-logging --v=0 `"$page`""
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    Register-ScheduledTask -TaskName 'SarTestEdge' -Force `
        -Action (New-ScheduledTaskAction -Execute $edge -Argument $edgeArgs) `
        -Principal (New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Limited) `
        -Settings (New-ScheduledTaskSettingsSet -MultipleInstances Parallel) | Out-Null

    # Wait for Edge's audio service to start (the page plays right away).
    # Edge once didn't come up at all, so a launch that produced no audio
    # service is retried once and then reported as such, not as silence.
    $record.edgeStarted = $false
    foreach ($attempt in 1..2) {
        Start-ScheduledTask -TaskName 'SarTestEdge'
        $deadline = (Get-Date).AddSeconds(20)
        while (-not $record.edgeStarted -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            $record.edgeStarted = [bool](Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" |
                Where-Object { $_.CommandLine -match 'audio\.mojom\.AudioService' })
        }
        if ($record.edgeStarted) { break }
        $taskResult = (Get-ScheduledTaskInfo -TaskName 'SarTestEdge').LastTaskResult
        Write-Warning "$Name : Edge's audio service didn't start (attempt $attempt, task result $taskResult)"
        Stop-ScheduledTask -TaskName 'SarTestEdge' -ErrorAction SilentlyContinue
        Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 2
    }

    if (-not $record.edgeStarted) {
        $record.outcome = 'error'
        $record.error = "Edge's audio service never started"
        # What Edge was doing instead, for working out why.
        $record.edgeProcesses = @(Get-CimInstance Win32_Process |
            Where-Object { $_.Name -match '^(msedge|MicrosoftEdgeUpdate|setup|elevation_service)' } |
            ForEach-Object { "$($_.ProcessId) $($_.Name) $($_.CommandLine)" })
        $record.edgeVersion = (Get-Item $edge).VersionInfo.ProductVersion
        $debugLog = Join-Path $userData 'chrome_debug.log'
        if (Test-Path $debugLog) { Copy-Item $debugLog (Join-Path $ResultsDir "$Name-chrome_debug.log") }
        $record.edgeProcesses | ForEach-Object { Write-Host "    $_" }
        Unregister-ScheduledTask -TaskName 'SarTestEdge' -Confirm:$false -ErrorAction SilentlyContinue
        try { Stop-Process -Id $hostProc.Id -Force } catch { }
        $null = $hostProc.WaitForExit(60000)
        Set-SarAsioRegistered $false
        return $record
    }

    $meter = Invoke-Scenario $Name @('meter', '--duration', 20, '--process', 'msedge.exe') -Timeout 120
    foreach ($key in $meter.Keys) { if (-not $record.Contains($key)) { $record[$key] = $meter[$key] } }

    # Facts about Edge's audio service: its sandbox, its mitigation policies,
    # whether SarAsio.dll got loaded, and any Code Integrity block events.
    $record.audioProcesses = @(Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" |
        Where-Object { $_.CommandLine -match 'audio\.mojom\.AudioService' } | ForEach-Object {
            $info = [ordered]@{
                pid = $_.ProcessId
                sandboxType = if ($_.CommandLine -match '--service-sandbox-type=(\S+)') { $Matches[1] } else { $null }
            }
            try {
                $m = Get-ProcessMitigation -Id $_.ProcessId
                $info.microsoftSignedOnly = "$($m.BinarySignature.MicrosoftSignedOnly)"
                $info.blockLowLabelImageLoads = "$($m.ImageLoad.BlockLowLabelImageLoads)"
                $info.blockRemoteImageLoads = "$($m.ImageLoad.BlockRemoteImageLoads)"
                $info.blockDynamicCode = "$($m.DynamicCode.BlockDynamicCode)"
            } catch { $info.mitigationError = "$_" }
            try {
                $info.sarAsioLoaded = [bool]((Get-Process -Id $_.ProcessId).Modules |
                    Where-Object ModuleName -eq 'SarAsio.dll')
            } catch { $info.modulesError = "$_" }
            [pscustomobject]$info
        })
    $record.sarAsioLoadedIn = @(Get-Process msedge -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Modules | Where-Object ModuleName -eq 'SarAsio.dll' } catch { $false }
    } | ForEach-Object { $_.Id })
    $record.codeIntegrityEvents = @(Get-WinEvent -ErrorAction SilentlyContinue -FilterHashtable @{
            LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; StartTime = $started } |
        Where-Object { $_.Message -match 'SarAsio' } | Select-Object -First 5 |
        ForEach-Object { "$($_.Id): $($_.Message)" })
    $record.audioProcesses | ForEach-Object { Write-Host "    audio service: $($_ | ConvertTo-Json -Compress)" }
    Write-Host "    SarAsio.dll loaded in msedge pids: $($record.sarAsioLoadedIn -join ', ')"
    $record.codeIntegrityEvents | ForEach-Object { Write-Host "    CI: $_" }

    Stop-ScheduledTask -TaskName 'SarTestEdge' -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName 'SarTestEdge' -Confirm:$false -ErrorAction SilentlyContinue
    Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force
    try { Stop-Process -Id $hostProc.Id -Force } catch { }
    $null = $hostProc.WaitForExit(60000)
    Set-SarAsioRegistered $false
    Start-Sleep -Seconds 5
    return $record
}

# Edge updates itself some minutes after boot, and an Edge started while it
# does exits without playing. Turn its updates off before they start.
if ($Scenarios -contains 'browser') {
    $updatePolicy = 'HKLM:\SOFTWARE\Policies\Microsoft\EdgeUpdate'
    New-Item -Path $updatePolicy -Force | Out-Null
    Set-ItemProperty $updatePolicy UpdateDefault 0 -Type DWord
}

# Waits until Windows' audio endpoint builder has worked off the endpoint
# changes earlier scenarios caused, and returns the seconds waited. A burst
# of host restarts (the race) leaves it busy for a while, tearing down and
# rebuilding SAR endpoints one at a time; streams opened meanwhile stall in
# Activate/Initialize or fail. Idle: under 100 ms of its CPU per second for
# three seconds in a row.
function Wait-EndpointBuilderIdle {
    param([int]$TimeoutSeconds = 120)

    $svc = Get-CimInstance Win32_Service -Filter "Name='AudioEndpointBuilder'"
    $proc = if ($svc) { Get-Process -Id $svc.ProcessId -ErrorAction SilentlyContinue }
    $started = Get-Date

    if (-not $proc) { return 0 }

    $last = $proc.TotalProcessorTime.TotalMilliseconds
    $quiet = 0

    while (((Get-Date) - $started).TotalSeconds -lt $TimeoutSeconds -and $quiet -lt 3) {
        Start-Sleep -Seconds 1
        $proc.Refresh()
        $now = $proc.TotalProcessorTime.TotalMilliseconds
        if ($now - $last -lt 100) { $quiet++ } else { $quiet = 0 }
        $last = $now
    }

    return [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
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

if ($Scenarios -contains 'issues') {
    # Regression checks, one short host each, most for a GitHub issue.
    $summary.scenarios += Invoke-Scenario 'issue-133-control-panel' (
        @('control-panel', '--endpoints', 1) + $common) -Timeout 120
    $summary.scenarios += Invoke-Scenario 'issue-54-no-interface' (
        @('no-interface') + $common) -Timeout 120
    $summary.scenarios += Invoke-Scenario 'issue-15-endpoint-names' (
        @('endpoint-names') + $common) -Timeout 300
    # An endpoint name longer than the driver's 63 characters used to abort
    # the host process in SarAsio.
    $summary.scenarios += Invoke-Scenario 'long-endpoint-name' (
        @('host', '--endpoints', 1, '--duration', 2, '--id-prefix', 'SarTest-long',
          '--prefix', ('SarTestLongEndpointName' * 4)) + $common) -Timeout 120
    # Application routing on: from then on the driver's registry filter sees
    # every registry value query on the machine. With SarAsio's COM classes
    # registered, the harness's own WASAPI client also goes through SarAsio's
    # device enumerator wrapper.
    Set-SarAsioRegistered $true
    $summary.scenarios += Invoke-Scenario 'app-routing' (
        @('run', '--endpoints', 1, '--iterations', 1, '--duration', $Duration, '--app-routing') + $common)
    Set-SarAsioRegistered $false
}

if ($Scenarios -contains 'matrix') {
    foreach ($count in $EndpointCounts) {
        $summary.scenarios += Invoke-Scenario "run-${count}x${Channels}" (
            @('run', '--endpoints', $count, '--iterations', $Iterations, '--duration', $Duration) + $common)
    }
}

# Timeline of SAR interface and endpoint state changes across the race, kill
# and recovery scenarios, for lining up with what their clients saw.
$watchProc = $null
if ($Scenarios -contains 'race' -or $Scenarios -contains 'kill') {
    $watchStop = Join-Path $ResultsDir 'watch.stop'
    $watchJson = Join-Path $ResultsDir 'watch.json'
    Remove-Item $watchStop -ErrorAction SilentlyContinue
    $watchProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $ResultsDir 'watch.log') `
        -ArgumentList @('watch', '--duration', 3600, '--stop-file', "`"$watchStop`"",
            '--results', "`"$watchJson`"")
    $null = $watchProc.Handle
}

if ($Scenarios -contains 'race') {
    # Start and stop the host over and over while other threads keep opening
    # streams on its endpoints: the start-up race reported against
    # many-endpoint setups.
    $count = $EndpointCounts[-1]
    $summary.scenarios += Invoke-Scenario "race-${count}x${Channels}" (
        @('race', '--endpoints', $count, '--cycles', $RaceCycles, '--up', 3, '--down', 200,
          '--openers', 4) + $common)
}

if ($Scenarios -contains 'race') {
    # Let the endpoint builder catch up with the race before the kill
    # scenario, so recovery-after-kill measures recovery from a killed host
    # rather than the race's aftermath. How long that takes is the race's
    # cost to Windows; a builder still busy after two minutes fails.
    $settleTimeout = 120
    $waited = Wait-EndpointBuilderIdle -TimeoutSeconds $settleTimeout
    Write-Host "==> settle-after-race : endpoint builder idle after $waited s"
    $summary.scenarios += [ordered]@{
        name = 'settle-after-race'
        outcome = if ($waited -lt $settleTimeout) { 'passed' } else { 'failed' }
        seconds = $waited
    }

    if ($PostRaceDelaySeconds -gt 0) {
        Write-Host "==> idling another $PostRaceDelaySeconds s"
        Start-Sleep -Seconds $PostRaceDelaySeconds
    }
}

if (-not $SkipKillTest -and $Scenarios -contains 'kill') {
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

if ($watchProc) {
    New-Item -ItemType File -Path $watchStop -Force | Out-Null
    if (-not $watchProc.WaitForExit(30000)) { try { $watchProc.Kill() } catch { } }
}

if ($Scenarios -contains 'browser') {
    # With application routing on, the driver points the audio device
    # enumerator's COM registration at SarAsio.dll for every process of the
    # user, Chromium's sandboxed audio service included, which then played
    # nothing (#80, #102, #121, #127). Runs last, since Edge is slow and the
    # rest of the run doesn't need it.
    $summary.scenarios += Invoke-BrowserScenario 'browser-audio-unsandboxed' $false
    $summary.scenarios += Invoke-BrowserScenario 'browser-audio-sandboxed' $true
}

$sarLogs = Join-Path $env:APPDATA 'SynchronousAudioRouter\logs'
if (Test-Path $sarLogs) {
    $dest = Join-Path $ResultsDir 'sarasio-logs'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item (Join-Path $sarLogs '*') $dest -Force -ErrorAction SilentlyContinue
}

$summary.finished = (Get-Date).ToString('o')
$outcomes = $summary.scenarios | ForEach-Object { $_.outcome }
$summary.passed = -not ($outcomes | Where-Object { $_ -ne 'passed' -and $_ -ne 'skipped' })
$summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $ResultsDir 'summary.json')

Write-Host ''
Write-Host 'Scenario summary:'
$summary.scenarios | ForEach-Object { Write-Host ("  {0,-24} {1}" -f $_.name, $_.outcome) }

if ($outcomes -contains 'hung') { exit 2 }
if ($summary.passed) { exit 0 } else { exit 1 }
