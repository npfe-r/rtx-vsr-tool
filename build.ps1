#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Build RTX VSR Tool (CMake Project)
.DESCRIPTION
    CMake configure + build for RTX VSR Tool using Visual Studio 2022.
    Run this script from the RTX_VSR_CMake directory.
.PARAMETER Config
    Build config: Debug, Release, MinSizeRel, RelWithDebInfo (default: Release)
.PARAMETER Fresh
    Delete CMake cache before configuring
.PARAMETER Clean
    Run cmake --build --target clean before building
.EXAMPLE
    .\build.ps1
    .\build.ps1 -Config Debug -Fresh
    .\build.ps1 -Config Release -Clean
#>

param(
    [ValidateSet("Debug", "Release", "MinSizeRel", "RelWithDebInfo")]
    [string]$Config = "Release",
    [switch]$Fresh,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ScriptDir   = Split-Path -Parent $PSCommandPath
$BuildDir    = Join-Path $ScriptDir "build"

# --- CMake ---
$cmake = Get-Command "cmake" -ErrorAction SilentlyContinue
if (-not $cmake) {
    $paths = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )
    $cmake = ($paths | Where-Object { Test-Path $_ } | Select-Object -First 1)
    if (-not $cmake) { throw "CMake not found. Install from https://cmake.org/" }
}
$cmakePath = $cmake.Path

# --- Visual Studio ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" }

$vsPath = if (Test-Path $vswhere) { & $vswhere -latest -property installationPath 2>$null }

if (-not $vsPath) {
    $vsPath = @(
        "D:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Community"
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools"
    ) | Where-Object { Test-Path "$_\VC\Auxiliary\Build\vcvars64.bat" } | Select-Object -First 1
}
if (-not $vsPath) { throw "Visual Studio 2022 not found." }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

Write-Host "============================================" -ForegroundColor Cyan
Write-Host " RTX VSR Tool — Build (CMake Project)" -ForegroundColor Cyan
Write-Host " Config: $Config" -ForegroundColor Cyan
Write-Host "============================================"
""

# --- Setup VS environment ---
Write-Host "[0] VS environment..." -ForegroundColor Green
cmd /c "call `"$vcvars`" > nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

# --- Prepare build directory ---
if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }

if ($Fresh -and (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Host "  (fresh: removing cache)" -ForegroundColor Yellow
    Remove-Item (Join-Path $BuildDir "CMakeCache.txt") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Clean) {
    Write-Host "  (cleaning...)" -ForegroundColor Yellow
    & $cmakePath --build $BuildDir --config $Config --target clean *>$null
}

# --- Configure ---
Write-Host "[1/3] CMake configure..." -ForegroundColor Green
& $cmakePath -S $ScriptDir -B $BuildDir -G "Visual Studio 17 2022" -DCMAKE_CUDA_ARCHITECTURES="75;86;89;100"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }

# --- Build ---
Write-Host "[2/3] Building $Config..." -ForegroundColor Green
& $cmakePath --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# --- Copy SDK DLLs ---
$OutDir = Join-Path $BuildDir $Config
$SdkBin = Join-Path $ScriptDir "..\RTX_Video_SDK_v1.1.0\bin\Windows\x64\rel"
foreach ($dll in @("nvngx_vsr.dll", "nvngx_truehdr.dll")) {
    $src = Join-Path $SdkBin $dll
    $dst = Join-Path $OutDir $dll
    if (Test-Path $src) {
        Copy-Item $src $dst -Force
        Write-Host "  + $dll" -ForegroundColor Gray
    }
}

""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  [3/3] OK — $Config" -ForegroundColor Green
Write-Host "  $OutDir\RTX_VSR_Tool.exe" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Cyan