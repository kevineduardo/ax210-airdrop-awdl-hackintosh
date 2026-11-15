# SPDX-License-Identifier: MIT
"""Static checks of the BlueToolFixup -btlfxadvsets patch tables.

These run without any Apple binary: they only check that every table entry in
driver/BlueToolFixup/BrcmPatchRAM/BlueToolFixup.cpp is internally consistent and
that tools/advsets/verify-advsets.pl searches for the same patterns the kext uses.
Checking the patterns against a real /usr/sbin/bluetoothd is a manual step
(see docs/porting-awdl-to-new-macos-versions.md).
"""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "driver", "BlueToolFixup", "BrcmPatchRAM", "BlueToolFixup.cpp")
VERIFIER = os.path.join(ROOT, "tools", "advsets", "verify-advsets.pl")


def _strip_comments(code):
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


def load_arrays(code):
    arrays = {}
    for m in re.finditer(r"static const uint8_t (k\w+)\[\d*\]\s*=\s*\{(.*?)\};", code, re.S):
        arrays[m.group(1)] = [int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
    return arrays


def load_table(code, name):
    m = re.search(r"%s\[kAdvSetsPatchCount\]\s*=\s*\{(.*?)\};" % name, code, re.S)
    if not m:
        return None
    entries = []
    for e in re.finditer(r'ADVSETS_(EXACT|MASKED)\("([^"]+)",\s*(\w+),\s*(\w+)(?:,\s*(\w+),\s*(\w+))?\)', m.group(1)):
        kind, label, a, b, c, d = e.groups()
        if kind == "EXACT":
            entries.append((label, kind, a, None, b, None))
        else:
            entries.append((label, kind, a, b, c, d))
    return entries


def as_pattern(find, fmask):
    return " ".join("%02X" % f if m == 0xFF else "??" for f, m in zip(find, fmask))


class AdvSetsTablesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(SRC, encoding="utf-8") as f:
            cls.code = _strip_comments(f.read())
        cls.arrays = load_arrays(cls.code)
        count = re.search(r"kAdvSetsPatchCount\s*=\s*(\d+)", cls.code)
        cls.count = int(count.group(1)) if count else 6

    def tables(self):
        found = {}
        for name in re.findall(r"static const AdvSetsPatch (kAdvSetsPatches\w+)\[", self.code):
            found[name] = load_table(self.code, name)
        self.assertTrue(found, "no AdvSetsPatch tables found")
        return found

    def test_every_table_has_all_patches(self):
        for name, entries in self.tables().items():
            self.assertEqual(len(entries), self.count, name)

    def test_entries_are_consistent(self):
        for name, entries in self.tables().items():
            for label, kind, f, fm, r, rm in entries:
                where = "%s / %s" % (name, label)
                find = self.arrays[f]
                fmask = self.arrays[fm] if fm else [0xFF] * len(find)
                rep = self.arrays[r]
                rmask = self.arrays[rm] if rm else [0xFF] * len(find)
                self.assertTrue(len(find) == len(fmask) == len(rep) == len(rmask), where + ": sizes differ")
                self.assertGreaterEqual(len(find), 8, where + ": pattern too short to be unique")
                self.assertTrue(set(fmask) <= {0x00, 0xFF}, where + ": find mask must be 00/FF per byte")
                self.assertTrue(set(rmask) <= {0x00, 0xFF}, where + ": replace mask must be 00/FF per byte")
                for i, (b, m) in enumerate(zip(find, fmask)):
                    if m == 0:
                        self.assertEqual(b, 0, "%s: wildcard byte %d must be 0x00 in find" % (where, i))
                for i, (b, m) in enumerate(zip(rep, rmask)):
                    if m == 0:
                        self.assertEqual(b, 0 if fmask[i] == 0 else b, "%s: byte %d" % (where, i))
                changed = [i for i in range(len(find)) if rmask[i] == 0xFF and (fmask[i] == 0 or rep[i] != find[i])]
                self.assertTrue(changed, where + ": the replacement changes nothing")
                fixed = sum(1 for m in fmask if m == 0xFF)
                self.assertGreaterEqual(fixed, 6, where + ": fewer than 6 fixed bytes")

    def test_uuid_is_16_bytes(self):
        for name, data in self.arrays.items():
            if name.startswith("kBluetoothdUuid"):
                self.assertEqual(len(data), 16, name)

    def test_verifier_matches_kext_sequoia_table(self):
        with open(VERIFIER, encoding="utf-8") as f:
            perl = f.read()
        seq = re.search(r"my @seq = \((.*?)\n\);", perl, re.S)
        self.assertIsNotNone(seq, "@seq table not found in verify-advsets.pl")
        verifier = [m.group(1) for m in re.finditer(r"\['[^']*',\s*'([0-9A-F? ]+)'", seq.group(1))]
        entries = load_table(self.code, "kAdvSetsPatches24")
        kext = []
        for _label, _kind, f, fm, _r, _rm in entries:
            find = self.arrays[f]
            kext.append(as_pattern(find, self.arrays[fm] if fm else [0xFF] * len(find)))
        self.assertEqual(verifier, kext)


if __name__ == "__main__":
    unittest.main()
