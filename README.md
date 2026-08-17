# tabber

A cross-platform CLI tool (C99) to install custom tabs (mappacks) for the game
**N++**. Work in progress; a DearImGui front-end is planned once the command
line side is complete.

## Building

| Platform | Command | Notes |
| --- | --- | --- |
| Windows (MSVC) | `build.bat` | Imports the MSVC environment automatically via `vswhere` |
| Linux / macOS  | `make`      | Any C99 compiler |
| Windows (MinGW)| `make`      | Links `advapi32`, `ole32`, `shell32`, `uuid` |

The binary lands in `build/tabber[.exe]`.

## Usage

```
tabber [options] [paths]

  paths            Locate N++'s installation and personal directories (default)
  -b, --bare       Print the paths only, one per line (installation first)
  -v, --verbose    Also print the Steam folder and the Steam library used
  -h, --help       Show help
  -V, --version    Show version
```

Exit status is `0` on success, `1` when a directory could not be found (the
reason goes to stderr) and `3` on a usage error.

## Layout

| File | Purpose |
| --- | --- |
| `src/main.c`     | CLI entry point and argument parsing |
| `src/paths.c/.h` | Discovery of N++'s installation and personal directories; all Steam/N++ layout constants |
| `src/kv.c/.h`    | Parser for Valve's KeyValues format (`.vdf`, `.acf`) |
| `src/platform.c/.h` | OS abstraction: filesystem, environment, Windows registry, UTF-8 paths |
| `src/util.c/.h`  | Allocation, string and error helpers |

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

## Status

- [x] Locate the installation and personal directories
- [ ] Swap level and challenge files
- [ ] Patch the main library to redirect server queries
- [ ] Install custom palettes
- [ ] Swap the savefile
- [ ] Replace in-game texts
- [ ] Optional extras (controls, …)
- [ ] DearImGui front-end
