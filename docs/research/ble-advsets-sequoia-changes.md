# `-btlfxadvsets` port to macOS 15.8.1 (Sequoia): changes

*Engineering notes written during the port (November 2026). Line numbers refer to the tree at the time of writing; the modified files later gained a two-line license notice at the top, so numbers may be off by two.*

Date: 2026-11-10 · Tree: `driver/BlueToolFixup` (a copy of crazyhulk/BlueToolFixup-FixA2DP-BLE @ 0fb661b)
Diff: generated with `diff -ruN -x Lilu.kext -x MacKernelSDK <crazyhulk/BlueToolFixup-FixA2DP-BLE@0fb661b checkout> driver/BlueToolFixup`
Verifier: `tools/advsets/verify-advsets.pl`

**Status: not compiled** (Xcode is not installed yet). The patterns and the C arrays were verified against the binary; the C++ code was only reviewed by hand.

> Status update: the code was later built and tested. The advsets patches were confirmed active on 15.8.1, and AirDrop iPhone→Mac and Mac→iPhone worked on Sequoia 15.8.1 together with the AirportItlwm AWDL port.

---

## Files changed

### `BrcmPatchRAM/BlueToolFixup.cpp`
1. **`#include <mach-o/loader.h>`**: explicit, for `mach_header_64`, `LC_UUID`, `uuid_command` (Lilu already includes it via `kern_mach.hpp`, but it is better not to depend on that).
2. **a2dpcheck**: behavior unchanged. Only a comment was added documenting the double match on 15.8.1 and the proposed fix (see below).
3. **26.4.1 table**: the `k*26` arrays are byte-for-byte intact (the diff removes none of their lines). They are applied as before (exact, no mask, `count=0`), now through the `kAdvSetsPatches26` table.
4. **Sequoia table (`k*24`)**: the six new patterns:

   | # | Log name | Type | Find (`??` = mask 00) | Bytes changed | 15.8.1 VA |
   |---|---|---|---|---|---|
   | 1 | BT Spec advertising sets capability | exact | `48 8B 08 48 89 C7 FF 91 60 08 00 00 41 89 C6` | `FF 91 60 08 00 00` → `B0 01 90 90 90 90` | 0x1003a657c |
   | 2 | extended scanning capability | exact | `48 8B 08 48 89 C7 FF 91 48 08 00 00 41 89 C6` | same | 0x1003a2e46 |
   | 3 | LE event mask | mask | `85 C0 48 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 0F 44 D0` | `48 0F 44 D0` → `0F 1F 40 00` | 0x100144a37 |
   | 4 | advertising set random address | mask | `83 B8 ?? ?? 00 00 03 77 ?? 31 FF E8` | `77 23` → `EB 07` | 0x10049d49e |
   | 5 | extended create connection | mask | `0F B6 BD ?? FF FF FF E8 ?? ?? ?? FF 44 0F B6 BD` | `0F B6 BD 5E FF FF FF` → `BF 01 00 00 00 66 90` | 0x10049d0ee |
   | 6 | advertising set address resend | mask | `8B ?? ?? 03 33 ?? ?? 11 0D 00 00 44 09 ?? 74 ??` | `74 7F` → `66 90` | 0x1001ca5ac |

   In the masked patterns, the *replace mask* only covers the bytes that change. Wildcard bytes (`rel32`/`disp32`, chipset field offset) are left as they are. This is the same semantics upstream already uses in `kSkipAddressCheck*`.
5. **New structures**: `AdvSetsPatch` (name, find, findMask, replace, replaceMask, size), `AdvSetsTable` (name, `KernelVersion`, minor range, UUID, patches) and `kAdvSetsTables[]`:
   - `macOS 15.8.1 (24H32)`: `KernelVersion::Sequoia`, minor 6..6 (Darwin 24.6.x), UUID `9D4727A2-1FC2-3777-BE26-7BAD0ED65EF0`.
   - `macOS 26.4.1 (25E253)`: `KernelVersion::Tahoe`, any minor, UUID `nullptr` (unknown).
   - `static_assert` on all find/mask/replace/replaceMask sizes.
