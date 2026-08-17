@echo off
rem Builds tabber with MSVC. Imports the compiler environment via vswhere when
rem cl.exe is not already on PATH.
setlocal
cd /d "%~dp0"

set "OUTDIR=build"
set "SOURCES=src\main.c src\util.c src\platform.c src\kv.c src\json.c src\net.c src\md5.c src\inflate.c src\zip.c src\paths.c src\digest.c src\config.c src\tabs.c src\install.c"
set "CFLAGS=/nologo /W4 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

where cl >nul 2>nul
if errorlevel 1 call :setup_msvc
if errorlevel 1 exit /b 1

cl %CFLAGS% /Fe%OUTDIR%\tabber.exe /Fo%OUTDIR%\ %SOURCES%
if errorlevel 1 exit /b 1

echo Built %OUTDIR%\tabber.exe
exit /b 0

rem ---------------------------------------------------------------------------
:setup_msvc
rem Ask vswhere for the newest Visual Studio and import its x64 environment.
rem The answer goes through a temp file: paths with parentheses do not survive
rem cmd's quoting rules inside "for /f" blocks.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSLIST=%TEMP%\tabber_vspath.txt"
if not exist "%VSWHERE%" goto :no_msvc

set "VSPATH="
"%VSWHERE%" -latest -products * -property installationPath > "%VSLIST%" 2>nul
if exist "%VSLIST%" set /p VSPATH=<"%VSLIST%"
del "%VSLIST%" >nul 2>nul
if not defined VSPATH goto :no_msvc

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :no_msvc
call "%VCVARS%" >nul
exit /b 0

:no_msvc
echo build: MSVC not found. Install the "Desktop development with C++" workload,
echo        or run this script from an "x64 Native Tools Command Prompt".
exit /b 1
