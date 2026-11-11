<#
.SYNOPSIS
    Check that a signed driver is the same binary as an unsigned build.

.DESCRIPTION
    A signature is added to a PE file without changing the code it covers, so
    a signed .sys and the unsigned build it came from have the same
    Authenticode hash. This script computes that hash for both files and fails
    if they differ. The release workflow uses it to make sure the driver that
    came back from Partner Center is the one CI built from the commit being
    released, and CI uses it on the test-signed package so the check itself is
    exercised on every build.

    With -RequireMicrosoftSignature it also checks that the signature is valid
    and was made by Microsoft's hardware publisher certificate, which is what
    Partner Center signs drivers with and what lets Windows load one without
    test signing.

.NOTES
    Designed for Windows PowerShell 5.1 and pwsh 7+.
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Unsigned,
    [Parameter(Mandatory)] [string]$Signed,
    [switch]$RequireMicrosoftSignature
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The SHA-256 Authenticode hash of a PE file: the whole file except the
# checksum, the certificate table's directory entry and the certificate table
# itself, which are the parts signing rewrites.
function Get-AuthenticodeHash([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $Path).ProviderPath)
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)

    if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) {
        throw "$Path is not a PE file."
    }

    $optional = $pe + 24
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    $checksum = $optional + 64
    # Data directory 4 (the certificate table); the directories start 16 bytes
    # later in a PE32+ header than in a PE32 one.
    $certEntry = $optional + $(if ($magic -eq 0x20b) { 144 } else { 128 })
    $certOffset = [BitConverter]::ToUInt32($bytes, $certEntry)
    $end = if ($certOffset) { $certOffset } else { $bytes.Length }

    $sha = [Security.Cryptography.SHA256]::Create()
    [void]$sha.TransformBlock($bytes, 0, $checksum, $null, 0)
    [void]$sha.TransformBlock(
        $bytes, $checksum + 4, $certEntry - ($checksum + 4), $null, 0)
    [void]$sha.TransformBlock(
        $bytes, $certEntry + 8, $end - ($certEntry + 8), $null, 0)

    # Signing pads the file to a multiple of 8 bytes before it appends the
    # certificate table, and the padding is part of the hash.
    if (-not $certOffset -and ($bytes.Length % 8)) {
        $padding = New-Object byte[] (8 - $bytes.Length % 8)
        [void]$sha.TransformBlock($padding, 0, $padding.Length, $null, 0)
    }

    [void]$sha.TransformFinalBlock($bytes, 0, 0)
    return -join ($sha.Hash | ForEach-Object { $_.ToString('x2') })
}

$unsignedHash = Get-AuthenticodeHash $Unsigned
$signedHash = Get-AuthenticodeHash $Signed
Write-Host "==> Authenticode SHA-256"
Write-Host "    $unsignedHash  $Unsigned"
Write-Host "    $signedHash  $Signed"

if ($unsignedHash -ne $signedHash) {
    throw "The signed driver is not the same binary as the unsigned build."
}

if ($RequireMicrosoftSignature) {
    $signature = Get-AuthenticodeSignature $Signed
    $signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '(none)' }
    Write-Host "==> Signature: $($signature.Status), signer: $signer"

    if ($signature.Status -ne 'Valid') {
        throw "The signed driver's signature is $($signature.Status)."
    }

    if ($signer -notmatch 'CN=Microsoft Windows Hardware Compatibility Publisher') {
        throw "The signed driver isn't signed by Microsoft's hardware publisher certificate."
    }
}

Write-Host "==> OK"
