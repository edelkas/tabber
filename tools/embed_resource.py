#!/usr/bin/env python3
"""Turns a binary file into a C source file holding it as a byte array.

    python tools/embed_resource.py res/nprofile.zip src/resource_save.c RES_FRESH_SAVE

Run it again whenever the resource changes; the generated file is committed,
so building tabber needs nothing but a C compiler.
"""
import os
import sys

PER_LINE = 12

HEADER = """/*
 * {base} - GENERATED, do not edit by hand.
 *
 * {src} ({size} bytes), embedded so the tool ships as a single executable.
 * Regenerate with:
 *
 *   python tools/embed_resource.py {src} {out} {name}
 */
#include <stddef.h>

#include "resource.h"

const unsigned char {name}[] = {{
"""


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    src, out, name = argv[1:]

    with open(src, "rb") as f:
        data = f.read()

    lines = [HEADER.format(base=os.path.basename(out), out=out.replace("\\", "/"),
                           src=src.replace("\\", "/"), size=len(data), name=name)]
    for i in range(0, len(data), PER_LINE):
        chunk = data[i:i + PER_LINE]
        lines.append("    " + " ".join("0x%02x," % b for b in chunk) + "\n")
    lines.append("};\n\nconst size_t %s_LEN = sizeof %s;\n" % (name, name))

    with open(out, "w", newline="\n") as f:
        f.write("".join(lines))
    print("%s: %d bytes from %s" % (out, len(data), src))


if __name__ == "__main__":
    main(sys.argv)
