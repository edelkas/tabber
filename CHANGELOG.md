# Changelog

## [[0.5.0] 2026-08-30](https://github.com/edelkas/tabber/releases/tag/v0.5.0)

New features:
- **Status bar**: Shows the last logged line, and also which tab is currently installed. It can be hidden from the settings.
- **Log viewer**: Allows to read the session log, all of which are also exported to the logfile. This can be disabled from the settings.
- Persisting settings is deferred now: Pressing `Cancel` discards them.

QOL changes:
- Main window auto-sizing: The height is automatically calculated to fit the content, unless manually resized.
- Show hand cursor when hovering buttons.
- Pressing `Esc` dismisses modals.
- Button to open config folder in the settings window.
- Button to open logfile in the log viewer window.
- Log settings which are changed.

Internal changes:
- Fix in Windows' build script (`build.bat`): extra keywords (`all`, `gui`, `clean`) no longer skip the test suite if `test` is specified.

## [[0.4.0] 2026-08-29](https://github.com/edelkas/tabber/releases/tag/v0.4.0)

Main new features:
- The GUI now supports automatic updates, with a button to trigger them on command.
- Rendering has been optimized well over tenfold by addressing some driver issues.
- Tool settings added, with a modal to configure them.

Cosmetic changes:
- Dark / light theme selector.
- [ForkAwesome](https://forkaweso.me/Fork-Awesome/icons/) icon font.
- ASCII art banners with [FIGlet fonts](https://www.figlet.org/).

Internal changes:
- Rebuilding on Windows optimized: Only changed sources are recompiled, other object files are maintained.
  And nothing new to compile also implies no relinking.

## [[0.3.0] 2026-08-26](https://github.com/edelkas/tabber/releases/tag/v0.3.0)

Added graphical interface with world's best toolkit, [Dear ImGui](https://github.com/ocornut/imgui):

- Button to update the local custom tab collection.
- Table listing all custom tabs with buttons to easily download, install and uninstall them.

Improved build scripts and streamlined release process.

## [[0.2.0] 2026-08-25](https://github.com/edelkas/tabber/releases/tag/v0.2.0)

Major restructure of the directory tree. User files (custom tab store, configuration, etc)
are now decoupled from the binary and stored in the standard local data folders:

- **Windows**: `%LocalAppData%\Tabber`, i.e. `C:\Users\<Username>\AppData\Local\Tabber`.
- **Linux**: `$XDG_DATA_HOME/tabber` or `~/.local/share/tabber`.
- **macOS**: `~/Library/Application Support/Tabber`.

Releases now also include a 32-bit Windows build.

## [[0.1.1] 2026-08-25](https://github.com/edelkas/tabber/releases/tag/v0.1.1)

True backwards compatibility with old installers accomplished. Two things were missing:

- Old installers didn't back up level and challenge files, so they aren't required anymore.
- Tabber now patches the credit line as well, which shows up as the level authors.

It should now be possible to install a tab with an old installer and uninstall it with Tabber.
The opposite direction doesn't work though, because Tabber has new functionality that old installers aren't aware of.

## [[0.1.0] 2026-08-24](https://github.com/edelkas/tabber/releases/tag/v0.1.0)

Initial release. CLI for now, GUI planned.

- Fetch custom tabs automatically from [inne++'s repo](https://github.com/edelkas/inne/tree/master/db/mappacks).
- Maintain local tab archive in `tabs` folder.
- Maintain user configuration in `config.json` file.
- Backwards compatible with old installers.
- Automatic updates from [GitHub Releases](https://github.com/edelkas/tabber/releases).
- Cross-platform source code (Win/Linux/macOS). Windows-only release for now though.

For each custom tab the installer contains all the core functionality of the old installers:

- Replace level and challenge files.
- Patch main library (`npp.dll`, `libnpp.so`, `libnpp.dylib`) to redirect server queries.
- Replace `nprofile.gz` savefile (fallback to `nprofile`). Handles Steam Cloud too.
- Bundle custom palettes, when under the 256 limit.
- Customize some in-game texts.
- Customize some keybindings (for tabs like Duality).

Installing and uninstalling is atomic, on failure all changes are reverted. Includes a solid test suite.