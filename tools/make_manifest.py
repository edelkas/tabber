#!/usr/bin/env python3
"""Writes the manifest that tabber's `upgrade` command reads.

Build one binary per platform, name each after the platform it runs on, then:

    python tools/make_manifest.py 0.3.0 \\
        --notes "What changed, in a line or three." \\
        --build windows-x64=dist/tabber-windows-x64.exe \\
        --build linux-x64=dist/tabber-linux-x64 \\
        --build macos-arm64=dist/tabber-macos-arm64 \\
        --out dist/manifest.json

Then create the GitHub release tagged v0.3.0 and attach every binary along
with manifest.json. The download URLs are pinned to that tag, so a release
published while somebody is mid-download cannot hand them a mixed pair; only
the manifest itself is fetched from .../releases/latest/download/.

The platform keys are what the tool asks for: "<os>-<arch>", with os one of
windows, linux or macos, and arch one of x64, arm64 or x86. A key of just the
os ("windows") also matches, for a release shipping one build per system.

The file names carry no version, and should not: an upgrade keeps the name the
binary already had on disk, so a version in it would be a lie the moment the
first upgrade lands. The version is in the manifest and on the release page.
"""
import argparse
import datetime
import hashlib
import json
import os
import sys

MANIFEST_NAME = "manifest.json"
ASSET_URL = "https://github.com/{repo}/releases/download/v{version}/{name}"
PAGE_URL = "https://github.com/{repo}/releases/tag/v{version}"


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
                    help="a built binary and the platform it is for; repeatable")
    ap.add_argument("--notes", default="", help="a line or three on what changed")
    ap.add_argument("--repo", default="edelkas/tabber", help="owner/name on GitHub")
    ap.add_argument("--date", default=None, help="ISO 8601 UTC; defaults to now")
    ap.add_argument("--out", default=MANIFEST_NAME, help="where to write it")
    args = ap.parse_args()

    builds = {}
    for item in args.build:
        if "=" not in item:
            sys.exit("--build wants KEY=PATH, got '%s'" % item)
        key, path = item.split("=", 1)
        if not os.path.isfile(path):
            sys.exit("no such file: %s" % path)
        builds[key] = build_entry(args.repo, args.version, path)

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
