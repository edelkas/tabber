@echo off
rem Builds tabber with MSVC. Imports the compiler environment via vswhere when
rem cl.exe is not already on PATH, or is on it for the wrong architecture.
rem
rem   build.bat                build the tool, 64-bit
rem   build.bat x86            ...32-bit, into build\x86
rem   build.bat test           build and run the test suite (offline tiers)
rem   build.bat x86 test       ...the 32-bit build's own suite
rem   build.bat test online    build and run everything, network included
rem   build.bat gui            also build the graphical front-end
setlocal
cd /d "%~dp0"

rem An optional first argument picks the architecture; everything after it is
rem read the same either way. ARCH ends up equal to %1 only when %1 named one,
rem which is exactly when it has to be shifted out of the way.
set "ARCH=x64"
if /i "%~1"=="x86" set "ARCH=x86"
if /i "%~1"=="x64" set "ARCH=x64"
if /i "%~1"=="%ARCH%" shift

rem 32-bit output lives in its own tree: the object files are not interchangeable
rem with the 64-bit ones sitting in build\, and neither are the executables.
set "OUTDIR=build"
if /i "%ARCH%"=="x86" set "OUTDIR=build\x86"

set "LIBSRC=src\util.c src\platform.c src\resource_save.c src\kv.c src\json.c src\net.c src\md5.c src\inflate.c src\deflate.c src\zip.c src\gzip.c src\paths.c src\digest.c src\config.c src\tabs.c src\server.c src\patch.c src\save.c src\cloud.c src\palettes.c src\loc.c src\keys.c src\install.c src\usage.c src\update.c"
set "SOURCES=src\main.c %LIBSRC%"
set "TESTSRC=test\test_main.c test\test_core.c test\test_archive.c test\test_state.c test\test_save.c test\test_palettes.c test\test_loc.c test\test_keys.c test\test_update.c test\test_game.c test\test_online.c test\fixture_zip.c"
set "CFLAGS=/nologo /W4 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS"

rem ---- The graphical front-end -----------------------------------------------
rem Dear ImGui and GLFW are built from their sources under vendor\, not linked
rem against a prebuilt library: a .lib is per-compiler and per-architecture, and
rem this tree already has two architectures and three operating systems to keep
rem happy. GLFW picks its backend from a define, one per platform.
set "IMGUIDIR=vendor\imgui"
set "GLFWDIR=vendor\glfw"
set "IMGUISRC=%IMGUIDIR%\imgui.cpp %IMGUIDIR%\imgui_draw.cpp %IMGUIDIR%\imgui_tables.cpp %IMGUIDIR%\imgui_widgets.cpp %IMGUIDIR%\imgui_demo.cpp %IMGUIDIR%\backends\imgui_impl_glfw.cpp %IMGUIDIR%\backends\imgui_impl_opengl3.cpp"
set "GLFWSRC=%GLFWDIR%\src\context.c %GLFWDIR%\src\init.c %GLFWDIR%\src\input.c %GLFWDIR%\src\monitor.c %GLFWDIR%\src\platform.c %GLFWDIR%\src\vulkan.c %GLFWDIR%\src\window.c %GLFWDIR%\src\egl_context.c %GLFWDIR%\src\osmesa_context.c %GLFWDIR%\src\null_init.c %GLFWDIR%\src\null_monitor.c %GLFWDIR%\src\null_window.c %GLFWDIR%\src\null_joystick.c"
set "GLFWSRC=%GLFWSRC% %GLFWDIR%\src\win32_init.c %GLFWDIR%\src\win32_joystick.c %GLFWDIR%\src\win32_module.c %GLFWDIR%\src\win32_monitor.c %GLFWDIR%\src\win32_thread.c %GLFWDIR%\src\win32_time.c %GLFWDIR%\src\win32_window.c %GLFWDIR%\src\wgl_context.c"

rem /utf-8 because both libraries carry UTF-8 string literals, and MSVC reads a
rem source file in the system codepage unless told otherwise.
set "GUIINC=/I src /I %IMGUIDIR% /I %IMGUIDIR%\backends /I %GLFWDIR%\include"
set "GUIFLAGS=/nologo /W4 /O2 /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS %GUIINC%"

rem Vendored code is compiled quietly: its warnings are upstream's to fix, and
rem at /W4 they would bury ours. Our own GUI code stays at /W4 like the rest.
set "VENDORFLAGS=/nologo /W1 /O2 /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS /D_GLFW_WIN32 /DUNICODE /D_UNICODE %GUIINC%"
set "GUILIBS=opengl32.lib gdi32.lib user32.lib shell32.lib"

