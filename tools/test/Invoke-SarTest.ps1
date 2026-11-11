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
    # Any of issues, conformance, matrix, race, kill, formats, fuzz, unload,
    # device, browser. Installation always runs.
    [string[]]$Scenarios = @('issues', 'conformance', 'matrix', 'race', 'kill', 'formats', 'fuzz', 'unload', 'device', 'browser'),
    [int]$RaceCycles = 10,
    # Extra idle time between the race and the kill scenario, on top of
    # waiting for the endpoint builder to go idle (diagnostics).
    [int]$PostRaceDelaySeconds = 0,
    # For runs under fault injection (Driver Verifier's low resources
    # simulation fails the driver's allocations at random): a scenario that
    # fails is recorded as "tolerated" and does not fail the run. A hang still
    # does, and a bugcheck ends the run whatever this says.
    [switch]$SurviveOnly,
    # Wait after installation until the machine has been up this long before
    # the first scenario. Fault injection starts a set number of minutes
    # after boot, so that the driver can be installed without it.
    [int]$StartAfterUptimeMinutes = 0,
    # The driver under test is the C++ driver of releases up to 0.13: where
    # the current driver deliberately differs (section 13 of the driver
    # specification), expect the old behaviour.
    [switch]$LegacyDriver,
    # How long the fuzz scenario sends malformed requests, and its seed (a
    # new one each run when 0; the scenario's log and results name it).
    [int]$FuzzSeconds = 60,
    [long]$FuzzSeed = 0,
    # A directory with another build of SarTest.exe, SarAsio.dll and
    # SarTestClock.dll to run every scenario with, once the ones in -ToolsDir
    # have installed the driver: the 32-bit builds, to put a 32-bit host and
    # 32-bit clients on the 64-bit driver. (Only a 64-bit program can install
    # a driver on 64-bit Windows.)
    [string]$ScenarioToolsDir
)

$ErrorActionPreference = 'Stop'

# Any terminating error ends up in the log with its location, and the
# script exits non-zero instead of letting the exception escape the caller's
# output redirection.
# Windows' own sounds (a device connecting, a notification) play on the
# default playback endpoint, which is a SAR endpoint whenever one exists. The
# loopback check then hears them as frames that are not the test signal:
# always the same run lengths, and only bits 8 and up of each sample
# disturbed, since the sounds are 16-bit. The user's sound scheme is emptied
# for the run and put back afterwards.
$script:soundEvents = @()
function Disable-SystemSounds {
    $apps = Get-Item 'HKCU:\AppEvents\Schemes\Apps' -ErrorAction SilentlyContinue
    if (-not $apps) { return }
    foreach ($appName in $apps.GetSubKeyNames()) {
        $app = $apps.OpenSubKey($appName)
        foreach ($eventName in $app.GetSubKeyNames()) {
            $current = $app.OpenSubKey("$eventName\.Current", $true)
            if (-not $current) { continue }
            $value = $current.GetValue('', $null, 'DoNotExpandEnvironmentNames')
            if ($value) {
                $script:soundEvents += [pscustomobject]@{
                    key = "$appName\$eventName\.Current"; value = $value; kind = $current.GetValueKind('')
                }
                $current.SetValue('', '', 'String')
            }
            $current.Close()
        }
        $app.Close()
    }
    Write-Host "==> Silenced $($script:soundEvents.Count) Windows sound events for the run"
}

function Restore-SystemSounds {
    $apps = Get-Item 'HKCU:\AppEvents\Schemes\Apps' -ErrorAction SilentlyContinue
    foreach ($event in $script:soundEvents) {
        $current = if ($apps) { $apps.OpenSubKey($event.key, $true) }
        if ($current) {
            $current.SetValue('', $event.value, $event.kind)
            $current.Close()
        }
    }
    $script:soundEvents = @()
}

trap {
    Write-Host "FATAL: $_"
    Write-Host $_.ScriptStackTrace
    Restore-SystemSounds
    exit 1
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal $identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Invoke-SarTest.ps1 must run elevated.'
}

