# Analysis of the test 5 panics (AirportItlwm, Sequoia port)

*Engineering notes written during the port (November 2026). Line numbers refer to the tree at the time of writing; the modified files later gained a two-line license notice at the top, so numbers may be off by two.*

Date: 2026-11-11. Kext analysed: Release, UUID `B0FFCFDD-5D6C-3FF0-BE8A-DB3E3D4C5723` (a copy with its dSYM was kept for symbolication).

## Summary

- Both panics have a single cause: the kext was linked from objects compiled against two different versions of `struct ieee80211com`.
  The TX pump change added 120 bytes (0x78) in the middle of the structure, in `ieee80211_var.h`. The incremental build only recompiled 3 files (`ItlIwx.cpp`, `AirportItlwmV2.cpp`, `AirportItlwmAWDLInterface.cpp`). The other 38 (all of net80211, including `ieee80211_node.c`, `ieee80211.c`, `ieee80211_proto.c`) kept the old layout.
- It is not a bug in the pump code, nor a "Release-only" bug (optimizer, uninitialized memory). The Debug build made at the same time had the same defect and would have failed the same way.
- The incremental build failed because the project puts `itl80211/openbsd` in `SYSTEM_HEADER_SEARCH_PATHS` (`-isystem`) and Xcode generates dependencies with `-MMD`, which ignores system headers. A change to `<net80211/ieee80211_var.h>` never forced its includers to recompile.
- Fixed: dependencies now include those headers, there is a layout fingerprint checked at link time, and all 4 builds were redone from scratch. The AWDL stop path was also hardened.

## 1. Symbolication

Kext base: panic B `0xffffff8018098000`, panic A `0xffffff801b098000` (`__TEXT` with vmaddr 0, so offset = address − base).

| Panic | Address | Offset | `atos` (dSYM B0FFCFDD) |
|---|---|---|---|
| B | `0xffffff80180c44c9` | `0x2c4c9` | `ieee80211_reset_scan` (`ieee80211_node.c:832`) |
| B | `0xffffff80180ae393` | | `ieee80211_setmode` (`ieee80211.c:1242`) |
| B | `0xffffff80180c45f5` | | `ieee80211_begin_scan` (`ieee80211_node.c:892`) |
| B | `0xffffff80180fc475` | | `ItlIwx::iwx_init` (`ItlIwx.cpp:11680`) |
| B | `0xffffff8018100506` | | `ItlIwx::iwx_init_task` (`ItlIwx.cpp:14836`) |
| A | `0xffffff801b0fff97` | `0x67f97` | `ItlIwx::iwx_intr` (`ItlIwx.cpp:12970`) |

## 2. Proof of the layout mismatch

`dwarfdump --debug-info` of the dSYM, size of `struct ieee80211com` per compile unit:

```
ieee80211com 0x1b60  38 units (ieee80211_node.c, ieee80211.c, ieee80211_proto.c, ieee80211_input.c, hal_iwm/*, ...)
ieee80211com 0x1bd8   3 units (ItlIwx.cpp, AirportItlwmV2.cpp, AirportItlwmAWDLInterface.cpp)
```

0x1bd8 − 0x1b60 = 0x78 = 120 bytes, exactly the new fields from commit `1a3be28`
(`ic_awdl_dwell_state` 8 + `ic_awdl_roc_delay_hist` 32 + `ic_awdl_roc_start_us` 8 + `ic_awdl_roc_end_us` 8 + `ic_awdl_tx_state_hist` 32 + `ic_awdl_tx_ok_band` 16 + `ic_awdl_tx_fail_band` 16), inserted before `ic_bgscan_timeout`. Everything after them shifts by 0x78:

| Field | Old offset (38 units) | New offset (3 units) |
|---|---|---|
| `ic_scan_count` / `ic_flags` | 0x12d8 / 0x12dc | 0x1350 / 0x1354 |
| `ic_state` | 0x12f8 | 0x1370 |
| `ic_bss` | 0x1350 | 0x13c8 |
| `ic_node_alloc` | 0x1370 | 0x13e8 |

Build log: 1 `CompileC` (only `AirportItlwmAWDLInterface.cpp`). Object timestamps: `ieee80211_node.o` 18:37 (previous full build), `ieee80211_var.h` modified at 19:11, `ItlIwx.o` 19:14.
The file `ieee80211_node.d` did not contain any `net80211/` header (compile line: `-isystem itl80211/openbsd ... -MMD`).

## 3. Panic B: `ieee80211_reset_scan + 0x69`

Code (`ieee80211_node.c:831-833`), compiled with the **old** layout:

```c
if (ic->ic_bss != NULL && ic->ic_bss->ni_chan == IEEE80211_CHAN_ANYC)
    ic->ic_bss->ni_chan = &ic->ic_channels[IEEE80211_CHAN_MAX];
```
```
2c4bd: mov rax, [rbx + 0x1350]      ; ic->ic_bss (old offset)
2c4c4: test rax, rax
2c4c7: je   ...
2c4c9: cmp  qword [rax + 0x88], 0   ; <-- faults: RAX = 0x7fffff9775612008
```

`RAX = 0x7fffff9775612008` is a non-canonical pointer (hence the general protection fault, CR2 = 0). It is the valid pointer `0xffffff9775612008` (inside the GEN1 zone `0xffffff9775309000–0xffffff9c413d5000` in the report) with bit 63 cleared.

What cleared the bit: `ItlIwx::iwx_setup_he_rates` (`ItlIwx.cpp:4243`), compiled with the **new** layout, called in `iwx_init` (line 11662) right before `ieee80211_begin_scan` (line 11674):

```c
ic->ic_flags &= ~IEEE80211_F_HEON;      /* IEEE80211_F_HEON = 0x80000000 */
```
```
56149: and byte [rsi + 0x1377], 0x7f    ; rsi = sc, sc_ic at sc+0x20 -> ic+0x1357
```

`ic+0x1357` is the top byte of `ic_flags` in the new layout (0x1354..0x1357) and, in the old layout, the top byte of `ic_bss` (0x1350..0x1357). `0xff & 0x7f = 0x7f`: this matches the RAX value bit for bit.

The `RCX = 0x4444400000000444` in the report is not a corrupted pointer: it is the second word of the `ic_chan_active` bitmap that the `memcpy` had just copied.

## 4. Panic A: `ItlIwx::iwx_intr + 0x437`

Code (`ItlIwx.cpp:12969-12970`), **new** layout, in the `IWX_CSR_INT_BIT_SW_ERR` branch:

```c
XYLog("  802.11 state %s\n", ieee80211_state_name[sc->sc_ic.ic_state]);
```
```
67f8a: mov eax, [rbx + 0x1428]              ; rbx = ItlIwx*, sc_ic at +0xb8 -> ic+0x1370 = ic_state (new)
67f90: lea rcx, [rip + ...]                 ; ieee80211_state_name
67f97: mov rdx, [rcx + 8*rax]               ; <-- faults
```

`ic+0x1370` in the old layout is `ic_node_alloc`, a function pointer that `ieee80211_node_attach` (old object) wrote there. `ItlIwx.cpp` reads the low 32 bits of that pointer as `ic_state` and uses them as an index into the state name table: an access far outside the table. The real `ic_state` was written by net80211 at `ic+0x12f8`.

What the captured panic report (not published) does not allow confirming: the trap type and the registers (they are not visible). The firmware error (`SW_ERR`) that led to this branch is almost certainly a consequence of the same mismatch (the driver and net80211 disagreed about channels, MAC address, rates, etc.), but this was not proven.

## 5. Release bug or pump bug?

Neither.
- The pump code never ran in any relevant way: both panics occur in old code (`iwx_init`, firmware error handling), because of the shifted fields.
- The new writes were reviewed (`ic_awdl_roc_delay_hist[i]` with i ≤ 7, `ic_awdl_tx_state_hist[kind][state]` with 0..1 and 0..3, `ic_awdl_tx_ok_band[band]` with 0..3, `fRocStartHist`/`fDataLatHist` with i ≤ 7, `fDataQUs[slot]` with slot < 256 and `static_assert`): there are no out-of-bounds writes.
- The new AWDL interface fields are initialized in `init()`; `sizeof(AirportItlwmAWDLInterface)` is the same (0x1dc0) in the 2 units that use it, and `iwx_softc` only exists in one.
- Test 4 did not fail because the Debug build A97B541C came from a full build (76 files, 18:38), before the header change.

## 6. Fixes

Files changed (all in `driver/AirportItlwm`):

1. `itlwm.xcodeproj/project.pbxproj`: `OTHER_CFLAGS = -Xclang -sys-header-deps` in both project configurations. The `.d` files now list headers found through `-isystem`. Verified: 44 `.d` files now reference `net80211/ieee80211_var.h`, and a `touch` on that header recompiles 44 files (before: 0). The warning count did not change (844 on Sequoia Release, before and after).
2. `itl80211/openbsd/net80211/ieee80211_var.h` and `ieee80211.c`: layout fingerprint at link time. Every unit that includes the header references the symbol `ieee80211_layout_tag<sizeof(ieee80211com), sizeof(ieee80211_node), sizeof(_ifnet), sizeof(ieee80211_rxinfo), sizeof(ieee80211_channel)>::tag`; only `ieee80211.c` defines it. Objects compiled with a different layout ask for a symbol with a different name, which stays undefined: the kext fails the import check and does not link in the kernel, instead of booting and corrupting memory. Runtime cost: none (one constant pointer per object).
3. `tools/check-kext.sh` (new): checks the fingerprint (1 defined, 0 undefined), compares the size of the shared structures across all units in the dSYM, and runs the import check against a kernel-collection symbol list (`kc-syms.txt`). Run on kext B0FFCFDD it flags `ieee80211com 0x1b60 ×38 / 0x1bd8 ×3`.
4. `AirportItlwm/AirportItlwmAWDLInterface.cpp` / `.hpp`: hardened stop path (section 7).