rem A windowed program, so no console is opened alongside it. mainCRTStartup is
rem the console entry point: naming it keeps plain main() and its argv, which
rem the subsystem alone would otherwise send looking for a WinMain.
set "GUILINK=/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem A compiler on PATH is only the right one if it targets what was asked for;
rem VSCMD_ARG_TGT_ARCH is what a Native Tools prompt sets to say which it is.
set "NEED_MSVC="
where cl >nul 2>nul
if errorlevel 1 set "NEED_MSVC=1"
if defined VSCMD_ARG_TGT_ARCH if /i not "%VSCMD_ARG_TGT_ARCH%"=="%ARCH%" set "NEED_MSVC=1"
if defined NEED_MSVC call :setup_msvc
if errorlevel 1 exit /b 1

cl %CFLAGS% /Fe%OUTDIR%\tabber.exe /Fo%OUTDIR%\ %SOURCES%
if errorlevel 1 exit /b 1
echo Built %OUTDIR%\tabber.exe (%ARCH%)

rem The fresh savefile is built into the binary (src\resource_save.c), so the
rem executable is the whole program: nothing has to ship beside it.

if /i "%1"=="gui" goto :build_gui
if /i not "%1"=="test" exit /b 0

if not exist "%OUTDIR%\test" mkdir "%OUTDIR%\test"
cl %CFLAGS% /I src /Fe%OUTDIR%\test\test_tabber.exe /Fo%OUTDIR%\test\ %TESTSRC% %LIBSRC%
if errorlevel 1 exit /b 1
echo Built %OUTDIR%\test\test_tabber.exe (%ARCH%)

if /i "%2"=="full" (
    "%OUTDIR%\test\test_tabber.exe" --full
) else if /i "%2"=="online" (
    "%OUTDIR%\test\test_tabber.exe" --online
) else (
    "%OUTDIR%\test\test_tabber.exe"
)
exit /b %errorlevel%

rem ---------------------------------------------------------------------------
:build_gui
rem Three object trees, because two of these sources are called platform.c:
rem GLFW has one and so do we, and two files of that name cannot share a /Fo
rem directory. Keeping them apart keeps their warning levels apart too.
if not exist "%OUTDIR%\gui\lib" mkdir "%OUTDIR%\gui\lib"
if not exist "%OUTDIR%\gui\vendor" mkdir "%OUTDIR%\gui\vendor"

rem The tool's own code, compiled exactly as the CLI compiles it. The GUI is a
rem second front-end onto the same library, not a second copy of it.
cl %CFLAGS% /c /Fo%OUTDIR%\gui\lib\ %LIBSRC%
if errorlevel 1 exit /b 1

cl %VENDORFLAGS% /c /Fo%OUTDIR%\gui\vendor\ %IMGUISRC% %GLFWSRC%
if errorlevel 1 exit /b 1

rem The libraries WinHTTP and the registry need come from the pragmas in
rem net.c and platform.c, the same way the CLI gets them.
cl %GUIFLAGS% /Fe%OUTDIR%\tabber-gui.exe /Fo%OUTDIR%\gui\ gui\main.cpp %OUTDIR%\gui\lib\*.obj %OUTDIR%\gui\vendor\*.obj /link %GUILIBS% %GUILINK%
if errorlevel 1 exit /b 1
echo Built %OUTDIR%\tabber-gui.exe (%ARCH%)
exit /b 0

rem ---------------------------------------------------------------------------
:setup_msvc
rem Ask vswhere for the newest Visual Studio and import the environment for the
rem architecture wanted. The answer goes through a temp file: paths with
rem parentheses do not survive cmd's quoting rules inside "for /f" blocks.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSLIST=%TEMP%\tabber_vspath.txt"
if not exist "%VSWHERE%" goto :no_msvc

set "VSPATH="
"%VSWHERE%" -latest -products * -property installationPath > "%VSLIST%" 2>nul
if exist "%VSLIST%" set /p VSPATH=<"%VSLIST%"
del "%VSLIST%" >nul 2>nul
if not defined VSPATH goto :no_msvc

rem vcvars32 builds 32-bit with the 32-bit toolchain; vcvars64 the 64-bit one.
set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if /i "%ARCH%"=="x86" set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" goto :no_msvc
call "%VCVARS%" >nul
exit /b 0

:no_msvc
echo build: MSVC not found for %ARCH%. Install the "Desktop development with C++"
echo        workload, or run this script from the matching Native Tools Command
echo        Prompt ("x64 Native Tools" for x64, "x86 Native Tools" for x86).
exit /b 1
