#!/usr/bin/env python3
"""Says whether dist/ still holds exactly what its manifest describes.

    python tools/check_manifest.py [--dist dist]

Every build the manifest names has to be sitting there under the name its URL
ends in, the same size and the same MD5; and nothing else that looks like a
release binary may be there beside them. Prints True or False and nothing else.
Why a False is a False goes to stderr, and the exit status is 0 for True and 1
for False, so this can gate the release itself:

    python tools/check_manifest.py && gh release create v0.5.0 dist/*

What it is for is the order the manifest has to be made in. Every build
restages dist/, so a binary rebuilt after the manifest was written is a
different file -- same size, most likely, and a different MD5 -- which the
manifest then describes wrongly, and which the tool would refuse to install
once it was published. This is that check, made before anything is uploaded
rather than by the first person to try updating.

Anything in dist/ that is not named like a release binary is ignored, the same
way make_manifest.py ignores it: the manifest and whatever else is kept there
are nobody's business here.
"""
import argparse
import hashlib
import json
import os
import re
import sys

DIST_DIR = "dist"
MANIFEST_NAME = "manifest.json"
READ_CHUNK = 1 << 20

# The names the build scripts leave behind, and the only files in dist/ this
# has anything to say about. The same pattern make_manifest.py discovers with.
BUILD_NAME = re.compile(
    r"^tabber-(cli|gui)-(windows|linux|macos)-(x64|x86|arm64)(\.exe)?$")


def md5_of(path):
    """Its MD5, read a piece at a time: these are binaries, not strings."""
    digest = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(READ_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def disagreements(dist):
    """Every way the directory and its manifest fail to describe each other."""
    path = os.path.join(dist, MANIFEST_NAME)
    try:
        with open(path, "r") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as err:
        return ["%s: %s" % (path, err)]

    builds = manifest.get("builds") or {}
    if not builds:
        return ["%s names no builds" % path]

    wrong = []
    named = set()
    for key in sorted(builds):
        entry = builds[key] or {}
        # The URL is where the name comes from, that being the name the binary
        # is published under and the one this file has to be sitting there as.
        name = os.path.basename(entry.get("url", ""))
        if not name:
            wrong.append("%s: no url to take a file name from" % key)
            continue
        named.add(name)

        binary = os.path.join(dist, name)
        if not os.path.isfile(binary):
            wrong.append("%s: %s is not in %s" % (key, name, dist))
            continue
        size = os.path.getsize(binary)
        if size != entry.get("size"):
            wrong.append("%s: %s is %d bytes, the manifest says %s"
                         % (key, name, size, entry.get("size")))
        digest = md5_of(binary)
        if digest != entry.get("md5"):
            wrong.append("%s: %s is %s, the manifest says %s"
                         % (key, name, digest, entry.get("md5")))

    # ...and the other way round: a build sitting there unnamed would go up
    # with the release as something nothing can install.
    for name in sorted(os.listdir(dist)):
        if BUILD_NAME.match(name) and name not in named:
            wrong.append("%s is in %s and the manifest does not name it"
                         % (name, dist))
    return wrong


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dist", default=DIST_DIR,
                    help="the directory holding the manifest and the builds")
    args = ap.parse_args()

    if not os.path.isdir(args.dist):
        wrong = ["no such directory: %s" % args.dist]
    else:
        wrong = disagreements(args.dist)

    for line in wrong:
        sys.stderr.write("%s\n" % line)
    print("False" if wrong else "True")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
