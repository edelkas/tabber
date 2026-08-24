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
| `src/platform.c/.h` | OS abstraction: filesystem, environment, Windows registry, UTF-8 paths |
| `src/util.c/.h`  | Allocation, string, buffer and error helpers |
| `src/version.h`  | Program name and version |
| `src/resource.h` | Files built into the binary |
| `src/resource_save.c` | The fresh savefile as bytes; generated, do not edit |
| `test/`          | The test suite (see above) |
| `res/`           | The source of the embedded files: the fresh savefile |
| `tools/`         | Release-time scripts: embedding a resource, writing a manifest |

## Environment

| Variable | Effect |
| --- | --- |
| `TABBER_HOME` | Use this directory as the tool's root instead of the executable's, for `config.json`, the cached digest and `tabs/` |
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
    "windows-x64": { "url": "https://github.com/edelkas/tabber/releases/download/v0.3.0/tabber-0.3.0-windows-x64.exe",
                     "size": 371712, "md5": "..." }
  }
}
```

The manifest is read from `.../releases/latest/download/manifest.json`, a URL
GitHub always points at the newest release, so a check is one small download
and no API call — nothing to rate-limit and nothing whose schema is not ours.
The per-build URLs are pinned to their own tag instead, so a release published
half way through a download cannot hand out a mismatched pair.

The build is chosen by `<os>-<arch>` (`windows-x64`, `linux-x64`,
`macos-arm64`), falling back to the bare system name for a release that ships
one build per platform. A release with no build we can run is still reported —
knowing there is a newer version is the point — with a pointer to the page.

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
python tools/make_manifest.py 0.3.0 --notes "What changed."     --build windows-x64=dist/tabber-0.3.0-windows-x64.exe     --build linux-x64=dist/tabber-0.3.0-linux-x64     --out dist/manifest.json
```

Bump `TABBER_VERSION` in `src/version.h`, build each platform, run that, then
tag `v0.3.0` and attach every binary **and** `manifest.json` to the release.
The first release is the odd one out, since nothing can update to it from
nothing; publish it, then test the path from it to the next.

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
- [ ] DearImGui front-end
