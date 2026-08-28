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
rem   build.bat all            every build this tree produces, both architectures
rem   build.bat gui clean      ...and "clean" anywhere starts that build over
rem
rem Only what changed is compiled. An object older than its source, or than the
rem newest header the group it belongs to might include, is built again; the
rem rest are the ones already sitting in build\, and are linked as they are.
setlocal
cd /d "%~dp0"

rem Compiler and linker diagnostics in English, whatever language the machine
rem speaks: an error message is far easier to look up that way.
set "VSLANG=1033"

rem "clean" is read before the arguments are, so it may sit at either end of
rem them. It discards the object tree of the architecture being built, which is
rem the only way a source that no longer exists stops being linked in.
set "FRESH="
for %%A in (%*) do if /i "%%~A"=="clean" set "FRESH=1"

rem Read, and then out of the way of the arguments that are read by position:
rem it may come before the architecture, after it, or last of the lot.
if /i "%~1"=="clean" shift

rem Every finished executable is copied to dist\ under the name it is released
rem as, which carries the front-end and the platform. Nothing else goes there.
if /i "%~1"=="all" goto :build_all

rem An optional first argument picks the architecture; everything after it is
rem read the same either way. ARCH ends up equal to %1 only when %1 named one,
rem which is exactly when it has to be shifted out of the way.
set "ARCH=x64"
if /i "%~1"=="x86" set "ARCH=x86"
if /i "%~1"=="x64" set "ARCH=x64"
if /i "%~1"=="%ARCH%" shift
if /i "%~1"=="clean" shift

rem 32-bit output lives in its own tree: the object files are not interchangeable
rem with the 64-bit ones sitting in build\, and neither are the executables.
set "OUTDIR=build"
if /i "%ARCH%"=="x86" set "OUTDIR=build\x86"

rem The 32-bit tree lives inside the 64-bit one, so cleaning x64 cleans both.
if defined FRESH if exist "%OUTDIR%" rd /s /q "%OUTDIR%"

rem Where the release copies go, and under what names. "cli" and "gui" name the
rem front-end the same way, so a release page lists the two side by side.
set "DISTDIR=dist"
set "DISTCLI=%DISTDIR%\tabber-cli-windows-%ARCH%.exe"
set "DISTGUI=%DISTDIR%\tabber-gui-windows-%ARCH%.exe"

set "LIBSRC=src\util.c src\platform.c src\resource_save.c src\kv.c src\json.c src\net.c src\md5.c src\inflate.c src\deflate.c src\zip.c src\gzip.c src\paths.c src\digest.c src\config.c src\tabs.c src\server.c src\patch.c src\save.c src\cloud.c src\palettes.c src\loc.c src\keys.c src\install.c src\usage.c src\update.c"
set "SOURCES=src\main.c %LIBSRC%"
set "TESTSRC=test\test_main.c test\test_core.c test\test_archive.c test\test_state.c test\test_save.c test\test_palettes.c test\test_loc.c test\test_keys.c test\test_update.c test\test_game.c test\test_online.c test\fixture_zip.c"
set "CFLAGS=/nologo /W4 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS"

rem What a group of sources depends on besides itself, as patterns separated by
rem semicolons. Any of these headers may be included by any file in its group,
rem and finding out which would mean keeping a dependency file per object; they
rem change seldom enough that rebuilding the group when the newest of them moves
rem costs less than the bookkeeping would.
set "CDEPS=src\*.h"
set "TDEPS=src\*.h;test\*.h"

rem ---- The graphical front-end -----------------------------------------------
rem Dear ImGui and GLFW are built from their sources under vendor\, not linked
rem against a prebuilt library: a .lib is per-compiler and per-architecture, and
rem this tree already has two architectures and three operating systems to keep
rem happy. GLFW picks its backend from a define, one per platform.
set "IMGUIDIR=vendor\imgui"
set "GLFWDIR=vendor\glfw"

rem ForkAwesome is two headers and nothing to compile: the icon font is baked
rem into one of them and the names of its codepoints are in the other.
set "FKDIR=vendor\forkawesome"
set "IMGUISRC=%IMGUIDIR%\imgui.cpp %IMGUIDIR%\imgui_draw.cpp %IMGUIDIR%\imgui_tables.cpp %IMGUIDIR%\imgui_widgets.cpp %IMGUIDIR%\imgui_demo.cpp %IMGUIDIR%\backends\imgui_impl_glfw.cpp %IMGUIDIR%\backends\imgui_impl_opengl3.cpp"
set "GLFWSRC=%GLFWDIR%\src\context.c %GLFWDIR%\src\init.c %GLFWDIR%\src\input.c %GLFWDIR%\src\monitor.c %GLFWDIR%\src\platform.c %GLFWDIR%\src\vulkan.c %GLFWDIR%\src\window.c %GLFWDIR%\src\egl_context.c %GLFWDIR%\src\osmesa_context.c %GLFWDIR%\src\null_init.c %GLFWDIR%\src\null_monitor.c %GLFWDIR%\src\null_window.c %GLFWDIR%\src\null_joystick.c"
set "GLFWSRC=%GLFWSRC% %GLFWDIR%\src\win32_init.c %GLFWDIR%\src\win32_joystick.c %GLFWDIR%\src\win32_module.c %GLFWDIR%\src\win32_monitor.c %GLFWDIR%\src\win32_thread.c %GLFWDIR%\src\win32_time.c %GLFWDIR%\src\win32_window.c %GLFWDIR%\src\wgl_context.c"