# A misspelled group would otherwise be skipped without a word, and the run
# would pass having tested nothing. Accept "a,b" as one argument too, which
# is how the list arrives through powershell -File.
$knownScenarios = 'issues', 'conformance', 'matrix', 'race', 'kill', 'formats', 'fuzz', 'unload', 'device', 'browser'
$Scenarios = @($Scenarios | ForEach-Object { $_ -split ',' } |
    ForEach-Object { $_.Trim() } | Where-Object { $_ })
$unknownScenarios = @($Scenarios | Where-Object { $knownScenarios -notcontains $_ })
if ($unknownScenarios) {
    throw "Unknown scenario group(s): $($unknownScenarios -join ', '). Known: $($knownScenarios -join ', ')."
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
# A run that dies before the end must not leave an earlier run's verdict.
Remove-Item (Join-Path $ResultsDir 'summary.json') -ErrorAction SilentlyContinue
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
    # So that a SarTest that dies without writing results isn't given the
    # results of an earlier run.
    Remove-Item $json -ErrorAction SilentlyContinue

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
    if ($proc.ExitCode -ne 0) {
        $message = "regsvr32 $($regArgs -join ' ') exited with $($proc.ExitCode)"
        # Without the classes registered, application routing redirects
        # nothing, and a scenario that relies on it would test nothing.
        if ($Registered) { throw $message }
        Write-Warning $message
    }
}

# Runs a host with application routing on and a WASAPI client in a process of
# its own. Passes when the loopback verifies and the client loaded
# SarAsio.dll. The client has no other reason to load it: it only does when
# the driver's registry filter redirected its device enumerator. A loopback
# alone also passes with routing off.
function Invoke-AppRoutingScenario {
    $name = 'app-routing'
    $record = [ordered]@{ name = $name }

    try { Set-SarAsioRegistered $true } catch {
        $record.outcome = 'error'
        $record.error = "$_"
        Write-Host "==> $name : $($record.error)"
        return $record
    }

    $hostLog = Join-Path $ResultsDir "$name-host.log"
    $hostJson = Join-Path $ResultsDir "$name-host.json"
    Write-Host "==> $name : host with application routing, client in its own process"
    $hostProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $hostLog `
        -ArgumentList (@('host', '--endpoints', 1, '--duration', 120, '--app-routing',
            '--results', "`"$hostJson`"") + $common)
    $null = $hostProc.Handle
    Start-Sleep -Seconds 5

    $log = Join-Path $ResultsDir "$name.log"
    $json = Join-Path $ResultsDir "$name.json"
    Remove-Item $json -ErrorAction SilentlyContinue
    $client = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $log `
        -ArgumentList @('wasapi', '--endpoints', 1, '--channels', $Channels, '--duration', $Duration,
            '--results', "`"$json`"")
    $null = $client.Handle

    # Look while the client runs; its module list is gone once it exits.
    $record.sarAsioLoaded = $false
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not $client.HasExited -and (Get-Date) -lt $deadline) {
        if (-not $record.sarAsioLoaded) {
            try {
                $record.sarAsioLoaded = [bool]((Get-Process -Id $client.Id).Modules |
                    Where-Object ModuleName -eq 'SarAsio.dll')
            } catch { }
        }
        Start-Sleep -Milliseconds 500
    }

    $exited = $client.HasExited
    if (-not $exited) { try { $client.Kill() } catch { } }
    $record.hostRunning = -not $hostProc.HasExited
    try { Stop-Process -Id $hostProc.Id -Force } catch { }
    $null = $hostProc.WaitForExit(60000)
    Set-SarAsioRegistered $false

    $record.exitCode = if ($exited) { $client.ExitCode } else { $null }
    $record.log = $log
    $record.results = $json
    $record.hostLog = $hostLog
    if (Test-Path $json) {
        try { $record.summary = (Get-Content $json -Raw | ConvertFrom-Json) } catch { }
    }

    if (-not $exited) {
        $record.outcome = 'hung'
    } elseif ($client.ExitCode -ne 0) {
        $record.outcome = 'failed'
    } elseif (-not $record.hostRunning) {
        $record.outcome = 'failed'
        $record.error = 'the host exited before the client finished'
    } elseif (-not $record.sarAsioLoaded) {
        $record.outcome = 'failed'
        $record.error = 'the client never loaded SarAsio.dll, so application routing was not in effect'
    } else {
        $record.outcome = 'passed'
    }

    Write-Host "    -> $($record.outcome)$(if ($record.error) { ': ' + $record.error })"
    return $record
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

    try { Set-SarAsioRegistered $true } catch {
        $record.outcome = 'error'
        $record.error = "$_"
        return $record
    }

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

    # The meter only says that Edge was heard, and Edge is heard just as
    # well when application routing never came on. So also require signs
    # that it was on: the host still running, and, where Edge's audio
    # service is allowed to load it, SarAsio.dll inside Edge.
    $record.hostRunning = -not $hostProc.HasExited
    if ($record.outcome -eq 'passed') {
        if (-not $record.hostRunning) {
            $record.outcome = 'failed'
            $record.error = 'the host with application routing exited before the check finished'
        } elseif (-not $Sandbox -and -not $record.sarAsioLoadedIn) {
            $record.outcome = 'failed'
            $record.error = 'SarAsio.dll was not loaded in Edge, so application routing was not in effect'
        }
    }

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

