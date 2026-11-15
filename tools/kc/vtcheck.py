#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""vtcheck: compare the vtables of a kext's C++ classes with their base classes in a kernel collection.

    vtcheck.py <kext-binary> <kernel-collection> KextClass:BaseClass [...]
    vtcheck.py --dump <kernel-collection> Class [slot-offset-hex ...]

For each pair, every slot of the base-class vtable in the collection must hold
the same method (demangled name without the class) as the same slot of the
kext's vtable. A mismatch means the headers the kext was built against do not
match this macOS build: virtual calls would land in the wrong method.
Destructors and pure-virtual slots are not compared. Exit status 1 if any pair
mismatches or cannot be found.

--dump prints the vtable of one class in the collection (offset from the
address point and demangled target), optionally only the given offsets.

Example (on the target macOS, kext built for it):
    python3 -I tools/kc/vtcheck.py AirportItlwm.kext/Contents/MacOS/AirportItlwm \\
        /System/Library/KernelCollections/BootKernelExtensions.kc \\
        AirportItlwmSkywalkInterface:IO80211InfraProtocol AirportItlwmAWDLInterface:IO80211AWDLProtocol

Read-only. Needs c++filt (Xcode Command Line Tools or binutils) for demangling.
Python 3.8+, standard library only; Mach-O parsing comes from kcsyms.py.
"""
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcsyms  # noqa: E402

PURE_VIRTUAL = "__cxa_pure_virtual"
MAX_REPORTED = 15


def demangle(names):
    """Demangles a list of symbol names with c++filt (one process for all)."""
    names = list(names)
    if not names:
        return []
    tool = shutil.which("c++filt")
    if tool is None:
        raise kcsyms.MachOError("c++filt not found (install the Xcode Command Line Tools or binutils)")
    out = subprocess.run([tool], input="\n".join(names) + "\n", capture_output=True, text=True,
                         check=True).stdout
    lines = out.split("\n")
    return [lines[i] if i < len(lines) else n for i, n in enumerate(names)]


def method(demangled):
    """'Foo::bar(int) const' -> 'bar(int) const'. Only the class qualifier is removed."""
    if demangled is None:
        return None
    if not re.match(r"^(?:[\w:<>~]+::)?([~\w]+|operator\S+)(\(.*)$", demangled):
        return demangled
    paren = demangled.index("(")
    return demangled[:paren].split("::")[-1] + demangled[paren:]


def kc_vtable(kc, cls):
    """Slot targets (symbol names, or hex addresses when unnamed) of CLS in the collection,
    or None if the class has no vtable there."""
    try:
        _img, slots = kc.vtable(cls)
    except kcsyms.MachOError:
        return None
    return [name or "0x%x" % target for _off, target, name in slots]


def kext_vtable(kx, cls):
    """Slot targets of CLS in an unlinked kext binary. Slots that point outside the kext
    are external relocations (the symbol is named in the relocation entry); slots inside
    it are plain pointers. Returns None if the kext has no vtable for CLS."""
    img = kx.images[0]
    sym = "__ZTV%d%s" % (len(cls), cls)
    addr = next((v for n, t, v in img.symbols if n == sym and (t & kcsyms.N_TYPE) == kcsyms.N_SECT), None)
    if addr is None:
        return None
    ext = {}
    if img.dysymtab and img.segments:
        extreloff, nextrel = img.dysymtab[14], img.dysymtab[15]
        base = img.segments[0][1]
        for i in range(nextrel):
            r_addr, info = struct.unpack_from("<iI", kx.buf, extreloff + 8 * i)
            ext[base + r_addr] = kx.symbol_at_index(img, info & 0xFFFFFF)
    defined = {}
    for n, t, v in img.symbols:
        if (t & kcsyms.N_TYPE) == kcsyms.N_SECT:
            defined.setdefault(v, n)
    later = sorted(v for v in defined if v > addr)
    end = later[0] if later else addr + 0x4000
    slots = []
    a = addr + 16  # skip offset-to-top and RTTI
    while a < end:
        if a in ext:
            slots.append(ext[a])
        else:
            raw = kx.read(a, 8)
            if len(raw) < 8:
                break
            value = struct.unpack("<Q", raw)[0]
            if value == 0:
                break
            slots.append(defined.get(value, "0x%x" % value))
        a += 8
    return slots


def compare(kc_slots, kext_slots, demangler=None):
    """Returns [(slot index, kc method, kext method)] for every slot that differs."""
    names = sorted(set(kc_slots) | set(kext_slots))
    dm = dict(zip(names, (demangler or demangle)(names)))
    mism = []
    for i, b in enumerate(kc_slots):
        k = kext_slots[i] if i < len(kext_slots) else None
        mb = method(dm.get(b, b))
        mk = method(dm.get(k, k)) if k is not None else None
        if mb == mk:
            continue
        if (mb or "").startswith("~"):
            continue
        if PURE_VIRTUAL in (b or "") or PURE_VIRTUAL in (k or ""):
            continue
        mism.append((i, mb, mk))
    return mism


def dump(kc_path, cls, offsets, out=None):
    out = out or sys.stdout
    kc = kcsyms.KernelCollection.open(kc_path)
    slots = kc_vtable(kc, cls)
    if slots is None:
        print("vtcheck: no vtable for %s in %s" % (cls, kc_path), file=sys.stderr)
        return 1
    want = {int(x, 16) for x in offsets}
    for i, d in enumerate(demangle(slots)):
        if not want or 8 * i in want:
            print("%#06x %s" % (8 * i, d), file=out)
    return 0


def check(kext_path, kc_path, pairs, out=None):
    out = out or sys.stdout
    kx = kcsyms.KernelCollection.open(kext_path)
    kc = kcsyms.KernelCollection.open(kc_path)
    bad = 0
    for kcls, bcls in pairs:
        kv = kext_vtable(kx, kcls)
        bv = kc_vtable(kc, bcls)
        if kv is None or bv is None:
            print("SKIP %s:%s (kext vtable %s, kc vtable %s)"
                  % (kcls, bcls, "found" if kv is not None else "missing",
                     "found" if bv is not None else "missing"), file=out)
            bad += 1
            continue
        mism = compare(bv, kv)
        print("%s %s : %s  kc slots %d, kext slots %d, mismatches %d"
              % ("OK  " if not mism else "FAIL", kcls, bcls, len(bv), len(kv), len(mism)), file=out)
        for i, mb, mk in mism[:MAX_REPORTED]:
            print("      +%#06x kc=%s | kext=%s" % (8 * i, mb, mk), file=out)
        bad += bool(mism)
    return 1 if bad else 0


def usage():
    print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
    return 2


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv and argv[0] == "--dump":
            if len(argv) < 3:
                return usage()
            return dump(argv[1], argv[2], argv[3:])
        if len(argv) < 3 or argv[0].startswith("-"):
            return usage()
        pairs = []
        for p in argv[2:]:
            if p.count(":") != 1 or "" in p.split(":"):
                print("vtcheck: expected KextClass:BaseClass, got %r" % p, file=sys.stderr)
                return 2
            pairs.append(tuple(p.split(":")))
        return check(argv[0], argv[1], pairs)
    except (kcsyms.MachOError, OSError, subprocess.CalledProcessError) as e:
        print("vtcheck: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
