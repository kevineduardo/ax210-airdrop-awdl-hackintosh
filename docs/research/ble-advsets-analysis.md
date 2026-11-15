# Analysis: `-btlfxadvsets` (crazyhulk's BlueToolFixup 2.7.3) and porting it to macOS 15.8.1

*Engineering notes written during the port (November 2026). Line numbers refer to the tree at the time of writing; the modified files later gained a two-line license notice at the top, so numbers may be off by two.*

Date: 2026-11-10 · Test machine: an AMD Ryzen 5600G desktop, macOS 15.8.1 (24H32), Darwin 24.6.0, x86_64, Intel AX210 (8087:0032).
Loaded kexts: Lilu 1.7.2, IntelBTPatcher 2.4.0, BlueToolFixup 2.7.2 (acidanthera). The boot-args at the time had **neither** `-btlfxa2dpcheck` nor `-btlfxadvsets`.

Sources analyzed:
- a local checkout of crazyhulk/BlueToolFixup-FixA2DP-BLE@0fb661b (release `v2.7.3-advsets-0fb661b`)
- a local checkout of acidanthera/BrcmPatchRAM@6538902 (master, "Bump version")
- `/usr/sbin/bluetoothd` (this is where it lives on 15.8.1; it does **not** exist in `/usr/libexec`). Universal binary `x86_64 + arm64e`, 21 917 136 bytes, SHA-256 `52743bab…0bb14`. x86_64 slice: offset 0x4000, size 0xa89bb0. `__TEXT` vmaddr 0x100000000 = file offset 0 (VA = offset + 0x100000000).

Tools: `lipo -info`, `file`, `xxd` and small ad-hoc Perl scripts (slice extraction, masked search, RIP-relative xrefs, `E8` calls). No otool/Python.

> **Methodology note:** Lilu applies user-space patches inside `cs_validate_page`, modifying the page in the *page cache* (UBC). Reading the file while `bluetoothd` is running therefore returns resident pages **already patched** by BlueToolFixup 2.7.2. Confirmed: at 0x1001b3a59 there are 8×`90` (the upstream patch `kBadChipsetCheck15_4` = `cmp ecx,5999; ja` → NOPs) and at 0x1000cd7f4 `81 FA 5C 0A 00 00 EB` (vendor patch `74→EB`). None of the advsets regions is touched by the current patches, so the bytes below are Apple's originals.

---

## 1. Code: where it is and how it differs from upstream

Only one file changed: `BrcmPatchRAM/BlueToolFixup.cpp` (`diff -rq` shows nothing else; Changelog and version are the same. The upstream master pbxproj already says `CURRENT_PROJECT_VERSION = 2.7.3`, but the latest upstream *release* is 2.7.2 from 2026-03-20).

Fork changes (vs. acidanthera master):

