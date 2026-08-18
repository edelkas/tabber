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
  remove CODE      Delete the downloaded files of the custom tab CODE
  install CODE     Install the custom tab CODE into the game (fetching it if needed)
  uninstall CODE   Restore the game's original files, undoing an install
  check            Verify the game library matches the recorded state
  server           Check that the 3rd party server is up

  -b, --bare       Machine-readable output: paths or tab-separated fields only
  -v, --verbose    Print extra detail
  -o, --offline    Skip the automatic digest refresh, use the cached copy
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
make test / test-online / test-full     Linux and macOS
```

The suite lives in `test/` and links the same objects as the tool, so it
exercises the real code rather than a copy of it. It runs in three tiers:

| Tier | Needs | Covers |
| --- | --- | --- |
| offline (default) | nothing | strings, paths, JSON, KeyValues, MD5, ZIP/DEFLATE, state file, server resolution, install/uninstall/patching against a stand-in game |
| `--online` | the network | downloading the live digest, fetching a tab and verifying it |
| `--full` | the network | downloading and verifying **every** published tab |

Nothing in the suite touches a real N++ installation or the tool's own state.
Each test builds a scratch world: a temporary tool root (via `TABBER_HOME`) and
a stand-in game (via `TABBER_GAME_DIR`) with the game's level files and a
library carrying the official URI. The scratch area is deleted when the run
ends.

Some things are worth knowing about how the tests check what they check:

- **Decompression** is judged by CRC-32, computed over the bytes the
  decompressor produced and compared with what the archive recorded. In the
  full tier that is an independent verdict on every file of every tab.
- **Corruption** is swept byte by byte through the compressed data of the
  embedded fixture: every single-byte flip must be caught.
- **Refusals** assert not just the error but that nothing moved: files, backups
  and the library are all re-checked afterwards.
- **The server** the offline tier probes is `127.0.0.1:9`, a port nothing
  listens on, so the health check is exercised for real — a refused connection
  — without leaving the machine. The live server is only asked in `--online`.
- The embedded ZIP fixture (`test/fixture_zip.c`) deliberately mixes a stored
  directory entry, a deflated file, and a deflate *stored block*, so all three
  paths through the decompressor are used without any network.

When a feature lands, its tests land with it in the same tier as the code they
cover.

## Layout

| File | Purpose |
| --- | --- |
| `src/main.c`     | CLI entry point and argument parsing |
| `src/paths.c/.h` | Discovery of N++'s installation and personal directories; all Steam/N++ layout constants |
| `src/digest.c/.h`| The custom tab catalogue: fetch, cache, parse, look up |
| `src/tabs.c/.h`  | Downloading, verifying and unpacking a custom tab |
| `src/install.c/.h` | Installing a custom tab into the game |
| `src/patch.c/.h` | Redirecting the game's server queries, and the library health check |
| `src/server.c/.h` | Which 3rd party server to point the game at |
| `src/config.c/.h`| The tool's own configuration and state (`config.json`) |
| `src/kv.c/.h`    | Parser for Valve's KeyValues format (`.vdf`, `.acf`) |
| `src/json.c/.h`  | Minimal JSON parser (RFC 8259) |
| `src/zip.c/.h`   | In-memory ZIP reader with CRC-32 verification |
| `src/inflate.c/.h` | DEFLATE decompressor (RFC 1951) |
| `src/md5.c/.h`   | MD5 digest (RFC 1321), for download integrity |
| `src/net.c/.h`   | HTTPS client: WinHTTP on Windows, libcurl elsewhere |
| `src/platform.c/.h` | OS abstraction: filesystem, environment, Windows registry, UTF-8 paths |
| `src/util.c/.h`  | Allocation, string, buffer and error helpers |
| `src/version.h`  | Program name and version |
| `test/`          | The test suite (see above) |

## Environment

| Variable | Effect |
| --- | --- |
| `TABBER_HOME` | Use this directory as the tool's root instead of the executable's, for `config.json`, the cached digest and `tabs/` |
| `TABBER_GAME_DIR` | Use this N++ installation directory instead of asking Steam |

Both are meant for tests and for unusual setups (a portable copy, a game Steam
does not know about); neither is needed in normal use.

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
`<installation dir>/NPP/<config.levels_dir>/`. Each original is kept next to it
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
| No `OG` backup exists yet | a previous install was never undone |
| The game folder accepts writes | permissions, or the game is running |
| The library still carries the official URI, once | it looks patched already |
| The new URI fits in the original's length | the server address is too long |

That last one matters: without it, installing over an existing backup would
overwrite a pristine original with a modded file. If a rename or write still
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

## The library check

`tabber check` reads the library, works out which URI it carries, and compares
that with what `config.json` says is installed:

| Recorded | Library | Verdict |
| --- | --- | --- |
| nothing installed | official URI | healthy |
| tab X installed | 3rd party URI ending in `/X` | healthy |
| nothing installed | 3rd party URI | a tab is installed that we do not know about |
| tab X installed | official URI | no tab is really installed |
| tab X installed | URI ending in `/Y` | the game is serving a different tab |

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
| Every one of them has its `OG` backup | restoring would leave the game short of files |
| The library carries a 3rd party URI | no tab appears to be installed |
| That URI names this very tab | the game is serving a different tab |

The library is restored first and the level files second, undoing the install in
reverse, and a failure in the second step re-applies the patch.

The files on disk are the authority, not `config.json`: a state file that has
drifted out of step will not stop a real installation from being undone.
Afterwards `installed` goes back to false and `uninstall_date` is stamped, while
`install_date` is kept, so when the tab was last installed is not lost.

Backups left in the game folder that were not part of this tab are reported as
warnings, since they point at an older install or a tab whose file list changed.

## State

`config.json`, next to the executable, holds the tool's configuration and what
it has done so far. Its `tabs` array carries one entry per tab the tool has
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
      "remove_date": null
    }
  ]
}
```

Dates are ISO 8601 UTC, the format the digest itself uses, and are `null` until
the corresponding action happens. A successful `fetch` creates the entry if
needed and sets `downloaded` and `download_date`; the install fields are wired
up but left untouched until installing exists.

The file is edited in place rather than regenerated: keys this version does not
know about, and fields it does not own, survive a rewrite. If it is missing it
is created; if it is unreadable the tool says so, carries on, and leaves it
alone rather than overwriting whatever is in there.

## Status

- [x] Locate the installation and personal directories
- [x] Fetch, cache and list the custom tab digest
- [x] Download, verify and unpack custom tab ZIPs
- [x] Track configuration and per-tab state in `config.json`
- [x] Remove a downloaded tab
- [x] Swap level and challenge files (install / uninstall)
- [x] Patch the main library to redirect server queries
- [x] Check that the 3rd party server is up
- [ ] Install custom palettes
- [ ] Swap the savefile
- [ ] Replace in-game texts
- [ ] Optional extras (controls, …)
- [ ] DearImGui front-end
