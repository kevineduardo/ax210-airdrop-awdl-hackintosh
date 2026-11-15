# SPDX-License-Identifier: MIT
"""Unit tests for tools/kc/kcsyms.py, using a small synthetic fileset Mach-O."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "kc"))
import kcsyms  # noqa: E402

BASE = 0xFFFFFF8000100000


def _segment(name, vmaddr, vmsize, fileoff, filesize):
    return struct.pack("<II16sQQQQiiII", kcsyms.LC_SEGMENT_64, 72, name.encode(),
                       vmaddr, vmsize, fileoff, filesize, 3, 3, 0, 0)


def _symtab(symoff, nsyms, stroff, strsize):
    return struct.pack("<IIIIII", kcsyms.LC_SYMTAB, 24, symoff, nsyms, stroff, strsize)


def _header(filetype, cmds):
    body = b"".join(cmds)
    return struct.pack("<IiiIIIII", kcsyms.MH_MAGIC_64, 0x01000007, 3, filetype, len(cmds), len(body), 0, 0) + body


def build_collection():
    """A fileset with one kext. The kext has a 4-slot vtable for class 'Foo':
    slot 0 -> Foo::a (chained-fixup encoded), slot 1 -> Foo::b (raw address),
    slot 2 -> ___cxa_pure_virtual, slot 3 -> 0 (end)."""
    KEXT_OFF = 0x1000
    DATA_OFF = 0x2000
    LINKEDIT_OFF = 0x3000
    text_va = BASE + KEXT_OFF   # the collection maps file offset N at BASE + N
    data_va = BASE + DATA_OFF
    names = [b"__ZTV3Foo", b"__ZN3Foo1aEv", b"__ZN3Foo1bEv", b"___cxa_pure_virtual", b"_imported"]
    strtab = b"\0"
    offs = []
    for n in names:
        offs.append(len(strtab))
        strtab += n + b"\0"
    a_va, b_va, pure_va = text_va + 0x10, text_va + 0x20, text_va + 0x30
    vt_va = data_va
    nlist = b"".join([
        struct.pack("<IBBHQ", offs[0], 0x0F, 2, 0, vt_va),
        struct.pack("<IBBHQ", offs[1], 0x0F, 1, 0, a_va),
        struct.pack("<IBBHQ", offs[2], 0x0E, 1, 0, b_va),     # local, still resolvable
        struct.pack("<IBBHQ", offs[3], 0x0F, 1, 0, pure_va),
        struct.pack("<IBBHQ", offs[4], 0x01, 0, 0, 0),         # undefined import
        struct.pack("<IBBHQ", 0, 0x24, 0, 0, 0),              # STAB, must be skipped
    ])
    symoff = LINKEDIT_OFF
    stroff = symoff + len(nlist)
    kext = _header(0xB, [
        _segment("__TEXT", text_va, 0x1000, KEXT_OFF, 0x1000),
        _segment("__DATA_CONST", data_va, 0x1000, DATA_OFF, 0x1000),
        _symtab(symoff, 6, stroff, len(strtab)),
    ])
    vtable = struct.pack("<QQ", 0, 0)                                  # offset-to-top, RTTI
    vtable += struct.pack("<Q", (1 << 63) | ((a_va - BASE) & 0x3FFFFFFF))  # chained, isAuth bit set
    vtable += struct.pack("<Q", b_va)                                  # plain pointer
    vtable += struct.pack("<Q", (pure_va - BASE) | (5 << 51))           # chained with 'next' bits
    vtable += struct.pack("<Q", 0)
    name = b"com.example.Foo\0"
    entry = struct.pack("<IIQQII", kcsyms.LC_FILESET_ENTRY, 32 + len(name), text_va, KEXT_OFF, 32, 0) + name
    top = _header(kcsyms.MH_FILESET, [_segment("__TEXT", BASE, 0x4000, 0, 0x4000), entry])
    buf = bytearray(0x4000)
    buf[0:len(top)] = top
    buf[KEXT_OFF:KEXT_OFF + len(kext)] = kext
    buf[DATA_OFF:DATA_OFF + len(vtable)] = vtable
    buf[symoff:symoff + len(nlist)] = nlist
    buf[stroff:stroff + len(strtab)] = strtab
    return bytes(buf)


class KcSymsTest(unittest.TestCase):
    def setUp(self):
        self.kc = kcsyms.KernelCollection(build_collection())

    def test_lists_fileset_entries(self):
        self.assertEqual([i.name for i in self.kc.images], ["com.example.Foo"])

    def test_symbols_skip_stabs(self):
        names = [s[0] for s in self.kc.images[0].symbols]
        self.assertEqual(len(names), 5)
        self.assertIn("_imported", names)

    def test_base_is_lowest_segment(self):
        self.assertEqual(self.kc.base, BASE)

    def test_decode_pointer(self):
        self.assertEqual(self.kc.decode_pointer(0), 0)
        self.assertEqual(self.kc.decode_pointer(0x1234), BASE + 0x1234)
        self.assertEqual(self.kc.decode_pointer((1 << 63) | 0x10), BASE + 0x10)
        self.assertEqual(self.kc.decode_pointer(0xFFFFFF8000ABCDEF), 0xFFFFFF8000ABCDEF)

    def test_vtable_slots(self):
        img, slots = self.kc.vtable("Foo")
        self.assertEqual(img.name, "com.example.Foo")
        self.assertEqual([(o, n) for o, _t, n in slots],
                         [(0, "__ZN3Foo1aEv"), (8, "__ZN3Foo1bEv"), (16, "___cxa_pure_virtual")])

    def test_missing_class(self):
        with self.assertRaises(kcsyms.MachOError):
            self.kc.vtable("Bar")

    def test_rejects_non_macho(self):
        with self.assertRaises(kcsyms.MachOError):
            kcsyms.KernelCollection(b"\0" * 64)

    def test_cli_exports(self):
        with tempfile.NamedTemporaryFile(suffix=".kc", delete=False) as f:
            f.write(build_collection())
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = kcsyms.main(["exports", f.name])
            self.assertEqual(rc, 0)
            self.assertEqual(out.getvalue().split(),
                             ["__ZTV3Foo", "__ZN3Foo1aEv", "___cxa_pure_virtual"])
        finally:
            os.unlink(f.name)


if __name__ == "__main__":
    unittest.main()
