# tabber

A cross-platform CLI tool (C99) to install custom tabs (mappacks) for the game
**N++**. Work in progress; a DearImGui front-end is planned once the command
line side is complete.

## Building

| Platform | Command | Notes |
| --- | --- | --- |
| Windows (MSVC) | `build.bat` | Imports the MSVC environment automatically via `vswhere` |
| Linux / macOS  | `make`      | Any C99 compiler; needs libcurl (`libcurl4-openssl-dev` or equivalent) |
| Windows (MinGW)| `make`      | Links `advapi32`, `ole32`, `shell32`, `uuid`, `winhttp` |

The binary lands in `build/tabber[.exe]`. HTTP goes through WinHTTP on Windows
(no dependency) and libcurl elsewhere, both behind the two-function API in
`src/net.h`.

## Usage

```
tabber [options] [command]

  paths            Locate N++'s installation and personal directories (default)
  list             List the custom tabs available in the digest
  update           Download the latest digest of custom tabs
  fetch CODE       Download, verify and unpack the custom tab CODE

  -b, --bare       Machine-readable output: paths or tab-separated fields only
  -v, --verbose    Print extra detail
  -o, --offline    Skip the automatic digest refresh, use the cached copy
  -h, --help       Show help
  -V, --version    Show version
```

Exit status is `0` on success, `1` when a directory could not be found, `2` when
an operation failed (network, corrupt digest) and `3` on a usage error. Errors
and warnings go to stderr, so stdout stays parseable.

## Layout

| File | Purpose |
| --- | --- |
| `src/main.c`     | CLI entry point and argument parsing |
| `src/paths.c/.h` | Discovery of N++'s installation and personal directories; all Steam/N++ layout constants |
| `src/digest.c/.h`| The custom tab catalogue: fetch, cache, parse, look up |
| `src/tabs.c/.h`  | Downloading, verifying and unpacking a custom tab |
| `src/kv.c/.h`    | Parser for Valve's KeyValues format (`.vdf`, `.acf`) |
| `src/json.c/.h`  | Minimal JSON parser (RFC 8259) |
| `src/zip.c/.h`   | In-memory ZIP reader with CRC-32 verification |
| `src/inflate.c/.h` | DEFLATE decompressor (RFC 1951) |
| `src/md5.c/.h`   | MD5 digest (RFC 1321), for download integrity |
| `src/net.c/.h`   | HTTPS client: WinHTTP on Windows, libcurl elsewhere |
| `src/platform.c/.h` | OS abstraction: filesystem, environment, Windows registry, UTF-8 paths |
| `src/util.c/.h`  | Allocation, string, buffer and error helpers |
| `src/version.h`  | Program name and version |

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
serves HTML). It is cached as `digest.json` **next to the executable**.

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
unpacks it into `tabs/<code>/` **next to the executable**, preserving the
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

## Status

- [x] Locate the installation and personal directories
- [x] Fetch, cache and list the custom tab digest
- [x] Download, verify and unpack custom tab ZIPs
- [ ] Swap level and challenge files
- [ ] Patch the main library to redirect server queries
- [ ] Install custom palettes
- [ ] Swap the savefile
- [ ] Replace in-game texts
- [ ] Optional extras (controls, …)
- [ ] DearImGui front-end
