#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""kcdis: disassemble an address range of a kernel collection (x86_64, Intel syntax).

    kcdis.py <kernel-collection> <vmaddr-hex> <length-hex> [out-prefix]

call/jmp targets outside the range are replaced by the collection's own symbol
names (demangled, as symbol+offset when not at a symbol start). With out-prefix,
the intermediate <out-prefix>.bin/.s/.o files are kept; otherwise they go to a
temporary directory.

Example:
    python3 -I tools/kc/kcdis.py /System/Library/KernelCollections/BootKernelExtensions.kc \\
        ffffff8001234560 200

Read-only. Needs clang and llvm objdump (Xcode Command Line Tools); override with
$CLANG / $OBJDUMP. Python 3.8+, standard library only; Mach-O parsing comes from kcsyms.py.
"""
import bisect
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcsyms  # noqa: E402

LINE_RE = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
BRANCH_RE = re.compile(r"\b(call|jmp)\s+0x([0-9a-f]+)")
COMMENT_RE = re.compile(r"\s*##.*")
LOCAL_LABEL_RE = re.compile(r"\s*<_f(?:\+0x[0-9a-f]+)?>")
MASK64 = 0xFFFFFFFFFFFFFFFF


class ToolError(Exception):
    pass


def _tool(env, name):
    path = os.environ.get(env) or shutil.which(name)
    if not path:
        raise ToolError("%s not found (install the Xcode Command Line Tools or set $%s)" % (name, env))
    return path


def objdump_text(code, prefix=None):
    """Wraps CODE in an object file with clang and returns `objdump -d` output (Intel syntax).
    Addresses in the output are offsets from the start of CODE."""
    clang, objdump = _tool("CLANG", "clang"), _tool("OBJDUMP", "objdump")
    with tempfile.TemporaryDirectory(prefix="kcdis") as tmp:
        pre = os.path.abspath(prefix) if prefix else os.path.join(tmp, "code")
        with open(pre + ".bin", "wb") as f:
            f.write(code)
        with open(pre + ".s", "w") as f:
            f.write('.text\n.globl _f\n_f:\n.incbin "%s.bin"\n' % pre.replace("\\", "\\\\").replace('"', '\\"'))
        subprocess.run([clang, "-c", "-target", "x86_64-apple-macos11", pre + ".s", "-o", pre + ".o"],
                       check=True, capture_output=True)
        return subprocess.run([objdump, "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", pre + ".o"],
                              check=True, capture_output=True, text=True).stdout


def parse_objdump(text):
    """[(offset, instruction text)] from objdump output."""
    out = []
    for line in text.splitlines():
        m = LINE_RE.match(line)
        if m:
            out.append((int(m.group(1), 16), m.group(2).strip()))
    return out


def disassemble(code, prefix=None):
    return parse_objdump(objdump_text(code, prefix))


class Symbolizer:
    """Maps addresses to 'symbol' or 'symbol+0xoff' using sorted (address, name) pairs."""

    def __init__(self, text_symbols, demangler=None):
        self.syms = list(text_symbols)
        self.keys = [a for a, _n in self.syms]
        self.demangler = demangler or demangle_one
        self.cache = {}

    def name(self, n):
        if n not in self.cache:
            self.cache[n] = self.demangler(n)
        return self.cache[n]

    def exact(self, addr):
        """Demangled name of the symbol that starts at ADDR, or None."""
        i = bisect.bisect_right(self.keys, addr) - 1
        if i >= 0 and self.keys[i] == addr:
            return self.name(self.syms[i][1])
        return None

    def __call__(self, addr):
        i = bisect.bisect_right(self.keys, addr) - 1
        if i < 0:
            return hex(addr)
        base, n = self.syms[i]
        d = self.name(n)
        return d if addr == base else "%s+0x%x" % (d, addr - base)


def demangle_one(name):
    tool = shutil.which("c++filt")
    if tool is None:
        return name
    return subprocess.run([tool, name], capture_output=True, text=True).stdout.strip() or name


def render(insns, addr, length, symbolize):
    """Instruction lines with external call/jmp targets symbolised."""
    lines = []
    for off, ins in insns:
        t = BRANCH_RE.search(ins)
        if t:
            tgt = (int(t.group(2), 16) + addr) & MASK64
            if not addr <= tgt < addr + length:
                ins = "%s %s" % (t.group(1), symbolize(tgt))
        ins = LOCAL_LABEL_RE.sub("", COMMENT_RE.sub("", ins))
        lines.append("+%04x  %s" % (off, ins))
    return lines


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) not in (3, 4):
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        addr, length = int(argv[1], 16), int(argv[2], 16)
        kc = kcsyms.KernelCollection.open(argv[0])
        code = kc.read(addr, length)
        insns = disassemble(code, argv[3] if len(argv) > 3 else None)
        for line in render(insns, addr, len(code), Symbolizer(kc.text_symbols())):
            print(line)
        return 0
    except ValueError as e:
        print("kcdis: bad number: %s" % e, file=sys.stderr)
    except (kcsyms.MachOError, ToolError, OSError, subprocess.CalledProcessError) as e:
        print("kcdis: %s" % e, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
