# tabber

A cross-platform CLI tool (C99) to install custom tabs (mappacks) for the game
**N++**. Work in progress; a [Dear ImGui front-end](#the-graphical-front-end)
has just been started now that the command line side is complete.

## Table of contents

- [Table of contents](#table-of-contents)
- [Building](#building)
- [The graphical front-end](#the-graphical-front-end)
   * [The window's frame](#the-windows-frame)
   * [Pacing the frames](#pacing-the-frames)
   * [When a tab was last played](#when-a-tab-was-last-played)
- [Usage](#usage)
- [Tests](#tests)
- [Layout](#layout)
- [Environment](#environment)
- [How the directories are found](#how-the-directories-are-found)
- [The digest](#the-digest)
- [Fetching a custom tab](#fetching-a-custom-tab)
- [Installing a tab](#installing-a-tab)
- [Redirecting the server](#redirecting-the-server)
   * [The developer credit](#the-developer-credit)
- [The library check](#the-library-check)
- [The server check](#the-server-check)
- [The savefile](#the-savefile)
   * [Compressed or not](#compressed-or-not)
   * [What makes it safe](#what-makes-it-safe)
- [Steam Cloud](#steam-cloud)
- [The palettes](#the-palettes)
   * [Names](#names)
   * [The limit](#the-limit)
   * [Undoing](#undoing)
- [The in-game texts](#the-in-game-texts)
   * [Which languages](#which-languages)
   * [Putting them back](#putting-them-back)
- [The controls](#the-controls)
   * [Putting them back](#putting-them-back-1)
   * [At the tab's own request](#at-the-tabs-own-request)
- [Uninstalling](#uninstalling)
   * [The older installers](#the-older-installers)
- [Where tabber keeps its files](#where-tabber-keeps-its-files)
- [State](#state)
- [Updating tabber](#updating-tabber)
   * [Replacing a running program](#replacing-a-running-program)
   * [When it looks](#when-it-looks)
   * [Cutting a release](#cutting-a-release)
   * [What this does and does not protect](#what-this-does-and-does-not-protect)
- [Status](#status)

## Building

| Platform | Command | Notes |
| --- | --- | --- |
| Windows (MSVC), 64-bit | `build.bat` | Imports the MSVC environment automatically via `vswhere` |
| Windows (MSVC), 32-bit | `build.bat x86` | The same, through `vcvars32`; lands in `build\x86\` |
| Windows, everything | `build.bat all` | Both architectures, each with its CLI and its GUI |
| Linux / macOS  | `make`      | Any C99 compiler; needs libcurl (`libcurl4-openssl-dev` or equivalent) |
| Windows (MinGW)| `make`      | Links `advapi32`, `ole32`, `shell32`, `uuid`, `winhttp` |
| Any, everything | `make all` | The CLI and the GUI, for this machine |

The binary lands in `build/tabber[.exe]`. HTTP goes through WinHTTP on Windows
(no dependency) and libcurl elsewhere, both behind the two-function API in
`src/net.h`.

Every finished executable is also copied to `dist/` under the name it is
released as — `tabber-<cli|gui>-<os>-<arch>`, with `.exe` on Windows — over
whatever was there before. Nothing else is put in that directory, which is what
lets [`make_manifest.py`](#cutting-a-release) simply read it. `build.bat all`
is two recursive calls, one per architecture, each with its own `setlocal`, so
`vcvars64` and `vcvars32` never meet. `make all` builds the pair for the host,
which is the only architecture a Makefile run has; bare `make` still builds the
tool alone, since the GUI wants OpenGL and the window system's headers that a
machine only after the CLI need not have.

The 32-bit build is a first-class one, not an afterthought: it is the same
source with nothing conditional on the word size, and the whole suite is run
against it. It keeps its own tree because the object files of the two are not
interchangeable, and so is `build.bat x86 test`. Both builds read the same
[tool folder](#where-tabber-keeps-its-files), so whichever one is run sees the
same tab store and the same record. An architecture given to `build.bat` comes first and
everything after it reads as usual, so `build.bat x86 test online` is the
32-bit run of what `build.bat test online` runs. A compiler already on `PATH`
is used only if it targets what was asked for; otherwise the right `vcvars` is
imported over it.

Where the word size shows is in the places the system splits in two, and both
are already handled for other reasons: a 32-bit process reads Steam's registry
key through the WoW64 redirector, which lands on the same 32-bit view the
64-bit build reaches by naming `Wow6432Node` outright, and `%ProgramFiles%`
means the x86 folder to it, which is where Steam is anyway. Both are among the
candidates each build already tries in turn, and every candidate has to look
like Steam before it is believed.

## The graphical front-end

[Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b on GLFW 3.5 and OpenGL 3,
built by `build.bat gui` (or `build.bat x86 gui`, or `make gui`) into
`build/tabber-gui[.exe]`. It is a separate program from `build/tabber[.exe]`,
but not a separate copy of the tool: it links the same objects the CLI is built
from, so everything below the presentation is the code `tabber` runs. The
headers in `src/` carry `extern "C"` for it.

It draws one window, filling the viewport whatever size it is dragged to. At
the top, its own [title bar](#the-windows-frame); under that a button that
refreshes the catalogue — `update`, in so many words — and when the copy on
disk was last written. Below them, every custom tab in a sortable table: the
four columns `list` prints, plus two the CLI has no use for.

| Column | |
| --- | --- |
| `CODE` `NAME` `AUTHOR(S)` `RELEASED` | What `list` prints, from the same headers |
| `LAST USED` | When the tab was last played, in words: `3 days ago`, `Never` |
| `INSTALL` | One button, saying what can be done to this tab and doing it |

The column names and the two cells that are not shown as stored (the code goes
up, the date keeps its `YYYY-MM-DD` part) live in `digest.h` and are shared, so
the two front-ends cannot drift apart. It opens sorted by `RELEASED`, newest
first, which is the order the digest is read in; clicking a header sorts by
that one instead. The order,
the column widths and the window's own settings are remembered in the
[tool's folder](#where-tabber-keeps-its-files) rather than in whichever
directory the program was started from.

The table is ten rows tall and scrolls, or shorter when there are fewer tabs
than that: it is told its height rather than handed the rest of the window, so
it ends where its last row does instead of trailing an empty band down to the
bottom edge. Every row's button is cut to the same width, the widest of the
three labels, so the column does not twitch as tabs change state.

The button in each row is the tab's state: **Download** when its files are not
in the store, green **Install** when they are, red **Uninstall** for the one
that is in the game. Each runs the call the matching command runs. Installing
over another tab is the one case the CLI refuses and this does not: it asks
first, and on a yes uninstalls the other one before installing this one. Either
way the outcome is a dialog — what happened, or why it did not.

None of that is believed for longer than five minutes. Whether a tab is
downloaded, which one is installed and when each was last played can all change
without tabber being told: the game rewrites the savefile as it is played, and
the CLI can install something from another window. So the whole table is worked
out again on a timer, and after anything this program does.

Two things it does not do yet. **The work runs on the drawing thread**, so a
download or an install freezes the window until it finishes — the frame before
it draws an overlay saying what is running, which is a caption on the problem
rather than a fix. Moving it to a worker is the next thing this file needs. And
the built-in font is ProggyClean, which covers Latin and little else: a tab
whose author writes their name in anything further out gets `?` where the CLI
prints the character. Fixing that means bundling a font, which is a decision
about size and licence rather than about code.

### The window's frame

The program is linked for the windowed subsystem, so no console opens beside
it: `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` under MSVC — naming the console
entry point is what keeps a plain `main` and its `argv` — and `-mwindows` under
MinGW. The CLI stays a console program, as it must.

The window is also asked for undecorated (`GLFW_DECORATED` off), and the bar
across its top is drawn with everything else. That buys one look on every
platform, and costs the three things the system frame did for free, all of
which are put back by hand in `gui/main.cpp`:

| | |
| --- | --- |
| The buttons | Minimise, maximise/restore and close, at the right end. The glyphs are drawn as lines rather than typed, the built-in font having no symbols for them |
| Dragging | Pressing anywhere else on the bar moves the window; the point taken hold of stays under the pointer. Double-clicking it maximises |
| The edges | Within a few pixels of one the pointer changes shape, and pulling moves that edge. Corners move two at once |

Both drags ask GLFW where the pointer is rather than reading it from Dear
ImGui: the window is moving out from under the pointer at that moment, and a
position left over from the last poll would fight the one being set now. Every
edge is measured from where the window stood when it was grabbed, so a drag
that doubles back lands exactly where it started, and one that runs past the
minimum size stops that edge instead of pushing the opposite one along.

The edges are checked before anything else in the panel is drawn, which is what
gives them first refusal on the pointer — the title bar reaches the very top of
the window, and the top edge has to win there.

What this does not get is the window manager's own gestures: no snapping to a
screen half, and no system shadow. Doing better means a native hit test per
platform — `WM_NCHITTEST` on Windows — rather than the portable arithmetic
here.

### Pacing the frames

A Dear ImGui program draws the whole window every frame, which is cheap: at 60
frames a second on an i5-7300U, all of tabber's own work — polling, working out
the table, building the draw lists — comes to **0.42 ms** of the 16.7 ms a
frame has. The rest is spent waiting for the display, and the waiting is where
the cost was.

`glfwSwapInterval(1)` hands that wait to the graphics driver. Intel's OpenGL
driver does not sleep through it: it polls, about half in user code and half in
the kernel, so the window cost **a whole core** to leave sixty identical frames
on screen. Sampling the busy thread put 73% of it in `ntdll` and 26% in
`win32u`, which is a syscall being asked the same question over and over. GLFW
knows this trick — `vendor/glfw/src/wgl_context.c` paces windowed frames with
`DwmFlush` rather than the swap interval — but only for Windows 7 and older;
on anything newer the interval goes to the driver, which is where it is
supposed to belong.

Two changes, both in `gui/main.cpp`:

| | |
| --- | --- |
| The wait sleeps | On Windows the swap interval is left at zero and `DwmFlush` does the waiting, blocking on the compositor's next vertical blank at no cost. Elsewhere the driver keeps the job, which elsewhere it does properly |
| Frames are earned | The loop blocks in `glfwWaitEventsTimeout` until something happens, rather than redrawing an unchanging window. It wakes on its own four times a second, because the savefile and the clock behind `LAST USED` both change without an event to announce it |

Measured idle, as a share of a four-thread machine:

| | idle | while the pointer moves |
| --- | --- | --- |
| before | 26% | 26% |
| `DwmFlush` alone | 6% | 6% |
| both | **0.65%** | 6% |

Waking is not quite the whole story: Dear ImGui answers a click over the frames
*after* the one that received it, and a click here asks for work that only
starts once the overlay naming it has been drawn. So a wake buys `SETTLE_FRAMES`
frames rather than one, and anything still under way — a held button, a dragged
edge, a click whose work has not run — keeps topping that up.

`DwmFlush` wants a desktop compositor, which a remote session can be without.
If it ever fails the swap interval goes back on and the driver has the job
again, spin and all: a warm laptop beats a window that never draws.

One more thing worth writing down, found while working out why a sibling
project on the same libraries and the same driver did not have this problem. It
asks for an OpenGL 3.0 context and no profile; tabber asks for 3.2 core,
because that is the floor the Dear ImGui backend documents and the oldest core
profile macOS will hand out. On this driver the compatibility path *blocks*
where the core path spins — the same program, changing nothing but the context
hint, idles at 11% of a core instead of 105%. That is the driver's business
rather than ours, and pacing the frames ourselves makes it moot, but it is
worth knowing that the two paths through a driver need not behave alike.

### When a tab was last played

Nothing records this. The game does not tell us and tabber only ever sees the
moments a tab went in or came out, so what `src/usage.c` reads instead is the
savefile, which the game rewrites every time it is played. Which file that is
depends on where the tab stands:

| The tab is | Dated by |
| --- | --- |
| installed | the live savefile, `nprofile.gz` or `nprofile` — it is this tab's for as long as the tab is in place |
| uninstalled | `uninstall_date` in `config.json`: when its save stopped being the live one |
| uninstalled, with nothing written down | `nprofile_<code>.zip`, the archive that uninstall left — which is what a tab one of the [older installers](#the-older-installers) handled looks like |

A tab with none of those has never been played, which is a real answer rather
than a missing one. An installed tab with no savefile at all falls back to its
`install_date`: the game has not been run since, so the install is the most
recent thing that happened to it.

Both libraries are vendored under `vendor/` and compiled from source along with
everything else, rather than linked against a prebuilt `glfw3.lib`. A static
library is tied to one compiler, one architecture and one operating system,
and this tree already has two architectures and three operating systems to keep
happy; the sources cost a rebuild and no more. GLFW picks its window backend
from a define — `_GLFW_WIN32`, `_GLFW_COCOA` or `_GLFW_X11` — and the build
scripts pass the one that matches, along with that platform's list of files.

`vendor/` holds only the files that are actually compiled or included: the five
Dear ImGui sources plus the GLFW and OpenGL 3 backends, and the GLFW files for
the three platforms. The CMake build, the examples, the tests, the docs, the
other twenty backends and the Wayland protocol definitions are not there, which
is 560 of the 634 files the two projects ship. Their licences are, since both
require them. Vendored code is compiled at a low warning level: its warnings
are upstream's to fix, and at `/W4` they would bury ours. `gui/main.cpp` is
compiled at the same level as the rest of the project.

| Platform | Needs |
| --- | --- |
| Windows | Nothing beyond the compiler; links `opengl32`, `gdi32`, `user32`, `shell32` |
| macOS | The Cocoa, IOKit, QuartzCore and OpenGL frameworks, all part of the SDK |
| Linux | X11 headers: `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `libxext-dev` and `libgl-dev`, or the equivalents |

The Linux build is X11 only. GLFW's Wayland backend needs its protocol headers
generated by `wayland-scanner` at build time, which is a code generation step
this Makefile has no business knowing about; under a Wayland session XWayland
runs the window all the same. Adding it later means restoring four `wl_*` files
and `deps/wayland/`, and teaching the Makefile the scanner.

## Usage

```
tabber [options] [command]

  paths            Locate N++'s installation and personal directories (default)
  list             List the custom tabs available in the digest
  update           Download the latest digest of custom tabs
  fetch CODE       Download, verify and unpack the custom tab CODE
  remove CODE      Delete the downloaded files of the custom tab CODE
  install CODE     Install the custom tab CODE into the game (fetching it if needed)
  uninstall CODE   Restore the game's original files, undoing an install
  upgrade          Update tabber itself to the newest release
  bind LIST        Give the players in LIST (e.g. 1,2) the first one's controls
  unbind [LIST]    Restore the controls 'bind' changed, or clear LIST's
  check            Verify the game library matches the recorded state
  server           Check that the 3rd party server is up

  -b, --bare       Machine-readable output: paths or tab-separated fields only
  -v, --verbose    Print extra detail
  -o, --offline    Skip the automatic digest refresh, use the cached copy
      --no-update-check
                   Do not look for a newer tabber on this run
  -c, --force-compress
                   Gzip the savefile put in place, when the game reads gzip
      --cloud-mode MODE
                   What to do with Steam Cloud's copy of the savefile:
                   replace (default), remove, or keep it untouched
      --on-palette-collision MODE
                   What to do when a bundled palette's name is taken:
                   skip (default), replace it, or suffix it with a number
      --keep-palettes
                   Leave the tab's palettes in the game when uninstalling
      --languages LIST
                   Which languages of the in-game texts a tab replaces:
                   all (default), none, or a comma-separated list
  -h, --help       Show help
  -V, --version    Show version
```

Exit status is `0` on success, `1` when a directory could not be found, `2` when
an operation failed (network, corrupt digest) and `3` on a usage error. Errors
and warnings go to stderr, so stdout stays parseable.

## Tests

```
build.bat test          Windows: build and run the offline suite
build.bat test online   ...including the tests that need the network
build.bat test full     ...including the sweep over every published tab
build.bat x86 test      ...any of the three against the 32-bit build
make test / test-online / test-full     Linux and macOS
```

The suite lives in `test/` and links the same objects as the tool, so it
exercises the real code rather than a copy of it. It runs in three tiers:

| Tier | Needs | Covers |
| --- | --- | --- |
| offline (default) | nothing | strings, paths, JSON, KeyValues, MD5, ZIP, DEFLATE both ways, gzip, state file, server resolution, savefile swapping, Steam Cloud, palettes, in-game texts, player controls, replacing the running binary, install/uninstall/patching against a stand-in game |
| `--online` | the network | downloading the live digest, fetching a tab and verifying it |
| `--full` | the network | downloading and verifying **every** published tab |

Nothing in the suite touches a real N++ installation, a real savefile or the
tool's own state. Each test builds a scratch world: a temporary tool root (via
`TABBER_HOME`), a stand-in game (via `TABBER_GAME_DIR`) with the game's level
files and a library carrying the official URI, a personal folder (via
`TABBER_PERSONAL_DIR`) and a Steam folder with invented accounts in it (via
`TABBER_STEAM_DIR`). The scratch area is deleted when the run ends.

Some things are worth knowing about how the tests check what they check:

- **Decompression** is judged by CRC-32, computed over the bytes the
  decompressor produced and compared with what the archive recorded. In the
  full tier that is an independent verdict on every file of every tab.
- **Corruption** is swept byte by byte through the compressed data of the
  embedded fixture: every single-byte flip must be caught.
- **Compression** is judged by decompressing it again: a spread of 120
  generated inputs — runs, skews, alphabets of every width, sizes across the
  block boundaries — has to come back byte for byte, as does the real
  savefile tabber ships.
- **Refusals** assert not just the error but that nothing moved: files, backups
  and the library are all re-checked afterwards.
- **Palettes** are checked from both sides: that the tab's go in, and that a
  palette of the user's own is never quietly lost — not to a collision, not to
  a rolled-back install, and not to the uninstall that follows one.
- **In-game texts** are judged on the whole file: the string table is compared
  byte for byte against what it should read, and a table that has been written
  to and then restored has to be the file it started as, down to its line
  endings.
- **The controls** are checked the same way, on a stand-in `keys.vars` written
  out in full: a bind that gets the three settings right but disturbs a comment,
  a blank line or somebody else's key fails, and a bind followed by an unbind
  has to give back the identical file.
- **The server** the offline tier probes is `127.0.0.1:9`, a port nothing
  listens on, so the health check is exercised for real — a refused connection
  — without leaving the machine. The live server is only asked in `--online`.
- The embedded ZIP fixture (`test/fixture_zip.c`) deliberately mixes a stored
  directory entry, a deflated file, and a deflate *stored block*, so all three
  paths through the decompressor are used without any network.

When a feature lands, its tests land with it in the same tier as the code they
cover. The suite covers the tool, not the
[front-end](#the-graphical-front-end): a window that has to be opened and drawn
is not something this suite is built to judge. What it does cover is everything
the front-end shares with the CLI, which is deliberately most of it — the
column headers and the two cells a listing reshapes are in `digest.c` for that
reason, the timestamps it shows are in `util.c`, and working out when a tab was
last played is `usage.c`, all three checked there. What stands in for the rest
is that both architectures compile and run it.

## Layout

| File | Purpose |
| --- | --- |
| `src/main.c`     | CLI entry point and argument parsing |
| `src/paths.c/.h` | Discovery of N++'s installation and personal directories; all Steam/N++ layout constants |
| `src/digest.c/.h`| The custom tab catalogue: fetch, cache, parse, look up, and the columns both front-ends list it in |
| `src/tabs.c/.h`  | Downloading, verifying and unpacking a custom tab |
| `src/install.c/.h` | Installing a custom tab into the game |
| `src/usage.c/.h` | When each tab was last played, read off whichever savefile is its own |
| `src/patch.c/.h` | Redirecting the game's server queries, the developer credit, and the library health check |
| `src/palettes.c/.h` | The palettes a tab bundles: names, the game's limit, copying them in and out |
| `src/loc.c/.h`   | The game's own texts (`loc.txt`): replacing them per language, and putting them back |
| `src/keys.c/.h`  | The player controls (`keys.vars`): binding several players to one set of keys |
| `src/update.c/.h` | Updating tabber itself: release manifests, versions, replacing the running binary |
| `src/save.c/.h`  | Archiving and swapping the savefile |
| `src/cloud.c/.h` | The savefile's copies in Steam Cloud, per Steam account |
| `src/gzip.c/.h`  | Reading and writing gzip streams (RFC 1952), for gzipped savefiles |
| `src/server.c/.h` | Which 3rd party server to point the game at |
| `src/config.c/.h`| The tool's own configuration and state (`config.json`) |
| `src/kv.c/.h`    | Parser for Valve's KeyValues format (`.vdf`, `.acf`) |
| `src/json.c/.h`  | Minimal JSON parser (RFC 8259) |
| `src/zip.c/.h`   | In-memory ZIP reader with CRC-32 verification, and a stored-entry writer |
| `src/inflate.c/.h` | DEFLATE decompressor (RFC 1951) |
| `src/deflate.c/.h` | DEFLATE compressor (RFC 1951) |
| `src/md5.c/.h`   | MD5 digest (RFC 1321), for download integrity |
| `src/net.c/.h`   | HTTPS client: WinHTTP on Windows, libcurl elsewhere |
| `src/platform.c/.h` | OS abstraction: filesystem, environment, Windows registry, UTF-8 paths, the tool's own folder |
| `src/util.c/.h`  | Allocation, string, buffer, timestamp and error helpers |
| `src/version.h`  | Program name and version |
| `src/resource.h` | Files built into the binary |
| `src/resource_save.c` | The fresh savefile as bytes; generated, do not edit |
| `gui/main.cpp`   | Entry point of the [graphical front-end](#the-graphical-front-end) |
| `test/`          | The test suite (see above) |
| `res/`           | The source of the embedded files: the fresh savefile |
| `tools/`         | Release-time scripts: embedding a resource, writing a manifest |
| `vendor/`        | Dear ImGui and GLFW, pruned to the files the GUI compiles |

## Environment

| Variable | Effect |
| --- | --- |
| `TABBER_HOME` | Use this directory as the tool's root instead of the standard one, for `config.json`, the cached digest and `tabs/` |
| `TABBER_GAME_DIR` | Use this N++ installation directory instead of asking Steam |
| `TABBER_PERSONAL_DIR` | Use this N++ personal directory instead of the usual per-platform one |
| `TABBER_STEAM_DIR` | Use this Steam directory instead of the one found in the registry (this is where the cloud saves are) |
| `TABBER_FRESH_SAVE` | Use this archive as the fresh savefile instead of the built-in one |
| `TABBER_EXE` | Treat this file as the running executable when updating, so a swap can be rehearsed on a copy |
| `TABBER_UPDATED` | Set on the process an update restarts, so it does not go looking again |

These are meant for tests and for unusual setups (a portable copy, a game Steam
does not know about); none is needed in normal use.

## How the directories are found

**Installation directory** (Steam app `230270`):

1. Steam's base folder comes from the registry (`HKLM\SOFTWARE\Wow6432Node\Valve\Steam\InstallPath`,
   the 64-bit variant, then `HKCU\Software\Valve\Steam\SteamPath`) and, failing that,
   from the platform's default locations. A candidate is accepted if it holds
   the Steam launcher or a `steamapps` folder.
2. `steamapps/libraryfolders.vdf` is parsed to enumerate every Steam library.
   The library whose `apps` block lists `230270` wins; otherwise each library is
   probed for `steamapps/appmanifest_230270.acf`.
3. That manifest's `AppState->installdir` names the folder inside
   `steamapps/common`. The result is accepted only if it contains the game's
   `NPP` assets folder.

**Personal directory** (savefile, editor levels): derived from the user's own
folders — `Documents\Metanet\N++` on Windows (resolved through the shell, so a
relocated Documents folder works), `~/Documents/Metanet/N++` on macOS, and
`$XDG_DATA_HOME/Metanet/N++` on Linux with a Proton-prefix fallback.

Every returned path is canonicalised, so its separators and letter case match
what is actually on disk.

## The digest

The catalogue of supported custom tabs is a JSON digest published at
[`db/mappacks/digest.json`](https://github.com/edelkas/inne/blob/master/db/mappacks/digest.json)
(the tool fetches the `raw.githubusercontent.com` equivalent, since the blob URL
serves HTML). It is cached as `digest.json` in [the tool's own folder](#where-tabber-keeps-its-files).

- The cache is refreshed from the network at most **once per session**, the
  first time something needs it — so `list` is up to date without `update`.
- `update` forces a refresh, and is the command to run more often than that.
- A download is only written to disk after it parses and contains a `tabs`
  array, and it is staged in a `.tmp` file then swapped in, so a failed or
  interrupted refresh can never corrupt the cache.
- When the network is unavailable the cached copy is used and a warning is
  printed to stderr; `--offline` skips the refresh entirely.

`DIGEST_URL` can be overridden at build time (`-DDIGEST_URL='"…"'`) to point the
tool at a staging server.

## Fetching a custom tab

`tabber fetch CODE` downloads the tab's ZIP from the link in the digest and
unpacks it into `tabs/<code>/` in [the tool's own folder](#where-tabber-keeps-its-files), preserving the
archive's own layout (`Levels/`, `Palettes/<name>/`, `AUTHORS`, `SCORES`).

Everything is checked in memory, before a single byte reaches the disk:

| Check | Against |
| --- | --- |
| Downloaded size | `download.size` |
| MD5 of the archive | `download.md5` |
| Total uncompressed size | `disk.size` |
| Level files present under `config.levels_dir` | `disk.level_files` |
| Challenge files present under `config.levels_dir` | `disk.challenge_files` |
| CRC-32 of every entry | the archive's own central directory |
| Entry paths | rejected if absolute or containing `..` |

Only once all of them pass is the tab written out, replacing any previous copy
of that same tab. A failed fetch reports the reason and leaves nothing behind;
a tab that was already installed and verified earlier is left untouched rather
than being deleted because a later download went wrong.

`tabber remove CODE` is the reverse: it deletes `tabs/<code>/` and everything in
it, then clears `downloaded` and stamps `remove_date`. The entry itself stays,
so a tab that is fetched again keeps its history. Removal is a local operation
and works with no network and no digest at all. Removing a tab that is not
downloaded changes nothing and reports it, rather than restamping the date.

Because the code becomes a directory that is deleted recursively, it is checked
first: letters and digits only, so `..`, `a/b` and friends are refused outright.

## Installing a tab

`tabber install CODE` fetches the tab first if it is not in the store, then
replaces the game's level and challenge files with the tab's own, in
`<installation dir>/NPP/<config.levels_dir>/`, copies in the palettes it
bundles, replaces a handful of the game's own texts, binds the players the tab
asks for to one set of controls, patches the library and swaps the savefile. Each original is kept next to it
with `OG` appended to the whole file name (`SI.txt` -> `SI.txtOG`), a name the
game does not parse, so uninstalling means deleting the tab's files and renaming
the originals back.

**Only one custom tab can be installed at a time.** Before installing (but after
downloading, which touches nothing in the game) `install_detect` decides whether
a tab is already in place and refuses if so. Today it reads the `installed`
flags in `config.json`; it takes the game paths as well, so later versions can
weigh evidence from the game folder itself without changing any caller.

Only files the game actually reads are copied: a tab file must be listed in
`config.level_files` or `config.challenge_files` in the digest, otherwise it is
left alone and reported as skipped, which does not stop the install.

Everything is checked before the first rename, and the whole set of files is
read into memory before any of it is written:

| Check | Abort reason |
| --- | --- |
| The library check passes | the game is not in a state we recognise |
| No other tab installed | another tab is in place |
| Every file the tab replaces exists in the game | the game is missing files |
| No `OG` backup name is taken by a folder | a rename would fail part way through |
| The game folder accepts writes | permissions, or the game is running |
| The library still carries the official URI, once | it looks patched already |
| The new URI fits in the original's length | the server address is too long |
| The game's text table is readable | `loc.txt` is missing, or is not the game's |

That last one matters: without it, installing over an existing backup would
overwrite a pristine original with a modded file. An `OG` **file** that is
already there is a different matter, and no longer refuses the install: see
[the older installers](#the-older-installers). If a rename or write still
fails part-way through, the files already done are rolled back, so a failed
install leaves the game folder exactly as it was.

## Redirecting the server

Installing also points the game at a 3rd party server, so custom tabs get their
own leaderboards. The main library (`npp.dll`, `libnpp.so`, or
`libnpp.dylib` inside `N++.app/Contents/Frameworks/` on macOS) carries the
server URI as a plain string; it is overwritten in place with the 3rd party URI
plus the tab's code as the first path component, then padded with NUL bytes to
the original's exact length, so nothing in the binary moves:

```
https://dojo.nplusplus.ninja   ->   http://outte.ovh:8126/ctp\0\0\0
```

The address is looked up in four places, first match wins:

1. the `server` object in `config.json` — `{"scheme": "http", "host": ..., "port": ...}`
2. the `server` object in the digest, same shape
3. the built-in host, `outte.ovh:8126`
4. the built-in address, `45.32.150.168:8126`, used when the host stops resolving

`scheme` is optional and defaults to HTTP, which the game also assumes for a
URI without one. That is what lets the `http://` prefix be dropped when the URI
would otherwise not fit — necessary for an address literal, since
`http://45.32.150.168:8126/ctp` is 29 bytes against a budget of 28.

The official URI must appear exactly once, or the install is refused.


### The developer credit

The library carries one more string worth changing, and the older installers
changed it: the developer credit the game renders, `Metanet Software`. While a
tab is installed it names the tab's author instead, and uninstalling puts
Metanet's name back.

```
Metanet Software   ->   fluxdrive\0\0\0\0\0\0\0
```

Same rules as the URI: overwritten in place, NUL-padded, never longer than the
original's 16 bytes. The author comes from the digest, reduced to what the
game can draw — its font has nothing outside ASCII, so everything else is
dropped, which is exactly what the old installers did (`flux͢ɕdrive` becomes
`fluxdrive`, and the two write the same bytes). A cut that lands on a space
loses it, so the credit never ends mid-gap.

Unlike the URI, the credit has no known set of values to search for, so it is
found by what it currently says: the original when a tab is going in, the
tab's author when one is coming out. That is enough to undo an install made by
an older installer, since both derive the same text from the same digest. Three
things have to hold before sixteen bytes are overwritten — the text occurs
exactly once, a NUL sits in front of it, and nothing but padding sits behind
it — and if any fails the credit is left alone and reported. It is cosmetic, so
it never fails an install or an uninstall.

## The library check

`tabber check` reads the library, works out which URI it carries, and compares
that with what `config.json` says is installed:

| Recorded | Library | Verdict |
| --- | --- | --- |
| nothing installed | official URI | healthy |
| tab X installed | 3rd party URI ending in `/X` | healthy |
| nothing installed | 3rd party URI ending in `/X` | healthy: X is installed, by something that was not tabber |
| tab X installed | official URI | no tab is really installed |
| tab X installed | URI ending in `/Y` | the game is serving a different tab |
| either | a URI from nowhere we know | patched by another tool |

The third row is the one worth dwelling on. A tab in the game that tabber has
no record of is not damage: it is what an installer that came before tabber
leaves, since none of them kept any state. So it passes, `check` names the tab
it found, and uninstalling it works — see
[the older installers](#the-older-installers). Installing over it is still
refused, because the library already names a tab.

The verdict is written to `state.library` in `config.json` every time the check
runs, and the check runs automatically before every install and uninstall, so
neither ever starts from a state we do not understand.

Recognising a patched library does not depend on knowing which form was
written: every URI from all four sources is searched for, with and without the
`http://` prefix. The search deliberately stops at the port, so the tab code
that follows can be compared with the one we expect.

## The server check

`tabber server` asks the 3rd party server — resolved exactly as above — whether
it is up, by requesting `/health` with a 5-second timeout:

```
  address   http://outte.ovh:8126/health (from the built-in host)
  reply     HTTP 404
Server check passed: outte.ovh is listening.
```

Any reply is a pass. The endpoint is not implemented yet and answers 404, which
is still worth having: it proves something is listening on that host and port.
Only a connection that never gets an answer at all — refused, timed out, name
not resolved — is a failure. When the endpoint arrives it will answer 200 with
diagnostics, and that will pass on the same terms.

The check also runs automatically during an install, once the library has been
found to carry the official URI and just before it is patched. It is purely
diagnostic there: a server that is briefly down for maintenance is no reason to
refuse an install, so a failure is only reported as a warning and the install
goes ahead. Uninstalling does not check, since it points the game back at the
official servers.

## The savefile

Installing a tab swaps N++'s savefile too, and this is the one thing tabber
touches that Steam cannot put back, so the whole design is built around never
overwriting a save that is not already archived somewhere.

Everything lives in N++'s personal folder, in the layout the previous per-tab
installers used, so both can be used on the same machine:

| File | What it is |
| --- | --- |
| `nprofile` / `nprofile.gz` | the live savefile |
| `nprofile_original.zip` | the vanilla save, archived when a tab is installed |
| `nprofile_<code>.zip` | that tab's save, archived when it is uninstalled |

Installing archives the live save as `nprofile_original.zip` and puts the tab's
own `nprofile_<code>.zip` in its place — or, the first time a tab is played,
the fresh save tabber ships. That one is built into the executable
(`src/resource_save.c`, generated from `res/nprofile.zip`, with a gzipped save
inside), so there is nothing to ship beside the binary; a `res/nprofile.zip` on
disk still wins if there is one, which is what `TABBER_FRESH_SAVE` uses.
Uninstalling is the same trade the other way round, and if there is no
`nprofile_original.zip` to come back to, the shipped save stands in for it. The
archives are ordinary ZIPs holding one entry named after the file that went in,
exactly as the old installers wrote them — and the ones those installers
already left behind are read as they are, ZIP64 fields and all (rubyzip writes
those even for small entries).

### Compressed or not

Since TEN++ the game keeps the save gzipped, falling back to the uncompressed
file when there is no gzipped one, and older builds know only the uncompressed
form. Since we cannot tell which build is installed, tabber reads whichever is
there — preferring `nprofile.gz`, since that is what the game reads first — and
never *creates* a gzipped save:

| Save on disk | Save going in | Written as |
| --- | --- | --- |
| `nprofile.gz` | gzipped | `nprofile.gz`, as it is |
| `nprofile.gz` | uncompressed | `nprofile` |
| `nprofile` (or none) | gzipped | `nprofile`, unwrapped first |
| `nprofile` (or none) | uncompressed | `nprofile` |

A save is only left gzipped when it already was *and* the game has shown it
reads that form by having one. `--force-compress` changes one line of that
table: with it, an uncompressed save going into a game that keeps a gzipped one
is compressed on the way in rather than left for the game to fall back to. It
costs a second or so on a full savefile and saves the game a fallback it
handles anyway, so it is not the default. What tabber compresses it unpacks
again and compares before writing, so a save the game could not read is caught
here rather than at its next launch. An old build is therefore never handed a file it
cannot open, and a new build simply falls back to the uncompressed one and
re-compresses it on its next save. Whichever form is written, the other is
deleted: leaving it behind would have the game read the file we did not mean it
to.

### What makes it safe

- Everything — reading the archive, unwrapping it, building the new archive —
  happens in memory first. A failure at any point means nothing was written.
- The archive of the live save is written, then **read back and checked against
  its own CRC-32**, before the savefile it holds is overwritten.
- The savefile itself goes to a temporary file, is read back and compared, and
  only then renamed into place.
- An empty savefile stops the whole thing: something is wrong, and it is not
  for tabber to decide what.
- If a later step of an install or uninstall fails, the swap is undone from the
  archive that was just made.

## Steam Cloud

N++ syncs its savefile through Steam Cloud, and **the cloud copy wins**: swap
the local save without touching it and Steam puts the old one back before the
game ever reads it. So installing and uninstalling deal with it too.

The copies live in `<Steam folder>/userdata/<Steam32ID>/230270/remote/`, one
folder per Steam account on the machine. There is no way to tell which account
is playing, so every account that has an N++ folder is treated the same way and
reported by its ID. An account with no cloud save is reported and left alone —
there is no reason to put one there.

`--cloud-mode` picks what happens to a cloud save that *is* there:

| Mode | Effect |
| --- | --- |
| `replace` (default) | overwrite it with the save that just went into the game |
| `remove` | delete it, and let the game upload a fresh one |
| `keep` | touch nothing; only report what is there |

Two rules are not up to the mode:

- **An uncompressed cloud save is always deleted, never replaced.** Steam Cloud
  was switched off in 2023 over corruption problems and only came back with
  TEN++, which gzips. Anything uncompressed up there predates that, is in a
  different format that was shrunk to fit the quota, and is of no use to any
  current build. Nothing is put in its place. (`keep` still keeps it.)
- **A replacement is always gzipped**, whatever form the local save is in: the
  quota is 3 MB and 8 files, and the uncompressed savefile is 70 MB. This is
  the one place where the fallback the game offers is no help, so the save is
  compressed here even without `--force-compress`, and unpacked again to check
  it before it is written.

Nothing is archived here: these are copies of the savefile, and the savefile
itself is archived in the personal folder as described above. If the sweep
cannot reach an account's folder, that account is reported as a warning and the
install still stands.

## The palettes

A palette is a folder of TGA colour swatches, and the game reads them from
`<installation dir>/NPP/<config.palettes_dir>/<palette name>/`: every subfolder
there is one palette, named by the folder itself. On Windows the game ships
without that folder at all, so it is created when the first palette goes in.

A tab's own palettes sit in the same place inside its download, and the digest
names them in `disk.palettes`. Only those are installed: a folder in the tab's
archive that the digest does not list is left where it is, and a palette the
digest promises but the archive lacks is caught at fetch time rather than
halfway through an install.

Copying them across is the easy part. What takes the work is deciding whether a
palette can go in at all, and each one gets its own line in the install log
saying what became of it.

### Names

Palette names must be unique, and are compared case-insensitively, so a name
cannot be freed by respelling it. When the name a tab wants is already taken,
`--on-palette-collision` decides:

| Mode | What happens |
| --- | --- |
| `skip` (default) | the palette that is there stays, and the tab's is not installed |
| `replace` | the tab's palette overwrites it |
| `suffix` | the tab's goes in beside it as `<name> 2`, `<name> 3`, and so on |

The default is `skip` because it is the only one that cannot cost the user
anything: `replace` overwrites a palette that may well be their own work, and
is not undone by a later uninstall — the folder it took is deleted along with
the tab's other palettes, since by then the original is already gone.

Names can also collide with a palette that is nowhere in the folder. **123 of
the game's palettes are baked into the library**, take precedence over anything
on disk, and cannot be found by looking at the game's files, so tabber carries
the list. A folder named after one of them would simply be ignored by the game,
which is why it is not copied in even under `replace`: the palette would simply
never appear. Under `suffix` it goes in anyway, since the suffixed name is no
longer a baked one.

The four palettes [cut before release][cut] are the exception: they were baked
with ` CUT` appended, so `line` is a free name while `line CUT` is not. Bringing
them back as custom palettes works, and tabber treats them accordingly.

[cut]: https://github.com/edelkas/nppdocs/blob/master/docs/palettes.md#cut-palettes

### The limit

The game indexes palettes with a single byte and stops reading at **256**, the
123 baked ones included. Past that line the remaining palettes are still listed
in-game but never parsed, and selecting one does nothing. That leaves room for
133 in the folder, whoever they belong to.

A folder named after a baked palette does not count against that: the game
drops it before it is ever parsed, so it occupies no slot. Keeping copies of
the baked palettes in the folder is common — the Linux build ships them, and
they are handy to edit from — and counting them would tally those palettes
twice and turn a tab away for a shortage of room that is not real. The total is
therefore the 123 baked ones plus only the folders the game does not already
skip.

Rather than deleting somebody else's palette to make room, a palette that does
not fit is simply not copied, and the log says why. The check comes after the
name check on purpose: a palette that replaces an existing one, or that is
skipped because its name is taken, takes no new slot and so can never be the one
that does not fit.

### Undoing

An install that fails later — the library patch, the savefile swap — takes the
palettes back out with everything else. Under `replace` the palette being
overwritten is renamed aside first and only deleted once the install is past the
point of undo, so a rollback puts the original back rather than leaving a hole.

## The in-game texts

A custom tab is not the official game, and a couple of the strings the game
shows stop being true once one is installed: the friend highscore panel is the
tab's speedrun boards, and the title screen still says "Press Any Key" where it
could name the tab. tabber rewrites those in
`<installation dir>/NPP/loc.txt`, the table holding every string the game
shows.

That file is one string per line, one language per field, fields separated by
vertical bars. The first field of a line is the `LOC_ID` naming the string, and
the first line is the header, whose fields name the languages in the order
every other line lists them:

```
LOC_ID|english|french|italian|german|spanish|...
HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT|Friends|Amis|Amici|Freunde|Amigos|...
```

Three strings are replaced today:

| `LOC_ID` | Becomes |
| --- | --- |
| `HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_LONG` | `Speedrun Boards` |
| `HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT` | `Speedrun` |
| `PLAYER_PRESS_ANY` | the tab's name, capitalised |

Adding a fourth is a row in `loc_replacements` in `src/loc.c` and nothing else:
recording the originals, restoring them and reporting what happened all work off
that table rather than off the entries in it. A `LOC_ID` the game's table does
not carry is reported and skipped, so a row that a later version of the game
drops costs nothing.

### Which languages

`--languages` takes `all` (the default), `none`, or a comma-separated list such
as `--languages spanish,english`. Names are trimmed and matched
case-insensitively, and one the table does not carry is a warning rather than a
refusal: the languages it does carry are still written. With `none` the file is
not even opened, so a game whose table has gone missing can still take a tab.

The replacements are English whichever column they go into. Custom texts in
eleven languages would be a lot of work for three labels, and an English label
in a Spanish menu at least says what the panel really shows, which the original
no longer does — which is why the default is every language rather than English
alone.

### Putting them back

Nothing about the game's own texts is hardcoded, with the one exception below.
What a replacement overwrites is copied into `config.json` under `strings`
before it is written:

```json
{
  "strings": {
    "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT": {
      "english": "Friends",
      "spanish": "Amigos"
    }
  }
}
```

Uninstalling puts each of those back and empties the record, so `strings` always
describes the replacements that are live right now. A column that already read
what tabber was about to write is not recorded: its contents are the
replacement, not an original, and recording them would make the uninstall
restore the change instead of undoing it.

The exception is the installer that came before tabber. It replaced the same
three strings in English and recorded nothing anywhere, so undoing its work
needs the originals — `Friends Highscores`, `Friends` and `Press Any Key` — and
those three are carried in the source. An uninstall restores them whenever the
record does not cover English and the file does not already read as the game
shipped it, which is what lets tabber take over from an installation it did not
make itself.

The whole table is rewritten in one pass from a copy held in memory, staged
beside the file and swapped in, so an interruption cannot leave the game short
of nine hundred strings, and an install that fails at a later step puts the
original bytes straight back.

## The controls

Some custom tabs — Duality most of all — ship co-op maps built for
one player driving both ninjas at once. That needs two players bound to the same
keys, and the game will not do it: bind a key that is already taken in its
options screen and it is unbound from whoever had it. Written straight into the
file it works perfectly well, and the file is

```
<personal dir>/keys.vars
```

next to the savefile. It is a list of `name = value;` settings, one per line,
with `//` comments and blank lines between the blocks, and a value is either
`KEYBIND("<key>")` or `-1`, which means the action has no key at all. tabber
treats those values as opaque: they are copied and compared as they stand, never
interpreted.

```
tabber bind 1,2      player 2 gets player 1's controls
tabber bind 1,2,3    ...and so does player 3
tabber unbind        put back whatever bind changed
tabber unbind 2,3    clear those players' controls instead
```

The first player in the list is the one whose keys are copied; the rest are
copied to, and are the only ones changed. Whitespace is stripped, a player named
twice counts once, and only numbers from 1 to 4 are players.

Only three settings per player are touched — `left`, `right` and `jump`. Those
are what drives a ninja; the other nine work the menus, and sharing those
between players would do nothing but take options away.

### Putting them back

Every value `bind` overwrites is recorded in `config.json` under `keybindings`
before it is written, so `unbind` puts back exactly what was there:

```json
{
  "keybindings": {
    "input_p2_left_key": "-1",
    "input_p3_jump_key": "KEYBIND(\"F3\")"
  }
}
```

A binding that already read what `bind` was about to write is not recorded — its
value is the new one, not an original — and neither is one an earlier `bind`
already recorded: that first record is what the player really had, while what is
in the file now is only what tabber left there. `-1` is recorded like any other
value, since an action with no key is a state worth restoring to.

`unbind` empties the record afterwards, so `keybindings` always describes the
changes that are live right now. With a full record it restores the file to
precisely what it was, whether or not a player list was given.

The list is the fallback for controls tabber did not bind — an older installer's
work, for instance, which recorded nothing. Naming a player whose bindings are
not in the record clears their three settings to `-1`: the original is
unknowable, and `-1` at least undoes the sharing instead of leaving two players
on one key. One recorded binding is enough to spare a player from that, since it
can only be there because tabber put it there; the two that are not recorded
were never changed, and are left alone.

The file is rewritten in one pass from a copy held in memory, staged beside
itself and swapped in. Only the values move: the spacing, the comments, the
blank lines and the line endings all come back exactly as they were.

### At the tab's own request

A tab that needs this says so in the digest, under its `disk` object:

```json
"disk": { "level_files": ["C.txt"], "bind": [1, 2] }
```

`bind` is the same list the command takes, so Duality's `[1, 2]` means exactly
`tabber bind 1,2`: player 2 answers to player 1's keys while the tab is
installed. Installing does it, recording the originals in `keybindings` as
usual; uninstalling puts them back and empties the record. Most tabs carry no
`bind` at all, and nothing about the controls happens for them.

The list is read the same way as the command's, and the digest gets no more
benefit of the doubt than a user does: anything that is not a whole number from
1 to 4, or a list naming a single player, is refused rather than guessed at.

This one step is not allowed to fail an install. A player who has never run the
game has no `keys.vars` at all, and refusing to install over that would be
absurd: the tab plays either way, only single-handed co-op does not. So a
bindings file that cannot be read is reported as a warning, the install carries
on without it, and `bind 1,2` puts it right once the game has written the file.
An install that does bind and then fails at a later step takes the change back
out with everything else.

## Uninstalling

`tabber uninstall CODE` puts the game back: it deletes the tab's files and
renames each `OG` original back over them, one atomic rename per file. The list
of files comes from the digest (`disk.level_files` and `disk.challenge_files`,
again limited to what `config` says the game reads), not from the tab store, so
an uninstall still works after `remove` has deleted the download.

Both checks run before anything moves, and either aborts the whole thing:

| Check | Abort reason |
| --- | --- |
| The library check passes | the game is not in a state we recognise |
| Every file the tab installed is in the game folder | the tab does not look installed |
| The library carries a 3rd party URI | no tab appears to be installed |
| That URI names this very tab | the game is serving a different tab |

The library is restored first and the level files second, undoing the install in
reverse, and a failure in the second step re-applies the patch.

The game's texts are put back next, from the record in `config.json` plus the
three English originals tabber carries for installs it did not make (see [the
in-game texts](#the-in-game-texts)). Like the palettes below it, a table that
will not rewrite is a warning rather than a reason to undo the uninstall, and
the record is only emptied once the originals really are back.

The controls follow, from the `keybindings` record and on the same terms (see
[the controls](#the-controls)). An empty record means nothing was ever bound,
and the bindings file is then not even opened.

The tab's palettes are removed last, unless `--keep-palettes` says to leave
them. Which ones those are comes from `config.json`, where the install recorded
the folders it actually created — never from the digest's list — so a palette
that was skipped because the user already had one of that name is not mistaken
for the tab's and deleted. Only an install that predates the record falls back
to the digest. A palette that will not delete is a warning: by then the game is
already back as it was, which is no reason to undo any of it.

The files on disk are the authority, not `config.json`: a state file that has
drifted out of step will not stop a real installation from being undone.
Afterwards `installed` goes back to false and `uninstall_date` is stamped, while
`install_date` is kept, so when the tab was last installed is not lost.

Backups left in the game folder that were not part of this tab are reported as
warnings, since they point at an older install or a tab whose file list changed.

### The older installers

The installers that came before tabber worked differently: they kept no copy of
anything, and put the game back by writing out the original files they shipped
themselves. Both halves of that matter here, because it means an `OG` file
proves nothing in either direction:

- **A tab installed by one of those has no backups.** So a missing `OG` is not
  proof that nothing is installed, and an uninstall may not refuse over it.
- **A tab uninstalled by one of those leaves ours behind.** So an `OG` sitting
  in the folder is not proof that something *is* installed, and an install may
  not refuse over it either.

Installing therefore overwrites a backup that is already there, and says so.
That is safe because of what has been established by then: the library check
has already found the game pointing at the official server, which is what
settles whether a tab is installed — the backups never were. The file beside
the leftover is the game's own, so the backup being rewritten from it is a
correction, not a loss.

Uninstalling restores from the backups where they exist, and falls back for the
rest to the same place the old installers got them: the first mappack in the
digest (`met`) **is** the vanilla game, so its files are the originals. Only
the files the installed tab actually replaced are needed, usually a handful. If
that mappack is not in the local store it is downloaded on the spot, and only
if that fails too — nothing left to restore from — is the uninstall refused.

```
SI.txtOG  is there    ->  renamed back over SI.txt
Scodes.txt has none   ->  written from tabs/met/Levels/Scodes.txt
neither               ->  refused, and nothing is touched
```

Which files came from where is in the report, and a download that had to happen
is named. Everything is worked out before the first byte is written, the
download included, so a failure part way still leaves the game as it was.

Only that direction works. An install made by *tabber* cannot be undone by an
older installer, because those write the 3rd party server's raw address into
the library and look for that same string again to undo it, whereas tabber
writes the host name (see [redirecting the server](#redirecting-the-server)).
The string they search for is not there, so they refuse. Upgrading to tabber is
a one-way step, which is why it keeps the ability to clean up after them and
not the other way round.

## Where tabber keeps its files

Everything the tool owns — `config.json`, the cached `digest.json` and the
`tabs/` store — lives in one folder, the one each system sets aside for a
program's per-user data:

| System | Folder |
| --- | --- |
| Windows | `%LOCALAPPDATA%\Tabber` |
| macOS | `~/Library/Application Support/Tabber` |
| Linux, elsewhere | `$XDG_DATA_HOME/tabber`, or `~/.local/share/tabber` |

Those are the places meant for exactly this: writable without asking, kept
across sessions, and not swept up by anything. The point of using them is that
the executable becomes disposable — it can be moved, renamed, replaced by an
upgrade or thrown away and downloaded again, and the tab store, the record of
what is installed and any settings stay where they are. It also keeps a build
tree clean, since a binary run out of `build/` no longer scatters state beside
itself.

It is one folder per user, not per binary: the 32-bit and 64-bit builds share
it, as do a copy in `build/` and one on the desktop. That is what makes the
record trustworthy — only one tab can be installed at a time, and that fact is
recorded in one place rather than in whichever folder the binary happened to be
run from.

- `TABBER_HOME` overrides it, when it names a directory that exists. Portable
  setups and the test suite use this; nothing else needs it.
- The folder is created on first use. If it cannot be — an unwritable or
  undiscoverable data directory — the tool falls back to the executable's own
  directory rather than failing, which is where everything used to live.
- **Upgrading from 0.1.1 or earlier moves the old files over**, once, on the
  first run. Anything already in the new folder wins and is never written over,
  and an entry that cannot move is left where it is. The move is reported on
  stderr. This matters mostly for `config.json`: which tab is installed is the
  one thing an uninstall cannot work out on its own.

## State

`config.json`, in [the tool's own folder](#where-tabber-keeps-its-files), holds
the tool's configuration and what it has done so far. Its `tabs` array carries one entry per tab the tool has
touched:

```json
{
  "tabs": [
    {
      "id": 21,
      "code": "lit",
      "downloaded": true,
      "installed": false,
      "download_date": "2026-08-17T19:15:06Z",
      "install_date": null,
      "uninstall_date": null,
      "remove_date": null,
      "palettes": []
    }
  ],
  "strings": {},
  "keybindings": {}
}
```

Dates are ISO 8601 UTC, the format the digest itself uses, and are `null` until
the corresponding action happens. A successful `fetch` creates the entry if
needed and sets `downloaded` and `download_date`; installing stamps
`install_date` and lists under `palettes` the folders it created in the game's
palettes folder, which is what a later uninstall goes by. An empty list means
the install put none in, and is not the same as the key being absent, which
means the install predates the record.

`strings`, beside `tabs` rather than inside it, holds the in-game texts an
install replaced, one entry per `LOC_ID` and one member per language, each
carrying the text that was there before. Only one tab can be installed at a
time, so one record is enough; uninstalling puts the originals back and leaves
it empty.

`keybindings` does the same for the player controls the `bind` command changes,
one member per setting it overwrote (see [the controls](#the-controls)), and
`unbind` empties it the same way.

`update` remembers the last look for a newer tabber, so ordinary commands are
not held up by one more than once a day (see
[updating tabber](#updating-tabber)):

```json
"update": {
  "check": true,
  "last_check": "2026-08-24T18:00:00Z",
  "latest": "0.3.0",
  "declined": null
}
```

`check` set to false turns the automatic look off altogether; `declined` is the
version the user said no to, which is why the offer comes once per release
rather than once a day.

The file is edited in place rather than regenerated: keys this version does not
know about, and fields it does not own, survive a rewrite. If it is missing it
is created; if it is unreadable the tool says so, carries on, and leaves it
alone rather than overwriting whatever is in there.

## Updating tabber

Releases live on [GitHub Releases](https://github.com/edelkas/tabber/releases).
Each one carries a plain executable per platform — tabber is a single file,
with the fresh savefile built into it — and a `manifest.json` describing them:

```json
{
  "version": "0.3.0",
  "date": "2026-09-01T12:00:00Z",
  "notes": "What changed, in a line or three.",
  "page": "https://github.com/edelkas/tabber/releases/tag/v0.3.0",
  "builds": {
    "windows-x64": { "url": "https://github.com/edelkas/tabber/releases/download/v0.3.0/tabber-windows-x64.exe",
                     "size": 371712, "md5": "..." }
  }
}
```

The manifest is read from `.../releases/latest/download/manifest.json`, a URL
GitHub always points at the newest release, so a check is one small download
and no API call — nothing to rate-limit and nothing whose schema is not ours.
The per-build URLs are pinned to their own tag instead, so a release published
half way through a download cannot hand out a mismatched pair.

The build is chosen by `<os>-<arch>` (`windows-x64`, `windows-x86`,
`linux-x64`, `macos-arm64`), falling back to the bare system name for a release
that ships one build per platform. A release with no build we can run is still
reported — knowing there is a newer version is the point — with a pointer to
the page.

Since Windows ships in two architectures, `"windows"` on its own is no longer
a key to reach for there: a 32-bit tabber would match it and pull down the
64-bit binary. That fails safely rather than quietly — the downloaded binary is
run and asked its version before the old one is let go, and one that cannot run
at all is put straight back — but it is a wasted download and an alarming
report, so name both architectures.

Versions are compared numerically, field by field, so `0.10.0` is above
`0.9.0`; a leading `v` is ignored, a missing field counts as zero, and
`1.0.0-rc1` sorts below `1.0.0`.

### Replacing a running program

The whole thing rests on one fact: a running executable cannot be written to or
deleted, but it **can be renamed** — on Windows as much as on Unix, where the
process simply keeps running from the file under its new name. So there is no
helper process and no batch file:

1. the new binary is written beside the old one, as `tabber.exe.new`
2. `tabber.exe` is renamed to `tabber.exe.old`
3. `tabber.exe.new` takes the name `tabber.exe`
4. the displaced binary is deleted on the next run, once nothing has it open

Staging beside the running binary is deliberate: the swap is a rename, and a
rename is only atomic — on Windows, only possible at all — within one
filesystem. If the executable's folder is not writable (tabber in
`Program Files` without administrator rights, say) that is reported before
anything is downloaded.

Nothing is swapped until the download matches both promises the manifest makes,
its **size** and its **MD5**. And nothing is kept until the new binary proves
it runs: it is started with `--self-check <version>` and has to exit cleanly
from the very version the manifest named. A file that verified perfectly can
still be a build that will not run on this machine, and that is the only way to
find out. If it fails, the old binary goes straight back.

### When it looks

`tabber upgrade` checks and installs in one go, without asking: it was asked
for by name.

Every other command checks too, but quietly and **at most once every 24 hours**,
with the result kept in `config.json`. On a terminal a newer version is offered
(`Update now? [Y/n]`), and saying yes updates and then re-runs the command that
was typed, on the new version. Anywhere else — a pipe, a script, `--bare` — it
prints one line to stderr naming `tabber upgrade` and gets out of the way.
Saying no is remembered for that version, so the question comes once per
release rather than once per day, and a check that fails is silent: GitHub
being unreachable is not this command's problem. `--offline` and
`--no-update-check` skip it, and `"update": { "check": false }` in
`config.json` turns it off for good.

### Cutting a release

```
python tools/make_manifest.py 0.3.0 --notes "What changed." \
    --build all --out dist/manifest.json
```

Bump `TABBER_VERSION` in `src/version.h`, build each platform, run that, then
tag `v0.3.0` and attach every binary **and** `manifest.json` to the release.
The first release is the odd one out, since nothing can update to it from
nothing; publish it, then test the path from it to the next.

`--build all` takes whatever the build scripts have left beside the manifest
and works the keys out of the names; anything else in there is ignored, and a
`KEY=PATH` given as well overrides what was found. **Generate the manifest
last.** Every build restages `dist/`, and a binary rebuilt afterwards is a
different file — same size, different MD5 — which the manifest would then
describe wrongly.

Two front-ends now ship per platform, and only one of them can hold the key the
tool asks for. `upgrade` looks up exactly `<os>-<arch>` (`src/update.h`), so
that stays the CLI's, which is also what keeps releases before this one
upgradeable. The GUI is keyed `<os>-<arch>-gui`, off to one side of that
lookup: nothing reads it today, and nothing can mistake it for the CLI.

The asset names carry no version on purpose. An upgrade writes over the file
that is already on disk and keeps whatever it is called, so a version in the
name would be wrong from the first upgrade on — and few people rename a binary
after updating it. The version lives in the manifest and on the release page,
which is where the tool reads it from anyway; `make_manifest.py` says so if an
asset is named after the release regardless.

`UPDATE_MANIFEST_URL` and `TABBER_VERSION` can both be overridden at build time
(`-DTABBER_VERSION='"0.9.0"'`), which is how an update is rehearsed against a
staging release before any of it is public.

### What this does and does not protect

HTTPS and the MD5 cover a corrupted download and a network that tampers with
one. They do **not** cover a compromised GitHub account: whoever could replace
the binary could rewrite the manifest that describes it. Real protection there
means signing the manifest with a key that never touches CI and checking the
signature in the tool — worth doing, and not a reason to hold up the first
release.

## Status

- [x] Locate the installation and personal directories
- [x] Fetch, cache and list the custom tab digest
- [x] Download, verify and unpack custom tab ZIPs
- [x] Track configuration and per-tab state in `config.json`
- [x] Remove a downloaded tab
- [x] Swap level and challenge files (install / uninstall)
- [x] Patch the main library to redirect server queries
- [x] Check that the 3rd party server is up
- [x] Back up and swap the savefile
- [x] Handle Steam Cloud's copy of the savefile
- [x] Install custom palettes
- [x] Replace in-game texts
- [x] Bind several players' controls together (`bind` / `unbind`, or at the tab's request)
- [x] Update tabber itself from GitHub Releases (`upgrade`)
- [ ] Optional extras
- [ ] Dear ImGui front-end (started: the catalogue, and download / install / uninstall)