set "VDEPS=%IMGUIDIR%\*.h;%IMGUIDIR%\backends\*.h;%GLFWDIR%\include\GLFW\*.h;%GLFWDIR%\src\*.h"
set "GDEPS=src\*.h;%IMGUIDIR%\*.h;%IMGUIDIR%\backends\*.h;%GLFWDIR%\include\GLFW\*.h;%FKDIR%\*.h"

rem /utf-8 because both libraries carry UTF-8 string literals, and MSVC reads a
rem source file in the system codepage unless told otherwise.
set "GUIINC=/I src /I %IMGUIDIR% /I %IMGUIDIR%\backends /I %GLFWDIR%\include /I %FKDIR%"
set "GUIFLAGS=/nologo /W4 /O2 /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS %GUIINC%"

rem Vendored code is compiled quietly: its warnings are upstream's to fix, and
rem at /W4 they would bury ours. Our own GUI code stays at /W4 like the rest.
set "VENDORFLAGS=/nologo /W1 /O2 /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS /D_GLFW_WIN32 /DUNICODE /D_UNICODE %GUIINC%"
rem dwmapi: DwmFlush paces the frames on Windows; see "Pacing the frames".
set "GUILIBS=opengl32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib"

rem A windowed program, so no console is opened alongside it. mainCRTStartup is
rem the console entry point: naming it keeps plain main() and its argv, which
rem the subsystem alone would otherwise send looking for a WinMain.
set "GUILINK=/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"

rem A compiler on PATH is only the right one if it targets what was asked for;
rem VSCMD_ARG_TGT_ARCH is what a Native Tools prompt sets to say which it is.
set "NEED_MSVC="
where cl >nul 2>nul
if errorlevel 1 set "NEED_MSVC=1"
if defined VSCMD_ARG_TGT_ARCH if /i not "%VSCMD_ARG_TGT_ARCH%"=="%ARCH%" set "NEED_MSVC=1"
if defined NEED_MSVC call :setup_msvc
if errorlevel 1 exit /b 1

set "DIRTY="
call :compile "%OUTDIR%" "%CFLAGS%" "%SOURCES%" "%CDEPS%"
if errorlevel 1 exit /b 1
call :link "%OUTDIR%\tabber.exe" "%CFLAGS%" "%OUTDIR%\*.obj" ""
if errorlevel 1 exit /b 1
call :to_dist "%OUTDIR%\tabber.exe" "%DISTCLI%"
if errorlevel 1 exit /b 1

rem The fresh savefile is built into the binary (src\resource_save.c), so the
rem executable is the whole program: nothing has to ship beside it.

if /i "%1"=="gui" goto :build_gui
if /i not "%1"=="test" exit /b 0

set "DIRTY="
call :compile "%OUTDIR%\test" "%CFLAGS% /I src" "%TESTSRC% %LIBSRC%" "%TDEPS%"
if errorlevel 1 exit /b 1
call :link "%OUTDIR%\test\test_tabber.exe" "%CFLAGS%" "%OUTDIR%\test\*.obj" ""
if errorlevel 1 exit /b 1

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
set "DIRTY="

rem The tool's own code, compiled exactly as the CLI compiles it. The GUI is a
rem second front-end onto the same library, not a second copy of it.
call :compile "%OUTDIR%\gui\lib" "%CFLAGS%" "%LIBSRC%" "%CDEPS%"
if errorlevel 1 exit /b 1

rem The two libraries, which only a vendor drop of their own changes.
call :compile "%OUTDIR%\gui\vendor" "%VENDORFLAGS%" "%IMGUISRC% %GLFWSRC%" "%VDEPS%"
if errorlevel 1 exit /b 1

call :compile "%OUTDIR%\gui" "%GUIFLAGS%" "gui\main.cpp" "%GDEPS%"
if errorlevel 1 exit /b 1