Disable-SystemSounds

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

if ($ScenarioToolsDir) {
    $sarTest = Join-Path $ScenarioToolsDir 'SarTest.exe'
    $sarAsio = Join-Path $ScenarioToolsDir 'SarAsio.dll'
    $clock = Join-Path $ScenarioToolsDir 'SarTestClock.dll'
    foreach ($required in $sarTest, $sarAsio, $clock) {
        if (-not (Test-Path $required)) { throw "Missing: $required" }
    }
    $summary.scenarioTools = $ScenarioToolsDir
    # Register this build's clock (a 32-bit build registers in the 32-bit
    # view of the registry) and write the configuration again.
    $summary.scenarios += Invoke-Scenario 'install-scenario-tools' @('install', '--clock', "`"$clock`"",
        '--endpoints', $EndpointCounts[0], '--channels', $Channels)
}

if ($StartAfterUptimeMinutes -gt 0) {
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    $wait = [math]::Ceiling(($boot.AddMinutes($StartAfterUptimeMinutes) - (Get-Date)).TotalSeconds)
    if ($wait -gt 0) {
        Write-Host "==> Waiting $wait s for the machine to have been up $StartAfterUptimeMinutes minutes"
        Start-Sleep -Seconds $wait
    }
    $summary.startedScenariosAtUptimeMinutes = [math]::Round(((Get-Date) - $boot).TotalMinutes, 1)
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
    # Issue #2: after the host's sample rate or channel count changes, the
    # audio engine kept the endpoint's old format. The driver encodes the
    # format in each endpoint's interface name, so the same endpoint IDs in a
    # new format are new endpoints to Windows. Same IDs at 48 kHz, 96 kHz,
    # 48 kHz again, then mono; each must stream with the engine format
    # matching. (Mono, because Windows gives SAR endpoints with more than two
    # channels a stereo shared-mode format, so four channels can't be told
    # apart from a stale two.)
    $fmt = @('run', '--endpoints', 2, '--iterations', 1, '--duration', 3, '--prefix', 'SarTestFormat',
        '--expect-format', '--sarasio', "`"$sarAsio`"")
    $summary.scenarios += Invoke-Scenario 'format-48k' ($fmt + @('--rate', 48000, '--channels', 2))
    $summary.scenarios += Invoke-Scenario 'format-96k' ($fmt + @('--rate', 96000, '--channels', 2))
    $summary.scenarios += Invoke-Scenario 'format-48k-again' ($fmt + @('--rate', 48000, '--channels', 2))
    $summary.scenarios += Invoke-Scenario 'format-mono' ($fmt + @('--rate', 48000, '--channels', 1))
    # Application routing on: from then on the driver's registry filter sees
    # every registry value query on the machine. With SarAsio's COM classes
    # registered, the harness's own WASAPI client also goes through SarAsio's
    # device enumerator wrapper.
    $summary.scenarios += Invoke-AppRoutingScenario
}

if ($Scenarios -contains 'conformance') {
    # Every request of the control interface, the refused ones included, sent
    # to the driver directly; then a stream on a section whose audio area is
    # not a multiple of 64 KiB, which SarAsio never asks for.
    $conformance = @('conformance', '--registry-filter')
    if ($LegacyDriver) { $conformance += '--legacy-quirks' }
    $summary.scenarios += Invoke-Scenario 'conformance' $conformance -Timeout 300
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
    Remove-Item $wasapiJson -ErrorAction SilentlyContinue
    $wasapiProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $wasapiLog `
        -ArgumentList @('wasapi', '--endpoints', $count, '--channels', $Channels, '--duration', 15,
            '--expect-invalidation', '--results', "`"$wasapiJson`"")
    $null = $wasapiProc.Handle
    Start-Sleep -Seconds 5

    # A host that already died on its own isn't killed mid-stream.
    $hostAliveAtKill = -not $hostProc.HasExited
    Write-Host "==> kill-host : killing the host mid-stream"
    try { Stop-Process -Id $hostProc.Id -Force } catch { }
    $hostExited = $hostProc.WaitForExit(60000)
    $wasapiExited = $wasapiProc.WaitForExit(60000)
    if (-not $wasapiExited) { try { $wasapiProc.Kill() } catch { } }

    # Both processes exiting only shows that nothing hung. The scenario is
    # about streams that were running when the host went away, so also
    # require that: every stream started, every recording stream got the
    # signal first, and every stream noticed the host going.
    $killStreams = @()
    if (Test-Path $wasapiJson) {
        try { $killStreams = @((Get-Content $wasapiJson -Raw | ConvertFrom-Json).wasapi.streams) } catch { }
    }
    $kill = [ordered]@{
        name = 'kill-host'
        hostAliveAtKill = $hostAliveAtKill
        hostExited = $hostExited
        wasapiExited = $wasapiExited
        wasapiExitCode = if ($wasapiExited) { $wasapiProc.ExitCode } else { $null }
        streams = $killStreams.Count
        streamsStarted = @($killStreams | Where-Object started).Count
        streamsInvalidated = @($killStreams | Where-Object deviceInvalidated).Count
        capturesWithSignal = @($killStreams |
            Where-Object { $_.direction -eq 'capture' -and $_.validFrames -gt 0 }).Count
        log = $hostLog
        wasapiLog = $wasapiLog
    }

    if (-not ($hostExited -and $wasapiExited)) {
        $kill.outcome = 'hung'
    } elseif (-not $hostAliveAtKill) {
        $kill.outcome = 'failed'
        $kill.error = 'the host had already exited when it was to be killed'
    } elseif ($wasapiProc.ExitCode -ne 0) {
        $kill.outcome = 'failed'
        $kill.error = "the WASAPI client exited with $($wasapiProc.ExitCode)"
    } elseif ($kill.streamsStarted -ne 2 * $count -or $kill.capturesWithSignal -ne $count) {
        $kill.outcome = 'failed'
        $kill.error = 'not every stream was running when the host was killed'
    } elseif ($kill.streamsInvalidated -ne 2 * $count) {
        $kill.outcome = 'failed'
        $kill.error = 'not every stream noticed the host going away'
    } else {
        $kill.outcome = 'passed'
    }

    Write-Host "    -> $($kill.outcome)$(if ($kill.error) { ': ' + $kill.error })"
    $summary.scenarios += $kill

    $summary.scenarios += Invoke-Scenario 'recovery-after-kill' (
        @('run', '--endpoints', $count, '--iterations', 1, '--duration', $Duration) + $common)
}

if ($watchProc) {
    New-Item -ItemType File -Path $watchStop -Force | Out-Null
    if (-not $watchProc.WaitForExit(30000)) { try { $watchProc.Kill() } catch { } }
}

if ($Scenarios -contains 'formats') {
    # The formats and sizes the other scenarios never use. Each one changes
    # the layout the host asks the driver for, and with it the driver's
    # buffer arithmetic: sample rates other than 48 and 96 kHz, ASIO periods
    # shorter than 480 frames, endpoints wider than stereo up to the limit of
    # 32 channels, and twice as many endpoints as the matrix goes to.
    $fmt = @('run', '--endpoints', 1, '--iterations', 1, '--duration', 3, '--prefix', 'SarTestFormats',
        '--sarasio', "`"$sarAsio`"")
    try {
        foreach ($rate in 44100, 88200, 176400, 192000) {
            # SarAsio refuses an ASIO buffer longer than the audio engine's
            # 10 ms period, so the clock's period follows the rate.
            $env:SAR_TEST_CLOCK_FRAMES = "$([int]($rate / 100))"
            $summary.scenarios += Invoke-Scenario "rate-$rate" (
                $fmt + @('--rate', $rate, '--channels', 2, '--expect-format'))
        }
        foreach ($frames in 64, 128, 256) {
            $env:SAR_TEST_CLOCK_FRAMES = "$frames"
            $summary.scenarios += Invoke-Scenario "period-$frames" ($fmt + @('--rate', 48000, '--channels', 2))
        }
    } finally {
        Remove-Item Env:SAR_TEST_CLOCK_FRAMES -ErrorAction SilentlyContinue
    }
    foreach ($wide in 4, 8, 32) {
        $summary.scenarios += Invoke-Scenario "channels-$wide" ($fmt + @('--rate', 48000, '--channels', $wide))
    }
    $summary.scenarios += Invoke-Scenario 'endpoints-32x2' (
        @('run', '--endpoints', 32, '--iterations', 1, '--duration', $Duration) + $common)
    $null = Wait-EndpointBuilderIdle -TimeoutSeconds 180
}

if ($Scenarios -contains 'fuzz') {
    # Malformed control and kernel streaming requests from several threads,
    # sent as a user without administrator rights, while endpoints come and
    # go. It passes if no request is left hanging; a bugcheck ends the run.
    # It runs before the unload scenario, so that memory its requests leaked
    # in the driver shows there.
    $fuzz = @('fuzz', '--seconds', $FuzzSeconds, '--restricted')
    if ($FuzzSeed -ne 0) { $fuzz += @('--seed', $FuzzSeed) }
    $summary.scenarios += Invoke-Scenario 'fuzz' $fuzz -Timeout ($FuzzSeconds + 300)
    # It leaves Windows' endpoint builder with a few hundred endpoints to
    # forget; the driver has to work once that has settled.
    $null = Wait-EndpointBuilderIdle -TimeoutSeconds 180
    $summary.scenarios += Invoke-Scenario 'run-after-fuzz' (
        @('run', '--endpoints', $EndpointCounts[0], '--iterations', 1, '--duration', $Duration) + $common)
}

# Stops the driver after the scenarios before it have used it: disables the
# SAR device, waits for the driver to unload, enables the device again and
# waits for the driver to come back. Driver Verifier checks a driver's pool
# allocations when it unloads and bugchecks on any that are left, so with
# Verifier on this is where memory leaked by the earlier scenarios shows.
# Without it, this still catches a driver that cannot be stopped or does not
# come back.
function Invoke-UnloadScenario {
    param([string]$Name)

    $record = [ordered]@{ name = $Name }
    $hardwareId = 'SW\{0BCFFA5C-E754-48CF-A783-A64C0DC0BB2C}'
    $driverState = {
        $driver = Get-CimInstance Win32_SystemDriver -Filter "Name='SynchronousAudioRouter'" `
            -ErrorAction SilentlyContinue
        if ($driver) { $driver.State } else { 'absent' }
    }
    $waitForState = {
        param([string]$State, [int]$Seconds)
        $start = Get-Date
        while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and (& $driverState) -ne $State) {
            Start-Sleep -Milliseconds 500
        }
        [math]::Round(((Get-Date) - $start).TotalSeconds, 1)
    }

    Write-Host "==> $Name : disabling the SAR device and waiting for the driver to unload"
    $device = Get-PnpDevice -Class MEDIA -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -contains $hardwareId } | Select-Object -First 1
    $record.stateBefore = & $driverState

    if (-not $device) {
        $record.error = 'no SAR device is present'
    } elseif ($record.stateBefore -ne 'Running') {
        $record.error = "the driver was not running to begin with ($($record.stateBefore))"
    } else {
        $record.instanceId = $device.InstanceId
        try {
            Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
            $record.unloadSeconds = & $waitForState 'Stopped' 30
        } catch {
            $record.error = "the device could not be disabled: $_"
        }
        $record.stateDisabled = & $driverState
        if (-not $record.error -and $record.stateDisabled -ne 'Stopped') {
            $record.error = "the driver did not unload within 30 s of its device being disabled ($($record.stateDisabled))"
        }

        # Enable it again whatever happened, so later scenarios have a device.
        try {
            Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
            $record.reloadSeconds = & $waitForState 'Running' 30
        } catch {
            if (-not $record.error) { $record.error = "the device could not be enabled again: $_" }
        }
        $record.stateAfter = & $driverState
        $after = Get-PnpDevice -InstanceId $device.InstanceId -ErrorAction SilentlyContinue
        $record.deviceStatus = "$($after.Status)"
        $record.deviceProblem = "$($after.Problem)"
        if (-not $record.error -and ($record.stateAfter -ne 'Running' -or $record.deviceStatus -ne 'OK')) {
            $record.error = "the driver did not come back (driver $($record.stateAfter), device $($record.deviceStatus))"
        }
    }

    $record.outcome = if ($record.error) { 'failed' } else { 'passed' }
    Write-Host "    -> $($record.outcome)$(if ($record.error) { ': ' + $record.error })"
    return $record
}

