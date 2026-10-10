#Requires -Version 7
<#
.SYNOPSIS
Fetches the pinned Microsoft.GameInput NuGet package (cmake/gameinput.cmake) into
generated/deps/gameinput.

.DESCRIPTION
Downloads the pinned .nupkg, verifies its SHA-256, extracts it into
generated/deps/gameinput/<version>/ and writes a .verified-sha256 marker there. A second run is a
no-op.

Exit codes: 0 success (including no-op), 1 failure, 2 usage error.

.PARAMETER From
Use this local .nupkg instead of downloading (offline use, or a kept copy of the pinned package).
It gets the same SHA-256 check and is never modified or deleted.

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
    if ($Unexpected) { [Console]::Error.WriteLine("fetch-gameinput: unexpected argument: $($Unexpected -join ' ')") }
    else { [Console]::Error.WriteLine("fetch-gameinput: -From needs a path") }
    [Console]::Error.WriteLine("usage: tools/fetch-gameinput.ps1 [-From <nupkg>] [-Force]")
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
    if (-not $match.Success) { throw "$Name is missing or malformed in cmake/gameinput.cmake" }
    $match.Groups[1].Value
}

$staging = $null
try {
    $repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $pinFile = Join-Path $repoRoot 'cmake/gameinput.cmake'
    if (-not (Test-Path -LiteralPath $pinFile)) { throw "pin file not found: cmake/gameinput.cmake" }
    $pinText = Get-Content -LiteralPath $pinFile -Raw
    $version = Read-Pin $pinText 'DAL_GAMEINPUT_VERSION' '[0-9]+\.[0-9]+\.[0-9]+'
    $expectedHash = (Read-Pin $pinText 'DAL_GAMEINPUT_SHA256' '[0-9A-Fa-f]{64}').ToLowerInvariant()
    $packageFile = "microsoft.gameinput.$version.nupkg"

    $depsDir = Join-Path $repoRoot 'generated/deps/gameinput'
    $downloadDir = Join-Path $depsDir 'download'
    $finalDir = Join-Path $depsDir $version
    $markerFile = Join-Path $finalDir '.verified-sha256'

    # Resolved against the caller's location; verified before anything on disk is touched.
    $packagePath = $null
    if ($From) {
        $packagePath = [System.IO.Path]::GetFullPath($From, $PWD.ProviderPath)
        if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf)) { throw "-From file not found: $From" }
        $actual = Get-Sha256 $packagePath
        if ($actual -ne $expectedHash) {
            throw "SHA-256 mismatch for -From file '$From': expected $expectedHash, got $actual"
        }
    }

    $verified = (Test-Path -LiteralPath $markerFile -PathType Leaf) -and
        ((Get-Content -LiteralPath $markerFile -Raw).Trim() -eq $expectedHash)
    if ($verified -and -not $Force) {
        "gameinput: up to date ($version)"
        exit 0
    }

    if (-not $packagePath) {
        $packagePath = Join-Path $downloadDir $packageFile
        # A cached download is reused only if it still matches the pin.
        if ((Test-Path -LiteralPath $packagePath -PathType Leaf) -and (Get-Sha256 $packagePath) -ne $expectedHash) {
            Remove-Item -LiteralPath $packagePath -Force
        }
        if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf)) {
            $url = "https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/$version/$packageFile"
            [void][System.IO.Directory]::CreateDirectory($downloadDir)
            $partPath = "$packagePath.part"
            "gameinput: downloading $url"
            try {
                Invoke-WebRequest -Uri $url -OutFile $partPath
            }
            catch {
                if (Test-Path -LiteralPath $partPath) { Remove-Item -LiteralPath $partPath -Force }
                throw "download failed: $($_.Exception.Message)`n  The pinned version may have been unlisted; use -From with a copy of the .nupkg."
            }
            $actual = Get-Sha256 $partPath
            if ($actual -ne $expectedHash) {
                Remove-Item -LiteralPath $partPath -Force
                throw "SHA-256 mismatch for the download: expected $expectedHash, got $actual"
            }
            Move-Item -LiteralPath $partPath -Destination $packagePath -Force
        }
    }

    # Extract beside the final location, then move into place, so a failure leaves no partial tree.
    [void][System.IO.Directory]::CreateDirectory($depsDir)
    $staging = Join-Path $depsDir ".staging-$PID"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    $extracted = Join-Path $staging 'package'
    [void][System.IO.Directory]::CreateDirectory($extracted)
    "gameinput: extracting $version"
    Expand-ZipSafely $packagePath $extracted

    foreach ($path in 'native/include/GameInput.h', 'native/lib/x64/GameInput.lib', 'LICENSE.txt',
        'NOTICE.txt', 'redist/GameInputRedist.msi') {
        if (-not (Test-Path -LiteralPath (Join-Path $extracted $path) -PathType Leaf)) {
            throw "the package does not contain '$path'"
        }
    }
    if (Test-Path -LiteralPath $finalDir) { Remove-Item -LiteralPath $finalDir -Recurse -Force }
    Move-Item -LiteralPath $extracted -Destination $finalDir
    Set-Content -LiteralPath $markerFile -Value $expectedHash -NoNewline
    "gameinput: ready in generated/deps/gameinput/$version"
    exit 0
}
catch {
    [Console]::Error.WriteLine("fetch-gameinput: $($_.Exception.Message)")
    exit 1
}
finally {
    if ($staging -and (Test-Path -LiteralPath $staging)) {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}