rem The libraries WinHTTP and the registry need come from the pragmas in
rem net.c and platform.c, the same way the CLI gets them.
call :link "%OUTDIR%\tabber-gui.exe" "%GUIFLAGS%" "%OUTDIR%\gui\main.obj %OUTDIR%\gui\lib\*.obj %OUTDIR%\gui\vendor\*.obj" "%GUILIBS% %GUILINK%"
if errorlevel 1 exit /b 1
call :to_dist "%OUTDIR%\tabber-gui.exe" "%DISTGUI%"
if errorlevel 1 exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------
:build_all
rem Both architectures, each of them building the CLI and then the GUI. The
rem recursion is what keeps one compiler environment per architecture: each
rem call has its own setlocal, so vcvars64 and vcvars32 never meet.
set "PASS="
if defined FRESH set "PASS=clean"
call "%~f0" x64 gui %PASS%
if errorlevel 1 exit /b 1
call "%~f0" x86 gui %PASS%
if errorlevel 1 exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------
:compile
rem Compiles the sources of one group whose objects are out of date, and leaves
rem DIRTY set behind it if it compiled anything at all. %1 is where the objects
rem go, %2 the flags to compile them with, %3 the sources, %4 the header
rem patterns from above. A group shares a /Fo directory, which is why no two of
rem its sources may be called the same thing.
setlocal
set "TB_OBJDIR=%~1"
set "TB_SRCS=%~3"
set "TB_DEPS=%~4"
if not exist "%TB_OBJDIR%" mkdir "%TB_OBJDIR%"

rem PowerShell answers in one pass what batch cannot ask about file times
rem without spawning a process per file. The sources and the patterns travel in
rem the environment, where neither cmd's quoting nor PowerShell's can reach
rem them. An object nobody can date - it does not exist yet - is out of date.
set "TB_LIST=%TEMP%\tabber_stale_%RANDOM%.txt"
powershell -NoProfile -Command "$d=(Get-ChildItem $env:TB_DEPS.Split(';') -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc | Select-Object -Last 1).LastWriteTimeUtc; foreach($s in $env:TB_SRCS.Split(' ')){ if(-not $s){ continue }; $o=Join-Path $env:TB_OBJDIR ([IO.Path]::GetFileNameWithoutExtension($s)+'.obj'); $t=(Get-Item $o -ErrorAction SilentlyContinue).LastWriteTimeUtc; if(-not $t -or $t -lt (Get-Item $s).LastWriteTimeUtc -or ($d -and $t -lt $d)){ $s } }" > "%TB_LIST%"

rem No PowerShell, no way to tell what changed: compile the group and be sure.
set "STALE="
if errorlevel 1 set "STALE=%TB_SRCS%"
if not defined STALE for /f "usebackq delims=" %%L in ("%TB_LIST%") do call :add_stale "%%L"
del "%TB_LIST%" >nul 2>nul

if not defined STALE goto :compile_current
cl %~2 /c /Fo%TB_OBJDIR%\ %STALE%
if errorlevel 1 goto :compile_failed
endlocal & set "DIRTY=1"
exit /b 0

:compile_current
endlocal
exit /b 0

:compile_failed
endlocal
exit /b 1

:add_stale
rem One line of that answer, appended to the group's list of work.
set "STALE=%STALE% %~1"
exit /b 0

rem ---------------------------------------------------------------------------
:link
rem Links one executable when there is a reason to: something was compiled for
rem it, or it is not there. %1 names it, %2 are the compiler's flags, %3 the
rem objects, %4 whatever the linker itself wants, if anything.
if exist "%~1" if not defined DIRTY goto :link_current
set "LINKTAIL="
if not "%~4"=="" set "LINKTAIL=/link %~4"
cl %~2 /Fe%~1 %~3 %LINKTAIL%
if errorlevel 1 exit /b 1
echo Built %~1 (%ARCH%)
exit /b 0

:link_current
echo %~1 is up to date (%ARCH%)
exit /b 0

rem ---------------------------------------------------------------------------
:to_dist
rem Copies one finished executable to dist\, over whatever is already there.
if not exist "%DISTDIR%" mkdir "%DISTDIR%"
copy /y %1 %2 >nul
if errorlevel 1 (
    echo build: could not copy %~1 to %~2
    exit /b 1
)
echo   -^> %~2
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

rem vcvars complains on stderr about tools it looked for and did not need, in
rem whatever language the machine speaks, and says nothing about whether it
rem worked. Both go quiet; what settles it is having a compiler afterwards.
call "%VCVARS%" >nul 2>nul
where cl >nul 2>nul
if errorlevel 1 goto :no_msvc
exit /b 0

:no_msvc
echo build: MSVC not found for %ARCH%. Install the "Desktop development with C++"
echo        workload, or run this script from the matching Native Tools Command
echo        Prompt ("x64 Native Tools" for x64, "x86 Native Tools" for x86).
exit /b 1
