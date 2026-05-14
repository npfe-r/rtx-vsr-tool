@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

if "%1"=="" ( set "CONFIG=Release" ) else ( set "CONFIG=%1" )

echo ============================================
echo  RTX VSR Tool — Build (CMake Project)
echo  Config: %CONFIG%
echo ============================================
echo.

:: --- CMake ---
where "%CMAKE%" >nul 2>&1 || where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found. Install from https://cmake.org/
    exit /b 1
)

:: --- Visual Studio via vswhere ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VS_PATH="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%a in (`"%VSWHERE%" -latest -property installationPath`) do set "VS_PATH=%%a"
)

if not defined VS_PATH (
    for %%p in (
        "D:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Community"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    ) do if exist "%%~p\VC\Auxiliary\Build\vcvars64.bat" set "VS_PATH=%%~p"
)

if not defined VS_PATH (
    echo [ERROR] Visual Studio 2022 not found.
    exit /b 1
)

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found.
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Failed to initialize VS environment.
    exit /b 1
)

:: --- Configure ---
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo [1/3] CMake configure...
"%CMAKE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -DCMAKE_CUDA_ARCHITECTURES="75;86;89;100"
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

:: --- Build ---
echo [2/3] Building %CONFIG%...
"%CMAKE%" --build "%BUILD_DIR%" --config "%CONFIG%"
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

:: --- Verify output ---
set "OUT_EXE=%BUILD_DIR%\%CONFIG%\RTX_VSR_Tool.exe"
if not exist "%OUT_EXE%" (
    echo [ERROR] Output binary not found: %OUT_EXE%
    exit /b 1
)

echo.
echo ============================================
echo  [3/3] OK — %CONFIG%
echo  %OUT_EXE%
echo ============================================

endlocal