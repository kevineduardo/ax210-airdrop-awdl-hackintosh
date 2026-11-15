# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""Unit tests for tools/kc/kcdis.py and tools/kc/kccalls.py, on recorded objdump output
and synthetic Mach-O files. The clang/objdump round trip only runs on macOS."""
import contextlib
import io
import os
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "kc"))
import kccalls  # noqa: E402
import kcdis  # noqa: E402
import kcsyms  # noqa: E402
import test_vtcheck  # noqa: E402

FUNC = 0x1000
# llvm-objdump -d --no-show-raw-insn --x86-asm-syntax=intel output for CODE below, placed at FUNC.
CODE = bytes.fromhex("e810000000" "488d0520000000" "ff9048010000" "e900010000" "c3")
OBJDUMP = """
code.o:\tfile format mach-o 64-bit x86-64

Disassembly of section __TEXT,__text:

0000000000000000 <_f>:
       0:      \tcall\t0x15 <_f+0x15>
       5:      \tlea\trax, [rip + 0x20]       ## 0x2c <_f+0x2c>
       c:      \tcall\tqword ptr [rax + 0x148]
      12:      \tjmp\t0x117 <_f+0x117>
      17:      \tret
"""


def identity(name):
    return name


class ParseTest(unittest.TestCase):
    def test_parse_objdump(self):
        insns = kcdis.parse_objdump(OBJDUMP)
        self.assertEqual([o for o, _i in insns], [0, 5, 0xC, 0x12, 0x17])
        self.assertTrue(insns[0][1].startswith("call"))

    def test_render_symbolises_external_targets_only(self):
        sym = kcdis.Symbolizer([(FUNC + 0x100, "_other"), (FUNC + 0x110, "_next")], identity)
        lines = kcdis.render(kcdis.parse_objdump(OBJDUMP), FUNC, 0x18, sym)
        self.assertEqual(lines, [
            "+0000  call\t0x15",                      # inside the range: left as is
            "+0005  lea\trax, [rip + 0x20]",
            "+000c  call\tqword ptr [rax + 0x148]",
            "+0012  jmp _next+0x7",
            "+0017  ret",
        ])

    def test_symbolizer(self):
        sym = kcdis.Symbolizer([(0x10, "_a"), (0x20, "_b")], identity)
        self.assertEqual(sym(0x10), "_a")
        self.assertEqual(sym(0x18), "_a+0x8")
        self.assertEqual(sym(0x5), "0x5")
        self.assertEqual(sym.exact(0x20), "_b")
        self.assertIsNone(sym.exact(0x21))


class KccallsTest(unittest.TestCase):
    def test_read_nm(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("ffffff8000001000 T _b\nffffff8000000800 t _a\nffffff8000002000 S _data\n"
                    "                 U _undef\n")
        try:
            self.assertEqual(kccalls.read_nm(f.name), [(0xFFFFFF8000000800, "_a"), (0xFFFFFF8000001000, "_b")])
        finally:
            os.unlink(f.name)

    def test_function_length(self):
        keys = [0x100, 0x180, 0x10000]
        self.assertEqual(kccalls.function_length(keys, 0x100), 0x80)
        self.assertEqual(kccalls.function_length(keys, 0x180), kccalls.MAX_FUNC)
        self.assertEqual(kccalls.function_length(keys, 0x10000), kccalls.DEFAULT_FUNC)

    def test_c_string(self):
        self.assertEqual(kccalls.c_string(b"hello world\0junk"), "hello world")
        self.assertIsNone(kccalls.c_string(b"abc\0"))
        self.assertIsNone(kccalls.c_string(b"\xff\xfe\xfd\xfc\xfb\xfa\xf9\0"))

    def test_analyze(self):
        sym = kcdis.Symbolizer([(FUNC, "_f"), (FUNC + 0x117, "_target")], identity)
        mem = {FUNC + 0x2C: b"a test string\0"}

        def read(addr, size):
            if addr not in mem:
                raise kcsyms.MachOError("unmapped")
            return mem[addr][:size]

        calls, slots, strs = kccalls.analyze(kcdis.parse_objdump(OBJDUMP), FUNC, 0x18, sym, read)
        self.assertEqual(calls, ["_target"])
        self.assertEqual(slots, ["0x148"])
        self.assertEqual(strs, ["a test string"])
        calls, slots, strs = kccalls.analyze(kcdis.parse_objdump(OBJDUMP), FUNC, 0x18, sym)
        self.assertEqual(strs, [])

    def test_analyze_lea_without_comment(self):
        sym = kcdis.Symbolizer([], identity)
        insns = [(0x10, "lea\trdi, [rip + 0x30]")]
        seen = []

        def read(addr, size):
            seen.append(addr)
            return b"\0"

        kccalls.analyze(insns, FUNC, 0x20, sym, read)
        self.assertEqual(seen, [FUNC + 0x10 + 7 + 0x30])

    def test_main_with_kc_symbols(self):
        # A kext whose only text symbol is __ZN7Derived1aEv at 0x100; disassembly is mocked.
        kext = test_vtcheck.build_kext("Derived", [("local", "__ZN7Derived1aEv")])
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(kext)
        out = io.StringIO()
        try:
            with mock.patch.object(kcdis, "disassemble", return_value=kcdis.parse_objdump(OBJDUMP)), \
                    mock.patch.object(kcdis, "demangle_one", side_effect=identity), \
                    contextlib.redirect_stdout(out):
                rc = kccalls.main([f.name, "-", "Derived1a"])
            self.assertEqual(rc, 0)
            text = out.getvalue()
            self.assertIn("=== __ZN7Derived1aEv  (0x100,", text)
            self.assertIn("   virtual slots: 0x148", text)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(kccalls.main([f.name, "-", "NoSuchFunction"]), 1)
                self.assertEqual(kccalls.main([f.name, "-"]), 2)
        finally:
            os.unlink(f.name)


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang") and shutil.which("objdump"),
                     "needs clang and llvm objdump (macOS)")
class RoundTripTest(unittest.TestCase):
    def test_disassemble(self):
        insns = kcdis.disassemble(CODE)
        self.assertEqual([o for o, _i in insns], [0, 5, 0xC, 0x12, 0x17])
        self.assertRegex(insns[1][1], r"^lea\s+rax, \[rip \+ 0x20\]")

    def test_cli_on_synthetic_kext(self):
        # Put CODE at the kext's text symbol and disassemble it through the CLI.
        kext = bytearray(test_vtcheck.build_kext("Derived", [("local", "__ZN7Derived1aEv")]))
        kext[0x100:0x100 + len(CODE)] = CODE
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "kext")
            with open(path, "wb") as f:
                f.write(kext)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = kcdis.main([path, "100", "%x" % len(CODE), os.path.join(tmp, "keep")])
            self.assertEqual(rc, 0)
            self.assertTrue(os.path.exists(os.path.join(tmp, "keep.o")))
            lines = out.getvalue().splitlines()
            self.assertEqual(len(lines), 5)
            self.assertTrue(lines[3].startswith("+0012  jmp "))


if __name__ == "__main__":
    unittest.main()