| Block | Content |
|---|---|
| `kFastConnectA2DPGate*14_0` / `*12_0` + masks | `-btlfxa2dpcheck` patch (from hexxyan's `fix-a2dp-fastconnect` branch = PR #38) |
| `kSpecAdvSetsCap*26`, `kExtScanCap*26`, `kLeEventMask*26`, `kAdvSetAddr*26`, `kExtCreateConn*26`, `kAdvSetAddrResend*26` | the six `-btlfxadvsets` patches (PR #40) |
| `searchAndPatchWithMask` template | fixes a bug: it passed `replaceSize` as the size of the replace mask instead of `replaceMaskSize` (harmless when they are equal) |
| `searchAndPatchNamedWithMask` | new helper that does `SYSLOG("[a2dpcheck] patched %s")` |
| `patched_cs_validate_page` | `bluetoothd` branch: applies a2dpcheck (picks the pattern by `getKernelVersion()` ≥ Sonoma / ≥ Monterey) and the six advsets patches **with no version check and no mask** (exact `KernelPatcher::findAndReplace`) |
| `pluginStart` | reads `-btlfxa2dpcheck` and `-btlfxadvsets`, logs a message |

Notes on the mechanism: patches are applied page by page (`PAGE_SIZE` = 4 KiB), so a pattern that crosses a page boundary never matches; `findAndReplace` replaces the first occurrence per page. The advsets patterns include `rel32`/`disp32` displacements and structure offsets, so by construction they are only valid for one build (25E253).

### a2dpcheck (`-btlfxa2dpcheck`)
- Target: the gate in the A2DP Fast-Connect path that depends on `HCI_Read_Clock` ("Failed to read BT clock"); Intel controllers fail it and `bluetoothd` aborts audio (AirPods "connected but no sound").
- 14+: `45 85 F6 74 25 48 8D 05 ?? ?? ?? ?? 48 8B 18` → `45 31 F6 EB 25 …` (`test r14d,r14d; je` → `xor r14d,r14d; jmp`).
- 12/13: `85 DB 74 25 48 8D 05 ?? ?? ?? ?? 48 8B 18` → `31 DB EB 25 …`.
- **On 15.8.1 it matches twice** (0x100229d2d and 0x1003ea091, different pages → both would be patched). Update: 0x1003ea091 is the intended gate ("Failed to read BT clock"); 0x100229d2d is a false positive in the PCIe firmware loader ("Unable to get firmware image file buffer"). Details and a proposed more specific pattern in [ble-advsets-sequoia-changes.md](ble-advsets-sequoia-changes.md).

---

## 2. Does upstream already include `-btlfxa2dpcheck`?

**No.** Checked in the local code and through the GitHub API (read-only):
- acidanthera/BrcmPatchRAM PR **#38** (hexxyan, `fix-a2dp-fastconnect`, opened 2026-04-07): **open, not merged**.
- PR **#40** (crazyhulk, `pr/btlfx-advsets`, 2026-11-09): open, not merged.
- PR #39 (trueToastedCode, sleep/wake for BrcmPatchRAM3 + native stack patches): open.
- Latest upstream release: **2.7.2** (2026-03-20). Master @ 6538902 only bumps to 2.7.3, without a2dpcheck. No occurrence of `a2dp`/`fastconnect` in the acidanthera checkout.

---

## 3. The six advsets patches and their state in `bluetoothd` 15.8.1

Context (26.4.1 and 15.8.1 have the same logic): `bluetoothd` decides "BT chip support for BT Spec advertisingsets" through a virtual capability method that only looks at the chipset enum; chipset 0 (`THIRD_PARTY_DONGLE`) is never in the list. Without it, `LE_InitMultipleAdvertising` falls back to the VSC (vendor) path, which also requires chipset ≠ 0 → status 0x98 (=152) → "Stack is not started" → no BLE advertising → the iPhone does not see the Mac.

Captured logs (not published) from the test machine confirm this state: `enableBTSpecAdvertisingSets=0 enableExtendedAdvertising=0 enableExtendedScanning=0 enableExtendedScanHCI=0 maxAdvHandles=4`.

**Searching the x86_64 slice of 15.8.1 for the exact 26.4.1 patterns: 0/6 match.** But all six sites exist with the same structure and were re-derived (each with exactly 1 occurrence, within a single 4 KiB page):

| # | What it bypasses | 26.4.1 (find → replace) | 15.8.1: site and original bytes | 15.8.1 replace |
|---|---|---|---|---|
| 1 | "BT Spec advertising sets" capability (vtable) → force `true` | `48 8B 08 48 89 C7 FF 91 C0 09 00 00 41 89 C6` → `… B0 01 90 90 90 90 …` (`call [rcx+0x9c0]` → `mov al,1; nop×4`) | **0x1003a657c**: `48 8B 08 48 89 C7 FF 91 60 08 00 00 41 89 C6`, slot **+0x860**; followed by the log "BT chip support for BT Spec advertisingsets : %s" (xref 0x1003a65d4) and "Overriding DisableSpecSets=%d" (0x1003a667e) | `48 8B 08 48 89 C7 B0 01 90 90 90 90 41 89 C6` |
| 2 | "extended scanning" capability → `true` (with extended advertising on, the controller refuses legacy scan with Command Disallowed) | `… FF 91 A8 09 00 00 …` → same replace as #1 | **0x1003a2e46**: `48 8B 08 48 89 C7 FF 91 48 08 00 00 41 89 C6`, slot **+0x848**; log "BT chip support for extended scans : %s" (0x1003a2e9e) | `48 8B 08 48 89 C7 B0 01 90 90 90 90 41 89 C6` |
| 3 | LE Event Mask: for chipset 0 it uses 0x1f (legacy events only) → remove the `cmove` to use the full mask | `85 C0 48 8D 05 A0 F2 8F 00 48 8D 15 A1 F2 8F 00 48 0F 44 D0` → last 4 bytes `0F 1F 40 00` | **0x100144a37**: `85 C0 48 8D 05 41 11 70 00 48 8D 15 42 11 70 00 48 0F 44 D0` (rax→0x100845b81 = `0x1f`; rdx→0x100845b89 = `0x000bf3ff3f86fa7f`); just before "OI_HCICMD_LE_SetEventMask returned" (0x100144a77) | `… 0F 1F 40 00` |
| 4 | For chipset ≤ 3, `bluetoothd` disables "LE Set Advertising Set Random Address" → skip only that call | `83 B8 34 03 00 00 03 77 2A 31 FF E8 A2 7D CE FF` → `… EB 07 …` | **0x10049d49e**: `83 B8 F8 02 00 00 03 77 23 31 FF E8 59 AB D2 FF` (`cmp dword [rax+0x2f8],3; ja +0x23; xor edi,edi; call setter@0x1001c8007`). The setter writes global 0x100a44de0, which is exactly the one tested before sending 0x2035 in #6 | `83 B8 F8 02 00 00 03 EB 07 31 FF E8 59 AB D2 FF` |
| 5 | Use LE **Extended** Create Connection (legacy 0x200d gets Command Disallowed and `bluetoothd` restarts itself: "Exceed max allowed consecutive command disallowed events") | `0F B6 BD 5E FF FF FF E8 2F B0 CF FF` → `BF 01 00 00 00 66 90 E8 …` | **0x10049d0ee**: `0F B6 BD 5E FF FF FF E8 00 CF D3 FF`, calls setter 0x1001d9ffa, which writes `gEnableExtendedCreateConnectionCommand` (global 0x100a588ca, read in the config dump at 0x1001da297). Same local `[rbp-0xa2]` as in 26.4.1 | `BF 01 00 00 00 66 90 E8 00 CF D3 FF` |
| 6 | Always resend the advertising set random address before enable (the Intel firmware forgets it when parameters are rewritten → enable fails with 0x12) | `8B 54 3A 03 33 94 08 11 0D 00 00 44 09 C2 0F 84 80 00 00 00` → `… 66 0F 1F 44 00 00` (je near → NOP6) | **0x1001ca5ac**: `8B 74 3E 03 33 B4 11 11 0D 00 00 44 09 C6 74 7F`, same cache offset `+0xd11`, but different registers and a **short `je`** (2 bytes). Next: `cmp byte [rip→0x100a44de0],0; je` and `call 0x1001c71ff` (wrapper for `LE_SetAdvertisingSetRandomAddress`) | `8B 74 3E 03 33 B4 11 11 0D 00 00 44 09 C6 66 90` |

Observations:
- The vtable slots shifted by exactly **+0x160** between 15.8.1 and 26.4.1 (0x860→0x9c0, 0x848→0x9a8), keeping the 0x18 distance between them. This is strong confirmation that they are the same methods.
- The offset of the chipset field in the object changed (0x2f8 → 0x334), as did all `rel32`/`disp32` values; the logic is identical.
- **The extended advertising/scanning path exists in full on 15.8.1**: `OI_HCI_CMD_LE_SetExtendedAdvertising{Parameters,Data,Enable}`, `LE_SetAdvertisingSetRandomAddress`, `LE_ClearAdvertisingSets`, `_HCIEventHandler_ExtendedAdvertisingReport`, `gEnableExtendedCreateConnectionCommand(V2)`, `DisableSpecSets`, "enableBTSpecAdvertisingSets cannot be false if enableExtendedAdvertising is true", `LE_InitMultipleAdvertising failed with status %!`, `THIRD_PARTY_DONGLE`, "Stack is not started, updateAdvertisement returning", `BTVCLinkAgent leSetExtended*` methods. Nothing structural is missing.

Difficulty of re-deriving: **low**. It was done in this analysis with only Perl/xxd, anchoring on log strings (RIP-relative xrefs) and `E8` calls to global setters.

---

## 4. Recommendation

### Feasibility
Porting the six patches to 15.8.1 (24H32) is **technically feasible**: all sites were found, they are unique and do not cross pages. The kext would need to be built (Xcode + Lilu SDK, on another machine or after installing Xcode).

**But note the "AirDrop" goal:** crazyhulk's release says AirDrop also needs the AWDL build of AirportItlwm, and that build is **Tahoe only** (the commit explains that in macOS 26 the AWDL stack moved into IO80211Family; on 15.x AWDL lives inside the Broadcom Wi-Fi driver and itlwm does not implement it). On 15.8.1, with advsets ported, the most likely gain is BLE advertising/scan (Continuity, BLE discovery, fewer "Stack is not started", extended LE connection), **but not working AirDrop**. For real AirDrop the path is Tahoe 26.4.1 + BlueToolFixup advsets + AirportItlwm AWDL (both experimental), or a native Broadcom card.

> Status update: the AWDL analysis that followed found that on Sequoia the AWDL stack is also in IO80211Family (see [awdl-sequoia-port-analysis.md](awdl-sequoia-port-analysis.md)). AirportItlwm AWDL was ported to Sequoia, the advsets port was built and confirmed active on 15.8.1, and AirDrop iPhone→Mac and Mac→iPhone worked on Sequoia 15.8.1.

### Porting steps (without touching the system; everything via the kext)
1. Fork `0fb661b`; add per-version tables (below) with the bytes from section 3.
2. Build DEBUG; install on a **test** EFI (keep a known-good EFI), add `-btlfxadvsets` (and optionally `-btlfxdbg`).
3. Verify: `log show --predicate 'sender == "BlueToolFixup" OR process == "kernel"' | grep advsets` → 6× "patched"; in `bluetoothd`: "BT chip support for BT Spec advertisingsets : enabled", `enableBTSpecAdvertisingSets=1 … enableExtendedScanning=1`, no `LE_InitMultipleAdvertising failed … 152`.
4. Test: iPhone seeing the Mac (Continuity/Handoff), AirPods (A2DP + `-btlfxa2dpcheck`), BLE scan, sleep/wake cycle, and look for "Exceed max allowed consecutive command disallowed" / `bluetoothd` restarts.
5. Introduce the patches gradually if something fails (1+2+3 first; then 5; then 4+6, which are specific to the Intel firmware).

### Risks
- Low for the system (in-memory patch, SSV intact; removing the boot-arg reverts it). Worst case: `bluetoothd` in a crash loop → Bluetooth unavailable until the boot-arg is removed.
- AX210 firmware behavior with extended advertising on 15.x has not been tested by anyone; interaction with IntelBTPatcher (which also intercepts HCI LE commands) not verified.
- Any 15.x update changes `rel32`/offsets → the patches simply stop matching (safe failure, nothing is corrupted).
- Enabling only #1 without #2/#3/#5 is worse than nothing (legacy scan refused, Command Disallowed in a row).

### Per-version patterns (keyed by kernel), keeping 26.x
Sequoia = `KernelVersion::Sequoia` (24), Tahoe = `KernelVersion::Tahoe` (25). Proposal:

```cpp
struct BtdPatch { const char *name; const uint8_t *find, *mask, *repl; size_t size; };

// 15.8.1 (24H32) — own slots/offsets; masks where safe
static const BtdPatch kAdvSets24[] = {
  {"BT Spec adv sets cap", k24_1_find, nullptr, kCapPatched /*B0 01 90x4*/, 15}, // FF 91 60 08
  {"ext scan cap",         k24_2_find, nullptr, kCapPatched, 15},                // FF 91 48 08
  {"LE event mask",        k24_3_find, k24_3_mask /*wildcard disp32*/, k24_3_repl, 20},
  {"adv set rnd addr",     k24_4_find, k24_4_mask /*83 B8 ?? ?? 00 00 03 77 ?? 31 FF E8*/, k24_4_repl, 12},
  {"ext create conn",      k24_5_find, k24_5_mask /*0F B6 BD ?? FF FF FF E8 ?? ?? ?? FF 44 0F B6 BD*/, k24_5_repl, 16},
  {"adv addr resend",      k24_6_find, k24_6_mask /*8B ?? ?? 03 33 ?? ?? 11 0D 00 00 44 09 ?? 74*/, k24_6_repl, 16},
};
static const BtdPatch kAdvSets25[] = { /* crazyhulk's current six (26.4.1) */ };

static const BtdPatch *advSets; static size_t advSetsCount;
// pluginStart:
if (getKernelVersion() == KernelVersion::Tahoe)        { advSets = kAdvSets25; advSetsCount = arrsize(kAdvSets25); }
else if (getKernelVersion() == KernelVersion::Sequoia) { advSets = kAdvSets24; advSetsCount = arrsize(kAdvSets24); }
// optionally also filter by getKernelMinorVersion() if future 15.x/26.x change
// cs_validate_page: for each p in advSets -> findAndReplaceWithMask(...); log "[advsets] patched %s"
```

Practical rules:
- The masked patterns above were verified to be **unique** on 15.8.1 (1 occurrence each); #1 and #2 cannot be masked (the slot is the only distinguishing part: with the slot masked there are 22 occurrences) → keep them exact per version.
- For robustness against minor updates, mask `rel32`/`disp32` and field offsets, but include enough context bytes for uniqueness (validate with a masked search on each build's binary).
- Count the applied patches and, once `bluetoothd` has started, log a warning if fewer than 6 (partial application is worse than none). Ideally "all or nothing", e.g. only enable #3–#6 after #1 has matched.
- Fix the log prefix (`searchAndPatchNamedWithMask` always says "[a2dpcheck]").
- Also propose upstream that `-btlfxa2dpcheck` (PR #38) be accepted first; advsets (PR #40) depends on it in the branch history.

---

## Appendix: useful addresses in `bluetoothd` 15.8.1 x86_64

| Item | VA |
|---|---|
| log "BT chip support for BT Spec advertisingsets" (string / xref) | 0x10091b2c0 / 0x1003a65d4 |
| log "Overriding DisableSpecSets=%d" (xref) | 0x1003a667e |
| log "BT chip support for extended scans" (xref) | 0x1003a2e9e |
| log "enableBTSpecAdvertisingSets=%d …" (xref) | 0x10049d265 |
| log "LE_InitMultipleAdvertising failed with status" (xref) | 0x1001e87de |
| log "Exceed max allowed consecutive command disallowed" (xref) | 0x10049e802 |
| `THIRD_PARTY_DONGLE` (xref) | 0x10031ad6b |
| setter `gEnableExtendedCreateConnectionCommand` / global | 0x1001d9ffa / 0x100a588ca |
| setter flag "Set Adv Set Random Address" / global | 0x1001c8007 / 0x100a44de0 |
| wrapper `LE_SetAdvertisingSetRandomAddress` | 0x1001c71ff |
| A2DP gate (2 sites) | 0x100229d2d, 0x1003ea091 |

The analysis scripts were temporary and are not published; `tools/advsets/verify-advsets.pl` re-checks the final patch table against a `bluetoothd` binary.
