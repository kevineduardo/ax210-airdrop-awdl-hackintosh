# macOS driver development notes

Practical notes from this project for anyone writing or porting macOS kernel extensions, especially network drivers for hackintosh hardware. Most of it applies to any kext.

## Legal boundaries we follow

We want this repo to be safe to fork and redistribute. The rules:

- Ship no Apple code. No kexts, kernel collections, `bluetoothd`, firmware, frameworks, or headers copied from Apple SDKs or KDKs. The build uses [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK), fetched at build time.
- Reverse engineering happens on your own licensed copy of macOS, for interoperability: to make our driver talk to Apple's IO80211Family. Run the tools on your machine; keep their raw output (symbol dumps, disassembly, decompiled code) on your machine.
- Commit only what interoperability needs: class names, method names and their slot order, struct sizes and field offsets the driver must use, and a note of how they were verified. Write that in your own words and code. Never paste decompiled Apple code.
- Byte patterns for in-memory patches (BlueToolFixup) are the minimum needed to find a location. They are verified on your own copy, and the binary itself is never committed.
- Respect the licenses of the open-source code you build on (GPL-2.0 for itlwm and BrcmPatchRAM: changed files carry a notice, derived kexts stay GPL-2.0, sources are published with every binary).
- Apple's macOS license only allows installing macOS on Apple-branded hardware. We don't give advice on getting around Apple's license or activation, and the docs don't explain how to obtain macOS.

When something can only be done with Apple software, the docs here describe the steps for you to run on your own Mac instead of providing the result.

## Toolbox

| Tool | Use |
|---|---|
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | Kernel headers and libraries for building kexts with a normal Xcode |
| [Lilu](https://github.com/acidanthera/Lilu) | Patching kexts and user-space binaries at load time (BlueToolFixup is a Lilu plugin) |
| `tools/kc/kcsyms.py` (this repo) | List kexts and symbols in your kernel collection; decode C++ vtables (chained fixups) to check header layouts |
| `tools/check-kext.sh` (this repo) | After every build: one struct layout across all compile units, and every import present in the target kernel |
| [ipsw](https://github.com/blacktop/ipsw) | Extract kexts from a kernel collection, disassemble, dump class info |
| Ghidra / Hopper / IDA | Disassembly when you need to know how Apple's own driver fills a structure |
| `ioreg -l -w0 -r -c <Class>` | Driver properties: we publish every counter as an IORegistry property |
| `log stream --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "awdl"'` | Live kernel logs |
| `kmutil showloaded`, `kextstat` | What is loaded, and which build (UUID) |
| `atos` + dSYM | Turn a panic backtrace into function names and lines (boot with `keepsyms=1`) |
| `tcpdump -i awdl0 -w x.pcap` | What actually goes over AWDL (needs root) |

## Reading C++ classes out of a kernel collection

IOKit is C++ with the Itanium ABI. A vtable symbol `__ZTV<len><Class>` points to two words (offset-to-top, RTTI) followed by the method pointers; the "address point" is +16. In a kernel collection those pointers are chained fixups: the low 30 bits are the target's offset from the collection base. `kcsyms.py vtable <Class>` does this decoding and names each slot from the symbol table:

```sh
tools/kc/kcsyms.py vtable IO80211BufferPool
0x0118  0xffffff8002204cc6  __ZN17IO80211BufferPool9getBufferEv
```

From that you can write a header where each `virtual` method sits at the right slot. Things to remember:

- Inherited slots come first, in the parent's order; a class's new methods follow in declaration order, including `OSMetaClassDeclareReservedUnused` slots.
- A class with no virtual methods of its own ends right after its parent's slots (on 15.8.1, `IO80211Buffer` ends at `OSObject::_RESERVEDOSObject15`). Calling a "virtual" method past the end reads garbage and panics; call the exported function by name instead.
- Pure virtual slots (`___cxa_pure_virtual`) are what a subclass must implement.
- Check every minor release you support. Layouts change within a major version.

## Keep the build honest

Two lessons that cost us boot panics:

1. Stale objects. itlwm includes the net80211 headers with `-isystem`, and clang's `-MMD` dependency files skip system headers. Changing a struct in `ieee80211_var.h` recompiled 3 files and left 38 with the old layout, linked into one kext. Fixes now in the project: `-Xclang -sys-header-deps`, a layout fingerprint symbol (objects built with another layout leave an undefined symbol, so the kext refuses to load), and `tools/check-kext.sh` comparing struct sizes in the DWARF of every compile unit. Details: [research/panic-stale-struct-layout.md](research/panic-stale-struct-layout.md).
2. Debug and Release are different binaries, and a bug can hide in one of them. Test the configuration you ship.

## Make the driver observable

Every counter that helped fix AWDL is published as an IORegistry property and read by `awdlctl show`: TX ok/fail/busy, dwell-start delay histogram, data latency histogram, peer alignment, election role. Publishing counters from day one is cheap, and it turns "AirDrop sometimes fails" into "45 % of dwells start more than 10 ms late".

Put risky features behind boot-args, default off, so a user can always get back to a working system by removing a flag. Add run-time switches (`setProperties`, administrator only) for anything you want to A/B in the same boot.

## Testing on hardware

- One change per boot when bringing something up.
- Record before and after (`scripts/monitor-awdl.sh` writes a timeline per AirDrop attempt).
- Compare a success with a failure from the same build before changing code; the master-election bug was found that way.
- Keep a known-good EFI on a USB stick and write down every EFI you boot, with the kext UUID.

## Where to learn more

- Apple's [IOKit Fundamentals](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/) and [Kernel Programming Guide](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/) (old but still the best overview).
- [XNU sources](https://github.com/apple-oss-distributions/xnu) for the kernel side (APSL).
- [Dortania guides](https://dortania.github.io/) for how kexts are injected on OpenCore.
- The OpenIntelWireless [itlwm](https://github.com/OpenIntelWireless/itlwm) code base, a complete example of porting a BSD Wi-Fi driver to macOS.