if ($Scenarios -contains 'unload') {
    $summary.scenarios += Invoke-UnloadScenario 'unload-after-use'
    # The reloaded driver has to work, not only load.
    $summary.scenarios += Invoke-Scenario 'run-after-reload' (
        @('run', '--endpoints', $EndpointCounts[0], '--iterations', 1, '--duration', $Duration) + $common)
}

# Asks Plug and Play to disable the SAR device while a host has endpoints and
# clients are streaming through them, which takes the driver through the
# query-remove path with everything open. Windows refuses to remove a device
# that has a handle open on it, so the expected course is: the audio engine
# lets go of its streams, the host's control handle vetoes the removal, and
# the device stays. Whichever way it goes is recorded; what is required is
# that nothing crashes or hangs and that, once everything is closed and the
# device enabled again, the driver works (the scenario that follows).
function Invoke-DeviceDisableScenario {
    param([string]$Name)

    $record = [ordered]@{ name = $Name }
    $pairs = 2
    $hardwareId = 'SW\{0BCFFA5C-E754-48CF-A783-A64C0DC0BB2C}'
    $driverState = {
        $driver = Get-CimInstance Win32_SystemDriver -Filter "Name='SynchronousAudioRouter'" `
            -ErrorAction SilentlyContinue
        if ($driver) { $driver.State } else { 'absent' }
    }
    $deviceState = {
        $d = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue |
            Where-Object { $_.HardwareID -contains $hardwareId } | Select-Object -First 1
        if ($d) { "$($d.Status)/$($d.Problem)" } else { 'absent' }
    }

    $hostLog = Join-Path $ResultsDir "$Name-host.log"
    $hostJson = Join-Path $ResultsDir "$Name-host.json"
    Write-Host "==> $Name : starting a host with $pairs endpoint pairs and streaming through them"
    $hostProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $hostLog `
        -ArgumentList (@('host', '--endpoints', $pairs, '--duration', 120, '--results', "`"$hostJson`"") + $common)
    $null = $hostProc.Handle
    Start-Sleep -Seconds 5

    $wasapiLog = Join-Path $ResultsDir "$Name-wasapi.log"
    $wasapiJson = Join-Path $ResultsDir "$Name-wasapi.json"
    Remove-Item $wasapiJson -ErrorAction SilentlyContinue
    $wasapiProc = Start-Process -FilePath $sarTest -NoNewWindow -PassThru -RedirectStandardOutput $wasapiLog `
        -ArgumentList @('wasapi', '--endpoints', $pairs, '--channels', $Channels, '--duration', 20,
            '--expect-invalidation', '--results', "`"$wasapiJson`"")
    $null = $wasapiProc.Handle
    Start-Sleep -Seconds 5

    $device = Get-PnpDevice -Class MEDIA -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -contains $hardwareId } | Select-Object -First 1
    $record.hostAliveBefore = -not $hostProc.HasExited
    $record.clientAliveBefore = -not $wasapiProc.HasExited
    $record.driverBefore = & $driverState

    if (-not $device) {
        $record.error = 'no SAR device is present'
    } else {
        Write-Host "==> $Name : disabling the SAR device with the host and its clients running"
        $started = Get-Date
        try {
            Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
            $record.disable = 'returned'
        } catch {
            $record.disable = "refused: $_"
        }
        $record.disableSeconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        Start-Sleep -Seconds 5
        $record.deviceAfterDisable = & $deviceState
        $record.driverAfterDisable = & $driverState
        $record.hostAliveAfterDisable = -not $hostProc.HasExited
        $record.clientAliveAfterDisable = -not $wasapiProc.HasExited
    }

    # Close everything: the clients first, then the host.
    $clientExited = $wasapiProc.WaitForExit(60000)
    if (-not $clientExited) { try { $wasapiProc.Kill() } catch { } }
    $record.clientExitedByItself = $clientExited
    if ($clientExited) { $record.clientExitCode = $wasapiProc.ExitCode }
    if (-not $hostProc.HasExited) { try { Stop-Process -Id $hostProc.Id -Force } catch { } }
    $hostExited = $hostProc.WaitForExit(60000)
    Start-Sleep -Seconds 3
    $record.deviceAfterClose = & $deviceState
    $record.driverAfterClose = & $driverState
    if (Test-Path $wasapiJson) {
        try {
            $streams = @((Get-Content $wasapiJson -Raw | ConvertFrom-Json).wasapi.streams)
            $record.streams = $streams.Count
            $record.streamsStarted = @($streams | Where-Object started).Count
            $record.streamsInvalidated = @($streams | Where-Object deviceInvalidated).Count
        } catch { }
    }

    if ($device) {
        try {
            Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
        } catch {
            $record.enableError = "$_"
        }
        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline -and (& $driverState) -ne 'Running') { Start-Sleep -Milliseconds 500 }
        $record.deviceFinal = & $deviceState
        $record.driverFinal = & $driverState
        if (-not $hostExited) {
            $record.error = 'the host process did not exit within 60 s of being killed'
        } elseif ($record.driverFinal -ne 'Running' -or $record.deviceFinal -notlike 'OK/*') {
            $record.error = "the device did not come back (driver $($record.driverFinal), device $($record.deviceFinal))"
        }
    }

    $record.outcome = if ($record.error) { 'failed' } else { 'passed' }
    Write-Host "    -> $($record.outcome)$(if ($record.error) { ': ' + $record.error })"
    return $record
}

if ($Scenarios -contains 'device') {
    $summary.scenarios += Invoke-DeviceDisableScenario 'device-disable-in-use'
    $summary.scenarios += Invoke-Scenario 'run-after-device-disable' (
        @('run', '--endpoints', $EndpointCounts[0], '--iterations', 1, '--duration', $Duration) + $common)
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

Restore-SystemSounds

$summary.finished = (Get-Date).ToString('o')
if ($SurviveOnly) {
    $summary.surviveOnly = $true
    foreach ($scenario in $summary.scenarios) {
        if ($scenario.outcome -eq 'failed') {
            $scenario.strictOutcome = 'failed'
            $scenario.outcome = 'tolerated'
        }
    }
}
$outcomes = $summary.scenarios | ForEach-Object { $_.outcome }
$summary.passed = -not ($outcomes | Where-Object { 'passed', 'skipped', 'tolerated' -notcontains $_ })
$summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $ResultsDir 'summary.json')

Write-Host ''
Write-Host 'Scenario summary:'
$summary.scenarios | ForEach-Object { Write-Host ("  {0,-24} {1}" -f $_.name, $_.outcome) }

if ($outcomes -contains 'hung') { exit 2 }
if ($summary.passed) { exit 0 } else { exit 1 }
