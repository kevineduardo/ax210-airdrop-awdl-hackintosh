# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""Unit tests for tools/kc/vtcheck.py (and the kcsyms helpers it uses), with a synthetic
unlinked kext and the synthetic fileset from test_kcsyms.py."""
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
import kcsyms  # noqa: E402
import test_kcsyms  # noqa: E402
import vtcheck  # noqa: E402

MH_KEXT_BUNDLE = 0xB
TEXT_VA, DATA_VA = 0x0, 0x1000
TEXT_PROT, DATA_PROT = 5, 3


def _segment(name, vmaddr, size, fileoff, prot):
    return struct.pack("<II16sQQQQiiII", kcsyms.LC_SEGMENT_64, 72, name.encode(),
                       vmaddr, size, fileoff, size, prot, prot, 0, 0)


def build_kext(cls, slots):
    """An unlinked kext (MH_KEXT_BUNDLE) with a vtable for CLS. SLOTS is a list of
    ("local", name) - a function defined in the kext - or ("ext", name) - an import,
    filled by an external relocation. Symbol 0 is a stab, so relocation indices only
    match if the raw symbol table index is used."""
    LINKEDIT = 0x2000
    syms = [("", 0x64, 0, 0)]                       # N_SO stab
    func_va = TEXT_VA + 0x100
    local_addr = {}
    for kind, name in slots:
        if kind == "local" and name not in local_addr:
            local_addr[name] = func_va
            syms.append((name, 0x0F, 1, func_va))
            func_va += 0x10
    vt_name = "__ZTV%d%s" % (len(cls), cls)
    syms.append((vt_name, 0x0F, 2, DATA_VA))
    vt_end = DATA_VA + 16 + 8 * len(slots) + 8
    syms.append(("__ZTV5After", 0x0F, 2, vt_end + 0x20))  # bounds the vtable
    ext_index = {}
    for kind, name in slots:
        if kind == "ext" and name not in ext_index:
            ext_index[name] = len(syms)
            syms.append((name, 0x01, 0, 0))

    strtab = b"\0"
    nlist = b""
    for name, ntype, sect, value in syms:
        strx = len(strtab) if name else 0
        if name:
            strtab += name.encode() + b"\0"
        nlist += struct.pack("<IBBHQ", strx, ntype, sect, 0, value)
    vt = struct.pack("<QQ", 0, 0)
    relocs = b""
    for i, (kind, name) in enumerate(slots):
        if kind == "local":
            vt += struct.pack("<Q", local_addr[name])
        else:
            vt += struct.pack("<Q", 0)
            r_addr = DATA_VA + 16 + 8 * i - TEXT_VA
            info = ext_index[name] | (3 << 25) | (1 << 27)   # length 8, extern
            relocs += struct.pack("<iI", r_addr, info)
    vt += struct.pack("<Q", 0)

    symoff = LINKEDIT
    stroff = symoff + len(nlist)
    extreloff = stroff + len(strtab)
    dysym = [0] * 18
    dysym[14], dysym[15] = extreloff, len(relocs) // 8
    cmds = [
        _segment("__TEXT", TEXT_VA, 0x1000, 0, TEXT_PROT),
        _segment("__DATA", DATA_VA, 0x1000, 0x1000, DATA_PROT),
        struct.pack("<IIIIII", kcsyms.LC_SYMTAB, 24, symoff, len(syms), stroff, len(strtab)),
        struct.pack("<II18I", kcsyms.LC_DYSYMTAB, 80, *dysym),
    ]
    hdr = test_kcsyms._header(MH_KEXT_BUNDLE, cmds)
    buf = bytearray(extreloff + len(relocs))
    buf[0:len(hdr)] = hdr
    buf[0x1000:0x1000 + len(vt)] = vt
    buf[symoff:symoff + len(nlist)] = nlist
    buf[stroff:stroff + len(strtab)] = strtab
    buf[extreloff:extreloff + len(relocs)] = relocs
    return bytes(buf)


# Enough of the Itanium demangling for the names used here, so the tests do not need c++filt.
FAKE_DEMANGLED = {
    "__ZN3Foo1aEv": "Foo::a()",
    "__ZN3Foo1bEv": "Foo::b()",
    "__ZN7Derived1aEv": "Derived::a()",
    "__ZN7Derived1cEv": "Derived::c()",
    "__ZN7DerivedD0Ev": "Derived::~Derived()",
    "__ZN3FooD0Ev": "Foo::~Foo()",
    "___cxa_pure_virtual": "__cxa_pure_virtual",
}


def fake_demangle(names):
    return [FAKE_DEMANGLED.get(n, n) for n in names]


class MethodTest(unittest.TestCase):
    def test_strips_class_only(self):
        self.assertEqual(vtcheck.method("IOService::start(IOService*)"), "start(IOService*)")
        self.assertEqual(vtcheck.method("Foo::bar(int) const"), "bar(int) const")
        self.assertEqual(vtcheck.method("ns::Foo::~Foo()"), "~Foo()")
        self.assertEqual(vtcheck.method("plain()"), "plain()")

    def test_non_function_unchanged(self):
        self.assertEqual(vtcheck.method("0x1234"), "0x1234")
        self.assertIsNone(vtcheck.method(None))


class CompareTest(unittest.TestCase):
    def test_same_methods_other_class(self):
        self.assertEqual(vtcheck.compare(["__ZN3Foo1aEv", "__ZN3Foo1bEv"],
                                         ["__ZN7Derived1aEv", "__ZN3Foo1bEv"], fake_demangle), [])

    def test_mismatch_reported_with_slot(self):
        self.assertEqual(vtcheck.compare(["__ZN3Foo1aEv"], ["__ZN7Derived1cEv"], fake_demangle),
                         [(0, "a()", "c()")])

    def test_destructor_and_pure_virtual_ignored(self):
        self.assertEqual(vtcheck.compare(["__ZN3FooD0Ev", "___cxa_pure_virtual", "__ZN3Foo1aEv"],
                                         ["__ZN7Derived1cEv", "__ZN7Derived1aEv", "__ZN7Derived1aEv"],
                                         fake_demangle), [])

    def test_kext_vtable_too_short(self):
        self.assertEqual(vtcheck.compare(["__ZN3Foo1aEv", "__ZN3Foo1bEv"], ["__ZN7Derived1aEv"], fake_demangle),
                         [(1, "b()", None)])

    @unittest.skipUnless(shutil.which("c++filt"), "c++filt not installed")
    def test_real_demangler(self):
        self.assertEqual(vtcheck.compare(["__ZN3Foo1aEv"], ["__ZN7Derived1cEv"]), [(0, "a()", "c()")])


class KcHelpersTest(unittest.TestCase):
    def setUp(self):
        self.kx = kcsyms.KernelCollection(build_kext("Derived", [("local", "__ZN7Derived1aEv"),
                                                                 ("ext", "__ZN3Foo1bEv")]))

    def test_symbol_at_index_counts_stabs(self):
        img = self.kx.images[0]
        self.assertEqual(self.kx.symbol_at_index(img, 0), "")
        self.assertEqual(self.kx.symbol_at_index(img, 1), "__ZN7Derived1aEv")
        with self.assertRaises(kcsyms.MachOError):
            self.kx.symbol_at_index(img, 99)

    def test_read_and_text_symbols(self):
        self.assertEqual(self.kx.read(DATA_VA + 16, 8), struct.pack("<Q", 0x100))
        self.assertEqual(self.kx.text_symbols(), [(0x100, "__ZN7Derived1aEv")])
        with self.assertRaises(kcsyms.MachOError):
            self.kx.read(0x9000, 8)

    def test_dysymtab_parsed(self):
        self.assertEqual(self.kx.images[0].dysymtab[15], 1)


class VtablesTest(unittest.TestCase):
    def test_kext_vtable_local_and_external(self):
        kx = kcsyms.KernelCollection(build_kext("Derived", [("local", "__ZN7Derived1aEv"),
                                                            ("ext", "__ZN3Foo1bEv"),
                                                            ("ext", "___cxa_pure_virtual")]))
        self.assertEqual(vtcheck.kext_vtable(kx, "Derived"),
                         ["__ZN7Derived1aEv", "__ZN3Foo1bEv", "___cxa_pure_virtual"])
        self.assertIsNone(vtcheck.kext_vtable(kx, "Missing"))

    def test_kc_vtable(self):
        kc = kcsyms.KernelCollection(test_kcsyms.build_collection())
        self.assertEqual(vtcheck.kc_vtable(kc, "Foo"), ["__ZN3Foo1aEv", "__ZN3Foo1bEv", "___cxa_pure_virtual"])
        self.assertIsNone(vtcheck.kc_vtable(kc, "Missing"))


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.kc = os.path.join(self.tmp, "test.kc")
        with open(self.kc, "wb") as f:
            f.write(test_kcsyms.build_collection())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def kext(self, slots):
        path = os.path.join(self.tmp, "kext")
        with open(path, "wb") as f:
            f.write(build_kext("Derived", slots))
        return path

    def run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(vtcheck, "demangle", fake_demangle), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = vtcheck.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def test_matching_pair(self):
        k = self.kext([("local", "__ZN7Derived1aEv"), ("ext", "__ZN3Foo1bEv"), ("ext", "___cxa_pure_virtual")])
        rc, out, _ = self.run_main([k, self.kc, "Derived:Foo"])
        self.assertEqual(rc, 0)
        self.assertIn("OK   Derived : Foo  kc slots 3, kext slots 3, mismatches 0", out)

    def test_mismatching_pair(self):
        k = self.kext([("local", "__ZN7Derived1cEv"), ("ext", "__ZN3Foo1bEv"), ("ext", "___cxa_pure_virtual")])
        rc, out, _ = self.run_main([k, self.kc, "Derived:Foo"])
        self.assertEqual(rc, 1)
        self.assertIn("FAIL Derived : Foo", out)
        self.assertIn("+0x0000 kc=a() | kext=c()", out)

    def test_missing_class_is_skip(self):
        k = self.kext([("local", "__ZN7Derived1aEv")])
        rc, out, _ = self.run_main([k, self.kc, "Derived:Nope"])
        self.assertEqual(rc, 1)
        self.assertIn("SKIP Derived:Nope (kext vtable found, kc vtable missing)", out)

    def test_dump(self):
        rc, out, _ = self.run_main(["--dump", self.kc, "Foo"])
        self.assertEqual(rc, 0)
        self.assertEqual(out.splitlines(), ["0x0000 Foo::a()", "0x0008 Foo::b()", "0x0010 __cxa_pure_virtual"])
        rc, out, _ = self.run_main(["--dump", self.kc, "Foo", "8"])
        self.assertEqual(out.splitlines(), ["0x0008 Foo::b()"])

    def test_bad_arguments(self):
        self.assertEqual(self.run_main([])[0], 2)
        self.assertEqual(self.run_main(["a", "b", "NoColon"])[0], 2)
        rc, _, err = self.run_main([os.path.join(self.tmp, "missing"), self.kc, "A:B"])
        self.assertEqual(rc, 1)
        self.assertIn("vtcheck:", err)


if __name__ == "__main__":
    unittest.main()
