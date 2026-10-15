#Requires -Version 7
<#
.SYNOPSIS
Fetches the pinned NVIDIA Streamline SDK (cmake/streamline.cmake) into externals/Streamline.

.DESCRIPTION
Downloads the pinned release zip, verifies its SHA-256, extracts it into externals/Streamline and
writes a .verified-sha256 marker there. A second run is a no-op.

An existing externals/Streamline without a matching marker (for example an SDK unpacked by hand) is
not replaced unless -Force or -From is given.

Exit codes: 0 success (including no-op), 1 failure, 2 usage error.

.PARAMETER From
Use this local release zip instead of downloading (offline use, or a kept copy of the pinned
release). It gets the same SHA-256 check and is never modified or deleted. It also allows replacing
an existing tree that has no matching marker.

.PARAMETER Force
Discard an existing extraction and extract again.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$From,
    [switch]$Force,
    # Catches unknown options so they become a usage error (exit 2) instead of a binding error.
    [Parameter(ValueFromRemainingArguments)]
    [string[]]$Unexpected
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ($Unexpected -or ($PSBoundParameters.ContainsKey('From') -and [string]::IsNullOrWhiteSpace($From))) {
    if ($Unexpected) { [Console]::Error.WriteLine("fetch-streamline: unexpected argument: $($Unexpected -join ' ')") }
    else { [Console]::Error.WriteLine("fetch-streamline: -From needs a path") }
    [Console]::Error.WriteLine("usage: tools/fetch-streamline.ps1 [-From <zip>] [-Force]")
    exit 2
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-Sha256 {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Extracts every entry under $Destination, rejecting anything that would land outside it.
function Expand-ZipSafely {
    param([string]$ZipPath, [string]$Destination)
    $destRoot = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            $segments = $name -split '[\\/]'
            if ([System.IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or ($segments -contains '..')) {
                throw "unsafe entry in the archive: '$name'"
            }
            $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($destRoot, $name))
            if (-not $target.StartsWith($destRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "archive entry escapes the target directory: '$name'"
            }
            if ($name.EndsWith('/') -or $name.EndsWith('\')) {
                [void][System.IO.Directory]::CreateDirectory($target)
                continue
            }
            [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($target))
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Read-Pin {
    param([string]$Text, [string]$Name, [string]$Pattern)
    $match = [regex]::Match($Text, "set\(\s*$Name\s+`"($Pattern)`"\s*\)")
    if (-not $match.Success) { throw "$Name is missing or malformed in cmake/streamline.cmake" }
    $match.Groups[1].Value
}

$staging = $null
try {
    $repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $pinFile = Join-Path $repoRoot 'cmake/streamline.cmake'
    if (-not (Test-Path -LiteralPath $pinFile)) { throw "pin file not found: cmake/streamline.cmake" }
    $pinText = Get-Content -LiteralPath $pinFile -Raw
    $version = Read-Pin $pinText 'TOX2_STREAMLINE_VERSION' '[0-9]+\.[0-9]+\.[0-9]+'
    $asset = Read-Pin $pinText 'TOX2_STREAMLINE_ASSET' '[A-Za-z0-9._-]+\.zip'
    $expectedHash = (Read-Pin $pinText 'TOX2_STREAMLINE_SHA256' '[0-9A-Fa-f]{64}').ToLowerInvariant()

    $finalDir = Join-Path $repoRoot 'externals/Streamline'
    $markerFile = Join-Path $finalDir '.verified-sha256'
    # Download cache and staging live under generated/, which is git-ignored.
    $depsDir = Join-Path $repoRoot 'generated/deps/streamline'
    $downloadDir = Join-Path $depsDir 'download'

    # Resolved against the caller's location; verified before anything on disk is touched.
    $zipPath = $null
    if ($From) {
        $zipPath = [System.IO.Path]::GetFullPath($From, $PWD.ProviderPath)
        if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) { throw "-From file not found: $From" }
        $actual = Get-Sha256 $zipPath
        if ($actual -ne $expectedHash) {
            throw "SHA-256 mismatch for -From file '$From': expected $expectedHash, got $actual"
        }
    }

    $verified = (Test-Path -LiteralPath $markerFile -PathType Leaf) -and
        ((Get-Content -LiteralPath $markerFile -Raw).Trim() -eq $expectedHash)
    if ($verified -and -not $Force) {
        "streamline: up to date ($version)"
        exit 0
    }
    # A tree this script did not write may hold local changes or another SDK version.
    if ((Test-Path -LiteralPath $finalDir) -and -not $verified -and -not $Force -and -not $From) {
        throw ("externals/Streamline exists but has no matching .verified-sha256 marker; it was left untouched.`n" +
            "  Use -Force to replace it with the pinned download, or -From <zip> to replace it from a local copy of the release.")
    }

    if (-not $zipPath) {
        $zipPath = Join-Path $downloadDir $asset
        # A cached download is reused only if it still matches the pin.
        if ((Test-Path -LiteralPath $zipPath -PathType Leaf) -and (Get-Sha256 $zipPath) -ne $expectedHash) {
            Remove-Item -LiteralPath $zipPath -Force
        }
        if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
            $url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/$asset"
            [void][System.IO.Directory]::CreateDirectory($downloadDir)
            $partPath = "$zipPath.part"
            "streamline: downloading $url"
            try {
                Invoke-WebRequest -Uri $url -OutFile $partPath
            }
            catch {
                if (Test-Path -LiteralPath $partPath) { Remove-Item -LiteralPath $partPath -Force }
                throw "download failed: $($_.Exception.Message)`n  The pinned release may have been removed; use -From with a copy of the zip."
            }
            $actual = Get-Sha256 $partPath
            if ($actual -ne $expectedHash) {
                Remove-Item -LiteralPath $partPath -Force
                throw "SHA-256 mismatch for the download: expected $expectedHash, got $actual"
            }
            Move-Item -LiteralPath $partPath -Destination $zipPath -Force
        }
    }

    # Extract beside the download cache, then move into place, so a failure leaves no partial tree.
    [void][System.IO.Directory]::CreateDirectory($depsDir)
    $staging = Join-Path $depsDir ".staging-$PID"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    [void][System.IO.Directory]::CreateDirectory($staging)
    "streamline: extracting $version"
    Expand-ZipSafely $zipPath $staging

    # The SDK root is the directory holding include/sl.h: the zip root or its single top-level directory.
    $extracted = $null
    if (Test-Path -LiteralPath (Join-Path $staging 'include/sl.h') -PathType Leaf) {
        $extracted = $staging
    }
    else {
        $topLevel = @(Get-ChildItem -LiteralPath $staging -Directory)
        if ($topLevel.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $topLevel[0].FullName 'include/sl.h') -PathType Leaf)) {
            $extracted = $topLevel[0].FullName
        }
    }
    if (-not $extracted) { throw "the archive does not contain 'include/sl.h'" }
    foreach ($path in 'bin/x64/sl.interposer.dll', 'bin/x64/sl.common.dll', 'bin/x64/sl.dlss.dll',
        'bin/x64/nvngx_dlss.dll', 'license.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path $extracted $path) -PathType Leaf)) {
            throw "the archive does not contain '$path'"
        }
    }

    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $finalDir))
    if (Test-Path -LiteralPath $finalDir) { Remove-Item -LiteralPath $finalDir -Recurse -Force }
    if ($extracted -eq $staging) {
        # The staging directory itself becomes the SDK tree, so the cleanup must not touch it afterwards.
        Move-Item -LiteralPath $staging -Destination $finalDir
        $staging = $null
    }
    else {
        Move-Item -LiteralPath $extracted -Destination $finalDir
    }
    Set-Content -LiteralPath $markerFile -Value $expectedHash -NoNewline
    "streamline: ready in externals/Streamline"
    exit 0
}
catch {
    [Console]::Error.WriteLine("fetch-streamline: $($_.Exception.Message)")
    exit 1
}
finally {
    if ($staging -and (Test-Path -LiteralPath $staging)) {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}
