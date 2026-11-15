#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Every AWDL boot argument read by the driver must be documented in
docs/boot-args.md, and every one documented there must exist in the code."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CODE_DIRS = [os.path.join(ROOT, "driver", "AirportItlwm", d) for d in ("AirportItlwm", "itlwm", "itl80211")]
DOC = os.path.join(ROOT, "docs", "boot-args.md")
PATTERN = re.compile(r'PE_parse_boot_argn\("(-?itlawdl[a-z]*)"')


def in_code():
    found = set()
    for d in CODE_DIRS:
        for base, _dirs, files in os.walk(d):
            for name in files:
                if name.endswith((".c", ".cpp", ".h", ".hpp")):
                    with open(os.path.join(base, name), encoding="utf-8", errors="replace") as f:
                        found.update(PATTERN.findall(f.read()))
    return found


def in_doc():
    with open(DOC, encoding="utf-8") as f:
        text = f.read()
    return set(re.findall(r"`(-?itlawdl[a-z]*)(?:=[^`]*)?`", text))


def main():
    code, doc = in_code(), in_doc()
    ok = True
    for arg in sorted(code - doc):
        print("FAIL boot-arg %s is read by the driver but not documented in docs/boot-args.md" % arg)
        ok = False
    for arg in sorted(doc - code):
        print("FAIL boot-arg %s is documented but the driver never reads it" % arg)
        ok = False
    if ok:
        print("ok   %d AWDL boot-args, code and docs agree" % len(code))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
