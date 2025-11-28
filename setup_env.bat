@echo off
REM This script sets up the Visual Studio environment for command-line builds.
REM Please verify the path to vcvarsall.bat matches your installation.

set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VS_PATH%" (
    set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvarsall.bat"
)
if not exist "%VS_PATH%" (
    set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
)

if exist "%VS_PATH%" (
    call "%VS_PATH%" x64
) else (
    echo Error: Could not find vcvarsall.bat. Please edit setup_env.bat with the correct path.
    exit /b 1
)
