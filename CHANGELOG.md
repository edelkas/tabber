# Changelog

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