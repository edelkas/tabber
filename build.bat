@echo off
rem Builds tabber with MSVC. Imports the compiler environment via vswhere when
rem cl.exe is not already on PATH.
rem
rem   build.bat            build the tool
rem   build.bat test       build and run the test suite (offline tiers)
rem   build.bat test full  build and run everything, network included
setlocal
cd /d "%~dp0"

set "OUTDIR=build"
set "LIBSRC=src\util.c src\platform.c src\kv.c src\json.c src\net.c src\md5.c src\inflate.c src\deflate.c src\zip.c src\gzip.c src\paths.c src\digest.c src\config.c src\tabs.c src\server.c src\patch.c src\save.c src\cloud.c src\palettes.c src\install.c"
set "SOURCES=src\main.c %LIBSRC%"
set "TESTSRC=test\test_main.c test\test_core.c test\test_archive.c test\test_state.c test\test_save.c test\test_palettes.c test\test_game.c test\test_online.c test\fixture_zip.c"
set "CFLAGS=/nologo /W4 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

where cl >nul 2>nul
if errorlevel 1 call :setup_msvc
if errorlevel 1 exit /b 1

cl %CFLAGS% /Fe%OUTDIR%\tabber.exe /Fo%OUTDIR%\ %SOURCES%
if errorlevel 1 exit /b 1
echo Built %OUTDIR%\tabber.exe

rem The fresh savefile ships beside the executable, as it will when packaged.
if exist "res\nprofile.zip" (
    if not exist "%OUTDIR%\res" mkdir "%OUTDIR%\res"
    copy /y "res\nprofile.zip" "%OUTDIR%\res\nprofile.zip" >nul
)

if /i not "%1"=="test" exit /b 0

if not exist "%OUTDIR%\test" mkdir "%OUTDIR%\test"
cl %CFLAGS% /I src /Fe%OUTDIR%\test\test_tabber.exe /Fo%OUTDIR%\test\ %TESTSRC% %LIBSRC%
if errorlevel 1 exit /b 1
echo Built %OUTDIR%\test\test_tabber.exe

if /i "%2"=="full" (
    "%OUTDIR%\test\test_tabber.exe" --full
) else if /i "%2"=="online" (
    "%OUTDIR%\test\test_tabber.exe" --online
) else (
    "%OUTDIR%\test\test_tabber.exe"
)
exit /b %errorlevel%

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