6. **Selection** (`selectAdvSetsTable`, called from `pluginStart` when `-btlfxadvsets` is set): picks the table by `getKernelVersion()`/`getKernelMinorVersion()` and logs which one was selected.
7. **All-or-nothing** (`patchAdvSets`, called in the `bluetoothd` branch of `patched_cs_validate_page`, after the existing patches):
   - Patches are applied page by page as `bluetoothd` pages in. A page validated without the patch cannot be patched later. And the whole binary cannot be read inside `cs_validate_page`, because the VM object lock is held and the read would block (deadlock). So the decision is made before any code page, on the Mach-O header page (dyld reads it before executing any `bluetoothd` code; on 15.8.1 the `LC_UUID` is at offset 0xcd0, inside the first page).
   - States: `Pending` → x86_64 header seen → `Armed` if the `LC_UUID` equals the table's (the six patterns were verified offline and match exactly once in that build, so all of them are applied), otherwise `Rejected` (none is applied; the log shows the UUID found).
   - If a page containing a patch site shows up before the header, the state becomes `Aborted` and nothing is applied on this boot.
   - `Armed`: applies and logs `[advsets] patched <name> in /usr/sbin/bluetoothd` (same text as the original build) and, on the 6th, `[advsets] all 6 patches applied (table …)`.
   - Tables without a UUID (26.4.1) or the new boot-arg `-btlfxadvsetsany`: `Unverified` mode, the same as the old behavior (per page). Logs `unverified build: N of 6 patches applied so far` so a partial application is visible right away.
   - State uses `__atomic_*` (page-ins may happen in parallel).
8. Removed the six inline `findAndReplace` calls (replaced by `patchAdvSets`). In practice, the 26.4.1 patterns are no longer tried outside Tahoe; before, they were tried on every version and simply did not match.

### `Changelog.md`
Entry `v2.7.3 (local port, unreleased)` with the three changes.

Nothing else was touched: Info.plist, pbxproj, README, Lilu.kext and MacKernelSDK are unchanged.

---

## Expected log lines (15.8.1, `-btlfxadvsets`)
```
btlfx: [advsets] BT Spec LE advertising sets for third-party controllers enabled
btlfx: [advsets] selected table macOS 15.8.1 (24H32) for Darwin 24.6 (waiting for bluetoothd UUID check)
btlfx: [advsets] /usr/sbin/bluetoothd UUID matches table macOS 15.8.1 (24H32), applying all 6 patches
btlfx: [advsets] patched LE event mask in /usr/sbin/bluetoothd           (order = page-in order)
… (6× "[advsets] patched …")
btlfx: [advsets] all 6 patches applied (table macOS 15.8.1 (24H32))
```
Possible failures: `… not verified for table …, applying none`, `… site seen … before the Mach-O header was checked, applying none`, `no patch table for Darwin X.Y`. In `bluetoothd`, the expected result is `BT chip support for BT Spec advertisingsets : enabled` and `enableBTSpecAdvertisingSets=1 … enableExtendedScanning=1`.

To view in the log: `log show --last boot --predicate 'eventMessage CONTAINS "[advsets]"'` (with the DEBUG build and `-btlfxdbg` if the RELEASE SYSLOG does not show up).

---

## a2dpcheck: double match on 15.8.1

The 14+ pattern `45 85 F6 74 25 48 8D 05 ?? ?? ?? ?? 48 8B 18` matches in two places, on different pages, and both are patched:

| VA | Context | Conclusion |
|---|---|---|
| **0x1003ea091** | `mov esi, 0x10` (os_log ERROR) then `call 0x1007a3d36`, an outlined function that logs **"Failed to read BT clock"**. In the same area: "Fast Connect A2DP Report Connection attempt" (0x1003e77c2) and "Fast Connect A2DP Setup Complete result" | **Intended**: this is the Read Clock gate in Fast-Connect A2DP |
| **0x100229d2d** | r14d = result of `call [rax+0x1c8]`; `mov esi, 0x11` (os_log FAULT) then the outlined function that logs **"Unable to get firmware image file buffer"**. In the same area: ".bin", "Firmware Filename: ", "PCIe successfully transferred firmware image file" | **Not intended**: this is the PCIe firmware loader. The patch forces "success" if the firmware buffer fails |

Impact: probably none with a USB dongle (that PCIe firmware path should not run), but it is a false positive.
**Proposal (not applied)**: add the log type to the pattern, `48 89 DF BE 10 00 00 00` (`mov rdi, rbx; mov esi, 0x10`). With that, 15.8.1 has a single match (0x1003ea091), verified. The mask and patch keep the same changed bytes. `count=1` is not enough, because Lilu's count is per page and the two sites are on different pages. Before adopting it, the pattern needs to be confirmed on 14.x and 26.x binaries (the author of PR #38 says the short pattern was verified on those versions).

