#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""License hygiene:
- every file we wrote (tools/, scripts/, tests/) carries an SPDX identifier;
- every source file under driver/ that differs from the upstream import carries
  the GPL-2.0 modification notice (GPL-2.0 section 2a);
- the license files exist."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OURS = ("tools", "scripts", "tests")
CODE_EXT = (".c", ".cpp", ".h", ".hpp", ".py", ".sh", ".pl", ".zsh", ".swift")
NOTICE = "ax210-airdrop-awdl-hackintosh project"
UPSTREAM_IMPORT_SUBJECT = "Import upstream sources unmodified"


def head(path, n=12):
    with open(path, encoding="utf-8", errors="replace") as f:
        return "".join(f.readline() for _ in range(n))


def changed_driver_files():
    """Driver files that differ from the upstream import commit, or None if git
    history is not available (shallow clone)."""
    try:
        rev = subprocess.run(["git", "-C", ROOT, "log", "--format=%H", "--grep", UPSTREAM_IMPORT_SUBJECT],
                             capture_output=True, text=True, check=True).stdout.split()
        if not rev:
            return None
        out = subprocess.run(["git", "-C", ROOT, "diff", "--name-only", "--diff-filter=AM", rev[-1], "--", "driver"],
                             capture_output=True, text=True, check=True).stdout
        return [p for p in out.split() if p.endswith((".c", ".cpp", ".h", ".hpp"))]
    except (OSError, subprocess.CalledProcessError):
        return None


def main():
    fails = []
    for top in OURS:
        for base, _d, files in os.walk(os.path.join(ROOT, top)):
            for name in files:
                if name.endswith(CODE_EXT) or name == "Makefile":
                    p = os.path.join(base, name)
                    if "SPDX-License-Identifier" not in head(p):
                        fails.append("missing SPDX header: " + os.path.relpath(p, ROOT))
    for lic in ("LICENSE", "driver/AirportItlwm/LICENSE", "driver/BlueToolFixup/LICENSE"):
        if not os.path.isfile(os.path.join(ROOT, lic)):
            fails.append("missing " + lic)
    changed = changed_driver_files()
    if changed is None:
        print("skip GPL notice check (no git history; use a full clone)")
    else:
        for rel in changed:
            p = os.path.join(ROOT, rel)
            if os.path.isfile(p) and NOTICE not in head(p, 4):
                fails.append("GPL-2.0 modification notice missing at the top of " + rel)
        print("ok   %d modified driver source files checked for the GPL notice" % len(changed))
    for f in fails:
        print("FAIL " + f)
    if not fails:
        print("ok   license headers")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
