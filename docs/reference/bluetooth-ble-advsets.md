# Bluetooth: in-memory `bluetoothd` patches (BLE advertising sets)

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

AirDrop and Continuity start with BLE advertisements. With the AX210, `bluetoothd` needs six patches (`-btlfxadvsets`, BlueToolFixup) to use advertising sets and extended scan/connect the way the controller expects. The patches are applied in memory, page by page, through a Lilu hook on `cs_validate_page`. The original analysis is in [ble-advsets-analysis.md](../research/ble-advsets-analysis.md); the 15.8.1 table is in [ble-advsets-sequoia-changes.md](../research/ble-advsets-sequoia-changes.md). Everything below was observed on 26.7.2 (25G332) unless stated otherwise.

## The six sites

| # | What it changes | When the page is loaded |
|---|---|---|
| 1 | advertising sets capability | early |
| 2 | extended scanning | early |
| 3 | LE event mask | controller initialization (~17 to 18 s) |
| 4 | advertising set random address | controller initialization |
| 5 | Extended Create Connection | controller initialization |
| 6 | advertisement resend | first advertisement |

## Lessons

### 1. All or nothing, behind a UUID gate

A pattern table from another macOS version applied only patches 1 and 2 on 25G332. Half-patched BLE is worse than no patches at all: advertising sets and extended scan are on, but without the event mask, random address and Extended Create Connection the result is `Command Disallowed` and `bluetoothd` restarts.

The table is chosen by the binary's UUID (`bluetoothd` 26.7.2 x86_64: `7EF7EF4D-9B0E-355A-BBA8-F34713B2580A`); if it does not match, nothing is applied. Each pattern must match exactly once and must not cross a page boundary.

### 2. Identify the binary by its vnode, not its path

The hook called `vn_getpath()` on every page. That function goes through the name cache and occasionally fails with `ENOENT`. A page that goes by unrecognized stays unpatched and is never validated again. It was almost always the LE event mask page: in 4 out of 6 boots, 1 to 3 sites were missing.

Fix: open `/usr/sbin/bluetoothd` once (retrying every 250 ms while the root volume is not mounted yet), store the vnode identity (pointer + vid) and compare against that. `vn_getpath` stays only as a fallback. Result: 6 boots in a row with all 6 patches, even with the `vn_getpath` failure still happening (`stats: bluetoothd pages by vnode 1305, by path 0; vn_getpath failures 1`).

This applies to any Lilu plugin that patches a user-space binary through `cs_validate_page`.

### 3. Boot ordering

Lilu starts its plugins before the root volume is mounted (the root becomes available at ~1.3 to 1.7 s); `bluetoothd` starts at ~5 s. If `bluetoothd` is nevertheless already running when the root appears, the kext kills it and evicts the binary's pages (`ubc_msync … UBC_INVALIDATE`), so `launchd` relaunches it with every page going through the hook. `-btlfxnolatefix` turns this off.

### 4. Verify without fooling yourself

- Reading a site's state before `bluetoothd` has loaded that page wrongly counts it as "not applied". A verification script that did this restarted `bluetoothd` for no reason, which in turn left `sharingd` not advertising.
- Manual proof that the patterns are right: `launchctl bootout system/com.apple.bluetoothd`, `purge`, `launchctl bootstrap` → all initialization sites are applied.
- The kext logs `all 6 patches applied (table …)` and a `stats:` line. For reliability, check these over several consecutive reboots, not just one (a reboot-loop script was used for this; not published).

### 5. Alternatives, and why not

- Patching `bluetoothd` on disk breaks the sealed system volume.
- The RPA / Filter Accept List patch from another fork injects code into several pages; not tested yet.

## A table for a new macOS version

See also [porting-awdl-to-new-macos-versions.md §6](../porting-awdl-to-new-macos-versions.md#6-bluetoolfixup-for-the-new-build).

1. Get the UUID of the new `bluetoothd` (`dwarfdump --uuid /usr/sbin/bluetoothd`).
2. Search for each pattern in the binary: it must match once (`tools/advsets/verify-advsets.pl`). Patterns 1, 2 and 6 from 26.4.1 worked on 26.7.2; 3, 4 and 5 needed the masks from 15.x.
3. Confirm that no match crosses a 4 KiB page boundary.
4. Add the table with the UUID and rebuild BlueToolFixup (it builds with the Command Line Tools alone, against Lilu 1.7.2 DEBUG).