---

## Verification performed (just before finishing)
`perl tools/advsets/verify-advsets.pl /usr/sbin/bluetoothd driver/BlueToolFixup/BrcmPatchRAM/BlueToolFixup.cpp`:
- Reads the x86_64 slice (offset 0x4000) and confirms that `LC_UUID` = `kBluetoothdUuid24H32`.
- Extracts the C arrays from the .cpp itself (find/mask/replace/replaceMask) and confirms a single match for each of the 6, without crossing 4 KiB pages. Simulates the replacement (`d = (d & ~rm) | (r & rm)`) and shows the bytes before and after (all as expected). Result: `ALL SIX UNIQUE - OK`.
- The 26.4.1 patterns give 0 matches on 15.8.1 (expected).
- Caveat about in-memory patching: Lilu modifies pages in the UBC, so reading the file may show already-patched bytes. The script also searches for the patched form (0 hits now, because `-btlfxadvsets` is not active). The pages read that were patched by 2.7.2 (chipset 15_4, vendor) do not overlap any advsets site.

## What could not be verified
1. **Compilation**: no Xcode. Possible trouble spots: `static_assert`/`auto` in C++14 (`gnu++14` in the target), visibility of `CPU_TYPE_X86_64` via `mach/machine.h`, and clang analyzer warnings (CI fails on any warning).
2. **Exact mask semantics in Lilu 1.7.2** (`findAndReplaceWithMask`/`findPattern`): assumed `(data & mask) == (find & mask)` and `data = (data & ~rmask) | (replace & rmask)`. This is the semantics the upstream masked patches already use. The wildcard bytes are 0x00 in both find and replace, so it works under both plausible interpretations of find. Lilu's `.cpp` was not read (the SDK only ships headers).
3. **Page-in order**: that the Mach-O header page passes through `cs_validate_page` before the code pages with patches. If it does not, the result is safe (`Aborted`, nothing applied, explicit log), but advsets does not turn on. If that happens, the fallback is `-btlfxadvsetsany`.
4. **Runtime behavior** with the AX210 on 15.x (Intel firmware with extended advertising, interaction with IntelBTPatcher, sleep/wake, AirPods).
5. **26.4.1 UUID**: unknown, so Tahoe stays in per-page mode (same as the original). To make it strict: `perl tools/advsets/verify-advsets.pl <bluetoothd 26.4.1>` and fill in the UUID in the table.
6. Other 15.x builds (15.6, 15.7.x) were not verified. With a different UUID, nothing is applied (safe). With `-btlfxadvsetsany`, the masked patterns may match, but #1 and #2 (exact slots) may not.

> Status update: the code was later built and the advsets patches were confirmed active on 15.8.1.

Reminder at the time of writing: even with this working, AirDrop on 15.x still needs AWDL, which AirportItlwm only implemented (experimentally) on Tahoe.

> Status update: AWDL was later ported to Sequoia (see [awdl-sequoia-port-changes.md](awdl-sequoia-port-changes.md)), and AirDrop iPhone→Mac and Mac→iPhone worked on Sequoia 15.8.1.

---

## Build command (once Xcode 16.4 is installed)
```sh
cd driver/BlueToolFixup
sudo xcode-select -s /Applications/Xcode.app   # once, if needed
xcodebuild -jobs 1 -project BrcmPatchRAM.xcodeproj -target BlueToolFixup -configuration Debug
xcodebuild -jobs 1 -project BrcmPatchRAM.xcodeproj -target BlueToolFixup -configuration Release
# output: build/Debug/BlueToolFixup.kext and build/Release/BlueToolFixup.kext
```
(CI uses `-target Package`, which also builds BrcmPatchRAM and packages everything; for this test only the `BlueToolFixup` target matters, and it uses `Lilu.kext/Contents/Resources` and `MacKernelSDK` in the project folder.)
Static analysis as in CI: `xcodebuild analyze -quiet -project BrcmPatchRAM.xcodeproj -target BlueToolFixup -configuration Debug`.
Then: `perl tools/advsets/verify-advsets.pl` (should still report OK), install on a test EFI with `-btlfxadvsets` (and optionally `-btlfxa2dpcheck -btlfxdbg`), and confirm the log lines above.
