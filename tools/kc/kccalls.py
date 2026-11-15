#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""kccalls: list what kernel-collection functions call.

    kccalls.py <kernel-collection> <nm-file|-> <mangled-name-regex> [--strings]

For every function whose mangled name matches the regex: its direct call/jmp
targets (demangled, in order, de-duplicated), the vtable offsets of the virtual
calls it makes, and with --strings the C strings it loads with lea [rip+...].

The second argument is either `-` (read the symbols from the collection itself)
or a saved `nm <kernel-collection>` listing, which is faster on repeated runs.
A function ends at the next function symbol (at most 16 KiB).

Example:
    python3 -I tools/kc/kccalls.py /System/Library/KernelCollections/BootKernelExtensions.kc - \\
        'IO80211AWDLPeerManager' --strings

Read-only. Needs clang, llvm objdump and c++filt (Xcode Command Line Tools).
Python 3.8+, standard library only; Mach-O parsing comes from kcsyms.py.
"""
import bisect
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcdis  # noqa: E402
import kcsyms  # noqa: E402

MAX_FUNC = 0x4000
DEFAULT_FUNC = 0x400
VCALL_RE = re.compile(r"call\s+qword ptr \[r\w+ \+ (0x[0-9a-f]+)\]")
LEA_RIP_RE = re.compile(r"lea\s+r\w+, \[rip \+ (0x[0-9a-f]+)\](?:\s*##\s*0x([0-9a-f]+))?")
NM_LINE_RE = re.compile(r"^[0-9a-f]{16}$")


def read_nm(path):
    """Sorted (address, name) of the text symbols (t/T) in an nm listing."""
    out = set()
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 3 and p[1] in "tT" and NM_LINE_RE.match(p[0]):
                out.add((int(p[0], 16), p[2]))
    return sorted(out)


def function_length(keys, addr):
    """Bytes from ADDR to the next function symbol, capped at MAX_FUNC."""
    i = bisect.bisect_right(keys, addr)
    end = keys[i] if i < len(keys) else addr + DEFAULT_FUNC
    return min(end - addr, MAX_FUNC)


def c_string(data, min_len=6):
    s = data.split(b"\0", 1)[0]
    try:
        st = s.decode("ascii")
    except UnicodeDecodeError:
        return None
    return st if len(st) >= min_len and st.isprintable() else None


def analyze(insns, addr, length, symbolizer, read=None):
    """Returns (direct call targets, virtual call offsets, strings) for one function.
    READ(address, size) -> bytes enables the string scan."""
    calls, slots, strs = [], [], []
    for off, ins in insns:
        t = kcdis.BRANCH_RE.match(ins)
        if t:
            tgt = (addr + int(t.group(2), 16)) & kcdis.MASK64
            if not addr <= tgt < addr + length:
                name = symbolizer.exact(tgt) or hex(tgt)
                if name not in calls:
                    calls.append(name)
        v = VCALL_RE.match(ins)
        if v and v.group(1) not in slots:
            slots.append(v.group(1))
        if read is not None:
            m = LEA_RIP_RE.match(ins)
            if m:
                # objdump's "## 0x..." comment is the exact target; without it assume a 7-byte lea.
                rel = int(m.group(2), 16) if m.group(2) else off + 7 + int(m.group(1), 16)
                try:
                    st = c_string(read((addr + rel) & kcdis.MASK64, 120))
                except kcsyms.MachOError:
                    st = None
                if st and st not in strs:
                    strs.append(st)
    return calls, slots, strs


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    want_str = "--strings" in argv
    args = [a for a in argv if a != "--strings"]
    if len(args) != 3:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    kc_path, nm_path, pattern = args
    try:
        rx = re.compile(pattern)
    except re.error as e:
        print("kccalls: bad regex: %s" % e, file=sys.stderr)
        return 2
    try:
        kc = kcsyms.KernelCollection.open(kc_path)
        text = kc.text_symbols() if nm_path == "-" else read_nm(nm_path)
        keys = [a for a, _n in text]
        sym = kcdis.Symbolizer(text)
        found = 0
        for a, n in text:
            if not rx.search(n):
                continue
            found += 1
            code = kc.read(a, function_length(keys, a))
            insns = kcdis.disassemble(code)
            calls, slots, strs = analyze(insns, a, len(code), sym, kc.read if want_str else None)
            print("=== %s  (%#x, %d bytes)" % (sym.name(n), a, len(code)))
            for name in calls:
                print("   call", name[:150])
            if slots:
                print("   virtual slots:", " ".join(slots))
            for st in strs:
                print("   str ", st[:150])
        if not found:
            print("kccalls: no function matches %r" % pattern, file=sys.stderr)
            return 1
        return 0
    except (kcsyms.MachOError, kcdis.ToolError, OSError, subprocess.CalledProcessError) as e:
        print("kccalls: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
