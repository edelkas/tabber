#!/usr/bin/env python3
"""Writes the manifest that tabber's `upgrade` command reads.

The build scripts put every finished executable in dist/ under the name it is
released as, so the usual run is:

    python tools/make_manifest.py 0.3.0 \\
        --notes "What changed, in a line or three." \\
        --build all \\
        --out dist/manifest.json

"all" reads whatever is sitting beside the manifest and works out the keys from
the names, which are "tabber-<cli|gui>-<os>-<arch>" with ".exe" on Windows.
Anything else in the directory is ignored. A binary can still be named
outright, as KEY=PATH, and doing so overrides what was found:

    --build windows-x64=dist/tabber-cli-windows-x64.exe

Then create the GitHub release tagged v0.3.0 and attach every binary along
with manifest.json. The download URLs are pinned to that tag, so a release
published while somebody is mid-download cannot hand them a mixed pair; only
the manifest itself is fetched from .../releases/latest/download/.

The platform keys are what the tool asks for: "<os>-<arch>", with os one of
windows, linux or macos, and arch one of x64, arm64 or x86. A key of just the
os ("windows") also matches, for a release shipping one build per system.

Those keys name the CLI: `upgrade` looks up exactly its own "<os>-<arch>"
(src/update.h), and leaving that key to the CLI is what keeps releases from
before the front-end existed upgradeable. The front-end asks for its own,
"<os>-<arch>-gui", and takes nothing else -- so a release that names only the
CLI leaves every front-end reporting a version it cannot install. Both belong
in every manifest; "--build all" picks up both from the file names.

The file names carry no version, and should not: an upgrade keeps the name the
binary already had on disk, so a version in it would be a lie the moment the
first upgrade lands. The version is in the manifest and on the release page.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys

MANIFEST_NAME = "dist/manifest.json"
ASSET_URL = "https://github.com/{repo}/releases/download/v{version}/{name}"
PAGE_URL = "https://github.com/{repo}/releases/tag/v{version}"

# What the build scripts leave in dist/, and nothing else in there.
BUILD_NAME = re.compile(
    r"^tabber-(cli|gui)-(windows|linux|macos)-(x64|x86|arm64)(\.exe)?$")


def discover(directory):
    """Every released binary in `directory`, keyed as the tool asks for it."""
    found = {}
    if not os.path.isdir(directory):
        sys.exit("no such directory: %s" % directory)
    for name in sorted(os.listdir(directory)):
        matched = BUILD_NAME.match(name)
        if not matched:
            continue
        front_end, os_name, arch = matched.group(1, 2, 3)
        # The bare "<os>-<arch>" is the CLI's, because the CLI is what asks
        # for it; the GUI is kept off to one side of that lookup.
        key = "%s-%s" % (os_name, arch)
        if front_end == "gui":
            key += "-gui"
        found[key] = os.path.join(directory, name)
    return found


def build_entry(repo, version, path):
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        sys.exit("%s is empty" % path)
    # An upgrade writes over the file the user already has, keeping its name,
    # so a version baked into that name goes stale on the first upgrade.
    if version in os.path.basename(path):
        sys.stderr.write("warning: %s has the version in its name, which an "
                         "upgrade will not update\n" % os.path.basename(path))
    return {
        "url": ASSET_URL.format(repo=repo, version=version,
                                name=os.path.basename(path)),
        "size": len(data),
        "md5": hashlib.md5(data).hexdigest(),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", help='release version, without the "v"')
    ap.add_argument("--build", action="append", required=True, metavar="KEY=PATH",
                    help="a built binary and the platform it is for, or 'all' "
                         "to take every one sitting beside the manifest; "
                         "repeatable")
    ap.add_argument("--notes", default="", help="a line or three on what changed")
    ap.add_argument("--repo", default="edelkas/tabber", help="owner/name on GitHub")
    ap.add_argument("--date", default=None, help="ISO 8601 UTC; defaults to now")
    ap.add_argument("--out", default=MANIFEST_NAME, help="where to write it")
    args = ap.parse_args()

    # "all" is read first whatever order it was given in, so that a key named
    # outright on the command line is the one that wins.
    paths = {}
    named = []
    for item in args.build:
        if item.lower() == "all":
            found = discover(os.path.dirname(args.out) or ".")
            if not found:
                sys.exit("no tabber-<cli|gui>-<os>-<arch> binaries in %s" %
                         (os.path.dirname(args.out) or "."))
            paths.update(found)
        elif "=" not in item:
            sys.exit("--build wants KEY=PATH or 'all', got '%s'" % item)
        else:
            named.append(item.split("=", 1))
    for key, path in named:
        paths[key] = path

    builds = {}
    for key in sorted(paths):
        if not os.path.isfile(paths[key]):
            sys.exit("no such file: %s" % paths[key])
        builds[key] = build_entry(args.repo, args.version, paths[key])

    date = args.date or datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    manifest = {
        "version": args.version,
        "date": date,
        "notes": args.notes,
        "page": PAGE_URL.format(repo=args.repo, version=args.version),
        "builds": builds,
    }

    with open(args.out, "w", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print("%s: version %s, %d build(s)" % (args.out, args.version, len(builds)))
    for key, build in sorted(builds.items()):
        print("  %-14s %8d bytes  %s" % (key, build["size"], build["md5"]))


if __name__ == "__main__":
    main()
