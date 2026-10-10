#Requires -Version 7
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'debug',
    [switch]$Clean,
    [switch]$Test,
    [string]$EbootElf,
    [string]$GeneratedDir,
    [string]$GameDir,
    [string]$LlvmBin = $env:TOX2_LLVM_BIN
)

$ErrorActionPreference = 'Stop'

# The Windows SDK environment comes from the VS DevShell; skip if already inside one.
if (-not $env:VSCMD_VER) {
    if (-not (Get-Module -ListAvailable -Name VSSetup)) {
        throw "Install-Module VSSetup -Scope CurrentUser"
    }
    Import-Module VSSetup
    $vsInstance = Get-VSSetupInstance | Select-VSSetupInstance -Version '[18.0,19.0)'
    if ($null -eq $vsInstance) {
        throw "Visual Studio 18.x (VS 2026) with the Windows SDK was not found."
    }
    Import-Module (Join-Path $vsInstance.InstallationPath "Common7/Tools/Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell $vsInstance.InstanceId -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
}

# Prepended after the DevShell so it wins over the clang bundled with VS.
if ($LlvmBin) {
    $env:PATH = "$LlvmBin$([System.IO.Path]::PathSeparator)$env:PATH"
}

# Capture fully: stopping the pipeline early with Select-Object leaves $LASTEXITCODE unset.
$clangVersion = & clang --version
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$clangVersion | Select-Object -First 1

# Resolved against the caller's location, before Push-Location. The directory need not exist:
# CMake reports a missing one with the emit command to run.
if ($GeneratedDir) {
    $GeneratedDir = [System.IO.Path]::GetFullPath($GeneratedDir, $PWD.ProviderPath)
}
if ($GameDir) {
    $GameDir = [System.IO.Path]::GetFullPath($GameDir, $PWD.ProviderPath)
}

Push-Location (Join-Path $PSScriptRoot '..')
try {
    if ($Clean) {
        $buildDir = Join-Path (Get-Location) "build/$Preset"
        if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
    }

    $configureArgs = @('--preset', $Preset)
    if ($EbootElf) {
        $configureArgs += "-DTOX2_EBOOT_ELF=$((Resolve-Path -LiteralPath $EbootElf).Path)"
    }
    if ($GeneratedDir) {
        $configureArgs += "-DTOX2_PPU_GENERATED_DIR=$GeneratedDir"
    }
    if ($GameDir) {
        $configureArgs += "-DTOX2_GAME_DIR=$GameDir"
    }
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $buildTimer = [System.Diagnostics.Stopwatch]::StartNew()
    & cmake --build --preset $Preset
    $buildExit = $LASTEXITCODE
    $buildTimer.Stop()
    "build: {0:N1} s" -f $buildTimer.Elapsed.TotalSeconds
    if ($buildExit -ne 0) { exit $buildExit }

    if ($Test) {
        & ctest --preset $Preset
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
finally {
    Pop-Location
}
