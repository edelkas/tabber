# tabber's test suite

Run it with `build.bat test` (Windows) or `make test` (Linux, macOS). Add
`online` / `--online` for the tests that need the network, or `full` /
`--full` to also sweep every published tab.

## Files

| File | Covers |
| --- | --- |
| `test_main.c` | The harness, the scratch-world helpers and the shared fixtures |
| `test_core.c` | Strings, buffers, path handling, canonical paths, JSON, KeyValues, MD5 |
| `test_archive.c` | ZIP reading and writing, DEFLATE both ways, gzip, CRC verification, corruption, unsafe entry names |
| `test_state.c` | `config.json` round trips and lifecycle, server resolution, URI budget, the server health check |
| `test_save.c` | Archiving and swapping the savefile in both its forms, and its copies in Steam Cloud |
| `test_palettes.c` | The baked palette names, collisions, the game's 256 limit, and copying palettes in and out |
| `test_loc.c` | The game's own texts: which languages are written, the state record, restoring, and the ones the old installer left behind |
| `test_keys.c` | Binding players' controls together, restoring them, the list a tab's digest entry asks for, and the fallback for controls nothing recorded |
| `test_update.c` | Comparing versions, reading a release manifest, and replacing the running binary |
| `test_game.c` | Install, uninstall, library patching, the controls a tab asks to have bound, compatibility with the older installers' backups, and both health checks |
| `test_online.c` | The live server, the live digest, fetching a tab, the full catalogue sweep |
| `fixture_zip.c` | A 462-byte ZIP embedded as bytes |

## Adding tests

A test is a function that calls `test_case("what it does")` and then `CHECK`,
`CHECK_STR` or `CHECK_NUM`. Checks report and carry on, so one bad assumption
does not hide the rest of the file. Add the function to the relevant
`suite_*()` at the bottom of its file; add a new file only for a genuinely new
area, and then wire it into `test.h` and `test_main.c`.

Two rules keep the suite trustworthy:

- **Never touch anything real.** Build a scratch world with `test_dir()`,
  `test_use_root()` and `test_fake_game()`. If a test needs the network, it
  belongs in `test_online.c`; a test that needs a server to be *down* can use
  `TEST_DEAD_HOST:TEST_DEAD_PORT`, which never leaves the machine.
- **Assert the absence of damage, not just the error.** When something is
  supposed to be refused, check afterwards that the files, the backups and the
  library are all still as they were. The savefile tests take this furthest:
  every refusal is followed by a byte-for-byte check that the save survived.
- **Prefer the whole file to a field of it.** The text and control tests compare
  the entire file against what it should read, so a rewrite that gets the right
  answer by disturbing something else still fails.
- **Replacing the running binary is done for real.** The update tests point
  `TABBER_EXE` at a copy of the test executable in scratch space and let the
  actual code rename it about, so the self-check that follows a swap really does
  start the binary that was put in place — which is why the harness answers
  `--self-check` the way tabber does. A file of rubbish stands in for a build
  that will not run, and the rollback has to put the working one back.

To confirm the suite still bites, break something on purpose and watch it fail
before fixing it back.
