#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""kcsyms: read symbols and C++ vtables from a macOS kernel collection (.kc).

Works on the kernel collection of YOUR OWN macOS installation, read in place.
Nothing from Apple is bundled with this tool, and its output (symbol names,
vtable slot order) should stay on your machine unless you only publish the
minimum needed for interoperability (see docs/macos-driver-development.md).

    kcsyms.py kexts   [KC]                      list the kexts in the collection
    kcsyms.py symbols [KC] [-k BUNDLE] [--exported] [--undefined]
    kcsyms.py vtable  [KC] CLASS [-k BUNDLE]    vtable slots: offset, target symbol
    kcsyms.py exports [KC] -k BUNDLE ... > kc-syms.txt   (input for tools/check-kext.sh)

KC defaults to /System/Library/KernelCollections/BootKernelExtensions.kc.
Python 3.8+, standard library only.
"""
import argparse
import bisect
import struct
import sys

DEFAULT_KC = "/System/Library/KernelCollections/BootKernelExtensions.kc"

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
MH_FILESET = 0xC
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_FILESET_ENTRY = 0x80000035
N_STAB = 0xE0
N_TYPE = 0x0E
N_EXT = 0x01
N_SECT = 0x0E
N_UNDF = 0x0


class MachOError(Exception):
    pass


class Image:
    """One Mach-O inside the collection (the kernel or a kext)."""

    def __init__(self, name, vmaddr, fileoff):
        self.name = name
        self.vmaddr = vmaddr
        self.fileoff = fileoff
        self.segments = []   # (name, vmaddr, vmsize, fileoff, filesize)
        self.symbols = []    # (name, type, value)


def _cstr(buf, off):
    end = buf.find(b"\0", off)
    if end < 0:
        end = len(buf)
    return buf[off:end].decode("utf-8", "replace")


def _parse_header(buf, off):
    if off + 32 > len(buf):
        raise MachOError("truncated Mach-O header at 0x%x" % off)
    magic, cputype, _sub, filetype, ncmds, sizeofcmds = struct.unpack_from("<IiiIII", buf, off)
    if magic != MH_MAGIC_64:
        raise MachOError("not a 64-bit Mach-O at 0x%x (magic 0x%08x)" % (off, magic))
    return cputype, filetype, ncmds, sizeofcmds


def _load_commands(buf, off):
    _cpu, _ft, ncmds, sizeofcmds = _parse_header(buf, off)
    p = off + 32
    end = p + sizeofcmds
    for _ in range(ncmds):
        if p + 8 > end:
            raise MachOError("load commands overflow at 0x%x" % p)
        cmd, cmdsize = struct.unpack_from("<II", buf, p)
        if cmdsize < 8 or p + cmdsize > end:
            raise MachOError("bad cmdsize %d at 0x%x" % (cmdsize, p))
        yield cmd, p, cmdsize
        p += cmdsize


def _parse_image(buf, img):
    for cmd, p, _size in _load_commands(buf, img.fileoff):
        if cmd == LC_SEGMENT_64:
            segname = buf[p + 8:p + 24].split(b"\0", 1)[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", buf, p + 24)
            img.segments.append((segname, vmaddr, vmsize, fileoff, filesize))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", buf, p + 8)
            if symoff + nsyms * 16 > len(buf) or stroff + strsize > len(buf):
                raise MachOError("symbol table of %s outside the file" % img.name)
            strtab = buf[stroff:stroff + strsize]
            for i in range(nsyms):
                strx, ntype, _sect, _desc, value = struct.unpack_from("<IBBHQ", buf, symoff + 16 * i)
                if ntype & N_STAB:
                    continue
                img.symbols.append((_cstr(strtab, strx), ntype, value))


class KernelCollection:
    def __init__(self, data):
        self.buf = bytes(data)
        self.images = []
        self._load()
        self._addr_index = None

    @classmethod
    def open(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def _load(self):
        buf = self.buf
        if len(buf) < 4:
            raise MachOError("file too small")
        if struct.unpack_from(">I", buf, 0)[0] == FAT_MAGIC:
            raise MachOError("fat file: extract the slice first (lipo -thin x86_64)")
        _cpu, filetype, _n, _s = _parse_header(buf, 0)
        if filetype != MH_FILESET:
            # A plain kext/kernel binary: treat it as a one-image collection.
            img = Image("(main)", 0, 0)
            _parse_image(buf, img)
            self.images.append(img)
            self.top = img
            return
        top = Image("(collection)", 0, 0)
        _parse_image(buf, top)
        self.top = top
        for cmd, p, _size in _load_commands(buf, 0):
            if cmd != LC_FILESET_ENTRY:
                continue
            vmaddr, fileoff, name_off = struct.unpack_from("<QQI", buf, p + 8)
            img = Image(_cstr(buf, p + name_off), vmaddr, fileoff)
            _parse_image(buf, img)
            self.images.append(img)

    @property
    def base(self):
        segs = self.top.segments or [s for i in self.images for s in i.segments]
        addrs = [s[1] for s in segs if s[2]]
        return min(addrs) if addrs else 0

    def image(self, name):
        for img in self.images:
            if img.name == name:
                return img
        raise MachOError("no image %r in the collection (try: kcsyms.py kexts)" % name)

    def vm_to_off(self, addr):
        for img in [self.top] + self.images:
            for _n, vmaddr, vmsize, fileoff, filesize in img.segments:
                if vmaddr <= addr < vmaddr + min(vmsize, filesize):
                    return fileoff + (addr - vmaddr)
        return None

    def defined(self):
        for img in self.images:
            for name, ntype, value in img.symbols:
                if (ntype & N_TYPE) == N_SECT:
                    yield img, name, ntype, value

    def lookup(self, addr):
        if self._addr_index is None:
            pairs = sorted({(v, n) for _i, n, _t, v in self.defined()})
            self._addr_index = ([p[0] for p in pairs], [p[1] for p in pairs])
        addrs, names = self._addr_index
        i = bisect.bisect_left(addrs, addr)
        if i < len(addrs) and addrs[i] == addr:
            return names[i]
        return None

    def decode_pointer(self, raw):
        """Resolves a vtable entry to a virtual address.

        Kernel collections use chained fixups (DYLD_CHAINED_PTR_64_KERNEL_CACHE and
        DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE): bits 0..29 hold the target as an offset
        from the collection base, bits 30..31 the cache level. A value that already
        looks like a kernel address is returned unchanged; 0 means an empty slot.
        """
        if raw == 0:
            return 0
        if raw >> 48 == 0xFFFF:
            return raw
        return self.base + (raw & 0x3FFFFFFF)

    def vtable(self, cls, bundle=None):
        sym = "__ZTV%d%s" % (len(cls), cls)
        hits = [(img, v) for img, n, _t, v in self.defined() if n == sym and (bundle is None or img.name == bundle)]
        if not hits:
            raise MachOError("symbol %s not found%s" % (sym, " in " + bundle if bundle else ""))
        img, start = hits[0]
        # The end of the vtable is the next symbol in the same image.
        later = sorted(v for _i, _n, _t, v in self.defined() if _i is img and v > start)
        end = later[0] if later else start + 0x4000
        off = self.vm_to_off(start)
        if off is None:
            raise MachOError("vtable %s at 0x%x is not backed by file data" % (sym, start))
        slots = []
        # Itanium ABI: offset-to-top and RTTI first; the address point is +16.
        for slot_addr in range(start + 16, end, 8):
            pos = off + (slot_addr - start)
            if pos + 8 > len(self.buf):
                break
            raw = struct.unpack_from("<Q", self.buf, pos)[0]
            target = self.decode_pointer(raw)
            if target == 0:      # vtables end with a zero word in kernel collections
                break
            slots.append((slot_addr - start - 16, target, self.lookup(target)))
        return img, slots


def _sym_kind(ntype):
    t = ntype & N_TYPE
    if t == N_UNDF:
        return "U"
    return "T" if ntype & N_EXT else "t"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("kexts")
    p.add_argument("kc", nargs="?", default=DEFAULT_KC)
    p = sub.add_parser("symbols")
    p.add_argument("kc", nargs="?", default=DEFAULT_KC)
    p.add_argument("-k", "--kext", action="append")
    p.add_argument("--exported", action="store_true", help="only defined external symbols")
    p.add_argument("--undefined", action="store_true", help="only imports")
    p = sub.add_parser("exports", help="defined external symbols, one per line (for check-kext.sh)")
    p.add_argument("kc", nargs="?", default=DEFAULT_KC)
    p.add_argument("-k", "--kext", action="append")
    p = sub.add_parser("vtable")
    p.add_argument("kc_or_class")
    p.add_argument("cls", nargs="?")
    p.add_argument("-k", "--kext")
    a = ap.parse_args(argv)

    try:
        if a.cmd == "vtable":
            kc_path, cls = (a.kc_or_class, a.cls) if a.cls else (DEFAULT_KC, a.kc_or_class)
            kc = KernelCollection.open(kc_path)
            img, slots = kc.vtable(cls, a.kext)
            print("# %s vtable in %s, offsets from the address point" % (cls, img.name))
            for off, target, name in slots:
                print("0x%04x  0x%016x  %s" % (off, target, name or ("-" if target == 0 else "?")))
            return 0
        kc = KernelCollection.open(a.kc)
        if a.cmd == "kexts":
            for img in kc.images:
                print("0x%016x  %6d syms  %s" % (img.vmaddr, len(img.symbols), img.name))
            return 0
        wanted = set(a.kext or [])
        for img in kc.images:
            if wanted and img.name not in wanted:
                continue
            for name, ntype, value in img.symbols:
                kind = _sym_kind(ntype)
                if a.cmd == "exports":
                    if kind == "T":
                        print(name)
                    continue
                if a.exported and kind != "T":
                    continue
                if a.undefined and kind != "U":
                    continue
                print("%s %016x %s %s" % (kind, value, img.name, name))
        return 0
    except (MachOError, OSError) as e:
        print("kcsyms: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
