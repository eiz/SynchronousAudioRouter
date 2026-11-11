<#
.SYNOPSIS
    Sign files with the release code signing certificate, whose key is held in
    DigiCert KeyLocker.

.DESCRIPTION
    Signs each file with signtool by certificate thumbprint, with a SHA-256
    digest and a DigiCert RFC 3161 timestamp, then verifies the signature.

    signtool reaches the key through DigiCert's key storage provider, so the
    KeyLocker client tools have to be installed and the certificate has to be
    in the current user's certificate store. If either is missing the script
    sets it up from the environment, which is how it runs in CI:

      SM_HOST                   KeyLocker host, https://clientauth.one.digicert.com
      SM_API_KEY                API token of the signing identity
      SM_CLIENT_CERT_FILE       path of its client authentication certificate (.p12)
        or SM_CLIENT_CERT_FILE_B64, the same file base64-encoded
      SM_CLIENT_CERT_PASSWORD   password of that .p12

    On a machine where the DigiCert ONE Clients app already set this up,
    nothing but the thumbprint is needed.

    Use the 64-bit signtool: the provider is 64-bit, and a 32-bit signtool
    finds the certificate but not its key.

.NOTES
    Designed for Windows PowerShell 5.1 and pwsh 7+.
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string[]]$Files,

    # SHA-1 thumbprint of the code signing certificate.
    [string]$Thumbprint = $env:SM_CODE_SIGNING_CERT_SHA1_HASH,

    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $Thumbprint) {
    throw "No certificate thumbprint: pass -Thumbprint or set SM_CODE_SIGNING_CERT_SHA1_HASH."
}
$Thumbprint = $Thumbprint -replace '\s', ''

$toolsDir = Join-Path $env:ProgramFiles 'DigiCert\DigiCert Keylocker Tools'

function Find-Smctl {
    $onPath = Get-Command smctl.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $installed = Join-Path $toolsDir 'smctl.exe'
    if (Test-Path $installed) { return $installed }
    return $null
}

function Test-CertificateInStore {
    return Test-Path "Cert:\CurrentUser\My\$Thumbprint"
}

# 1. The KeyLocker client tools (smctl and the key storage provider).
$smctl = Find-Smctl
if (-not $smctl) {
    if (-not $env:SM_API_KEY) {
        throw "The KeyLocker client tools aren't installed and SM_API_KEY isn't set to download them."
    }

    $msi = Join-Path ([IO.Path]::GetTempPath()) 'Keylockertools-windows-x64.msi'
    Write-Host "==> Downloading the KeyLocker client tools"
    Invoke-WebRequest `
        'https://one.digicert.com/signingmanager/api-ui/v1/releases/Keylockertools-windows-x64.msi/download' `
        -Headers @{ 'x-api-key' = $env:SM_API_KEY } -OutFile $msi

    # This installer gets to see the signing credentials, so only run it if
    # DigiCert signed it.
    $signature = Get-AuthenticodeSignature $msi
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.Subject -notmatch 'O=DigiCert') {
        throw "The KeyLocker tools installer isn't validly signed by DigiCert ($($signature.Status))."
    }

    $install = Start-Process msiexec.exe -ArgumentList '/i', "`"$msi`"", '/quiet', '/qn' -PassThru -Wait
    if ($install.ExitCode -ne 0) { throw "KeyLocker tools install failed (exit $($install.ExitCode))." }

    $smctl = Find-Smctl
    if (-not $smctl) { throw "smctl.exe not found after installing the KeyLocker tools." }

    # The installer normally registers the key storage provider itself. If it
    # already has, this reports an error that doesn't matter; if the provider
    # really is missing, the sync or signtool below fails and says so.
    & $smctl windows ksp register
    if ($LASTEXITCODE -ne 0) { Write-Warning "smctl windows ksp register exited with $LASTEXITCODE." }
}

# smctl looks for its companion tools on the PATH.
$env:PATH = "$(Split-Path $smctl);$env:PATH"

# 2. The certificate in this user's store, linked to the provider.
if (-not (Test-CertificateInStore)) {
    if (-not $env:SM_CLIENT_CERT_FILE -and $env:SM_CLIENT_CERT_FILE_B64) {
        $p12 = Join-Path ([IO.Path]::GetTempPath()) 'keylocker-client-auth.p12'
        [IO.File]::WriteAllBytes($p12, [Convert]::FromBase64String($env:SM_CLIENT_CERT_FILE_B64))
        $env:SM_CLIENT_CERT_FILE = $p12
    }

    foreach ($name in 'SM_HOST', 'SM_API_KEY', 'SM_CLIENT_CERT_FILE', 'SM_CLIENT_CERT_PASSWORD') {
        if (-not (Get-Item "Env:$name" -ErrorAction SilentlyContinue)) {
            throw "The certificate isn't in the store yet and $name isn't set to sync it."
        }
    }

    Write-Host "==> smctl windows certsync"
    & $smctl windows certsync
    if ($LASTEXITCODE -ne 0) { throw "smctl windows certsync failed (exit $LASTEXITCODE)." }

    if (-not (Test-CertificateInStore)) {
        throw "Certificate $Thumbprint isn't in the store after the sync. Is the thumbprint right, and may this identity sign with it?"
    }
}

# 3. Sign and verify.
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" `
                -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "The 64-bit signtool.exe wasn't found - is the Windows SDK installed?" }

foreach ($file in $Files) {
    $path = (Resolve-Path $file).ProviderPath
    Write-Host "==> Signing $path"
    & $signtool.FullName sign /sha1 $Thumbprint /fd sha256 /tr $TimestampUrl /td sha256 $path
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed for $path (exit $LASTEXITCODE)." }

    & $signtool.FullName verify /pa $path
    if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $path (exit $LASTEXITCODE)." }
}