Why this is sufficient: both panics are reads/writes of the same object at different offsets; with every unit compiled against the same header they cannot happen. In the new kext `ieee80211_reset_scan` reads `ic_bss` at `0x13c8` and the `and byte [rsi+0x1377], 0x7f` in `iwx_setup_he_rates` lands on `ic_flags`, as it should.

Behaviour: no functional change outside the AWDL stop path; without AWDL boot-args the AWDL interface is not created and the changed code does not run.

## 7. Review of the stop path

The shutdown error is not identified (there is no log, and with the test 5 kext the machine never finished booting). The review found these weaknesses, all older than the pump, and fixed them:

- `AirportItlwm::destroyAWDLInterface()` released `fAWDLBsdIf` and `fAWDLIf` on the `stop()` thread without going through the command gate, while the work loop could be using those pointers (`awdlActionFrameHandler`, `awdlDataFrameHandler`, the 3 AWDL timers, `enqueueData`). Now the pointers are set to NULL, the BSD interface owner is cleared and `teardownAW()` (cancels and removes the timers, drains the queue) runs inside the gate; only then is detach/release done.
- `outputAWDL()` read `fOwner` outside the gate and used it inside: it could pick up an already released interface. It now re-reads `fOwner` inside the gate and drops the packet if there is no longer an owner.
- 15 places did `fController->fHalService->get80211Controller()` without checking `fHalService`, which `releaseAll()` sets to NULL. They now use `halIc()`, which returns NULL in that case; `startListen()` gained the missing check.
- `teardownAW()` now resets `ic_awdl_want_data = 0`, so the P2P MAC promiscuous filter does not stay requested after AWDL goes away.

No problems found in: `iwx_stop` resets `sc_aux_roc_*`, `sc_p2p_*` and `sc_awdl_listen_active`; `iwx_awdl_listen`/`iwx_awdl_tx` refuse with `IWX_FLAG_SHUTDOWN`/`HW_ERR`; the pump only re-arms while `fAWRunning` and until `fTxPumpEndUs`; `stopAWTimeline()` cancels the 3 timers and sets `fTxPhase = 0`.

Not tested at runtime: none of this has run on hardware yet.

## 8. Builds and checks

Full build of all 4 (76 files each), all `BUILD SUCCEEDED`.

| Build | UUID | Fingerprint | Sizes (dSYM) | Imports |
|---|---|---|---|---|
| Sequoia Release | `7F4D5A99-C15F-3CAB-A6E7-5B87558F0DEC` | ok | `ieee80211com 0x1bd8` ×41 | 1088 undefined, 0 missing |
| Sequoia Debug | `C5795D35-9DBA-3FC8-B922-DB81F6508509` | ok | no dSYM | 1087 undefined, 0 missing |
| Tahoe Release | `8608E534-19A5-3E51-AD95-B2ABAF39523E` | ok | `ieee80211com 0x1bd8` ×41 | not applicable (list is from the 24H32 KC) |
| Tahoe Debug | `DB3FB2BD-F3C5-3BB0-BCE3-A3CD68F74524` | ok | no dSYM | not applicable |

Command: `tools/check-kext.sh <kext> <kc-syms.txt> <baseline-undef.txt>`.

Note: older copies of the kext made before this fix still contain stale binaries. The kext to install has to come from `build/dd-port/Build/Products/<config>/Sequoia/`, and it is worth running `check-kext.sh` on the final copy.

## 9. Recommended next test

The Release build never booted successfully and the pump never ran, so the two are separated, but within a single boot:

1. **Test 6a**: kext Sequoia Release `7F4D5A99`, test 4 boot-args, **without** `-itlawdltxpump`:
   `-itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc`
   Expected: same as test 4 (awdl0 active, no panic). Confirm the UUID with `kmutil showloaded` and run `awdlctl show`.
2. **Test 6b**, same boot, no reboot: `sudo tools/awdlctl/awdlctl pump on`, then `sudo tools/awdlctl/awdlctl lead 30000`, and AirDrop iPhone → Mac. A panic here would come from the pump and the report will say where.
3. Only if 6b goes well: next boot with `-itlawdltxpump itlawdllead=30000` in the boot-args (the test 5 configuration).
4. If 6a panics: repeat with Debug `C5795D35` and the same boot-args, to tell a real Release problem apart.

Keep `keepsyms=1 debug=0x100`, as before.
