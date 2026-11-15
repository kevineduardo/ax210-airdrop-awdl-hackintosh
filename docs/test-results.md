# Test results

How AirDrop went from "the Mac shows up but nothing arrives" to working, on one machine. Each test is one boot with one EFI; the kext UUID identifies the exact build.

**Machine:** AMD Ryzen 5 5600G, SMBIOS MacPro7,1, OpenCore 1.0.8, macOS 15.8.1 Sequoia (24H32), Intel AX210 (Wi-Fi over PCIe, Bluetooth over USB `8087:0032`), access point on 5 GHz channel 100 (DFS, 80 MHz). Peer: an iPhone on a current iOS.

## Summary

| Test | Build | What changed | Result |
|---|---|---|---|
| 1 to 2 | upstream AirportItlwm + IntelBluetoothFirmware | native Wi-Fi/BT baseline | Wi-Fi and Bluetooth through the native stack |
| 3 | port, Debug | AWDL port to Sequoia, `-itlawdlnoinject`; BLE advsets on | `awdl0` up, AWDL reception works, BLE advertising sets active |
| 4 | port, Debug | full AWDL path | AirDrop iPhone → Mac reaches ASK accepted, UPLOAD fails: Mac → iPhone TX unreliable (`TxFwBusy 919` vs `TxFwOk 630`) |
| 5 | port, Release | TX pump in boot-args | Panic at boot (`iwx_intr`, `ieee80211_reset_scan`): stale objects with two `ieee80211com` layouts in one kext |
| 6 | Release `7F4D5A99` | clean build + layout fingerprint; pump switched on at run time with `awdlctl` | No panics. Data latency < 5 ms for 294 of 334 frames. First complete AirDrop iPhone → Mac |
| 7 | `7F4D5A99` | pump in boot-args | Fails again: this time the Mac won the AWDL master election and the iPhone never followed its timeline |
| 8 | `0C0630E7` | `-itlawdlfollow`, TX gate fixes | iPhone → Mac 2/2 with the Mac as master, 0 retransmissions; Mac → iPhone OK |
| 9 | `17DB71E3` | AWDL multicast delivered to `awdl0`, stale data dropped after 10 s | iPhone → Mac 2/2. Mac reappears in the iPhone's list in 6 to 9 s instead of 41.6 s |

## Details per problem

### TX: the firmware starts dwells late (tests 4 → 6)

The driver sent its frames at fixed offsets inside each listen dwell (+1 ms, +4 ms). The firmware starts the dwell 2 to 5 ms after the request in 41 % of cases, but 10 to 75 ms late in about 55 %. Half the dwells sent nothing, ACKs waited up to 524 ms, and the iPhone reset the TCP connection after 3 to 5 s.

The TX pump polls the dwell state every millisecond and sends as soon as it runs. After it: `TxFwBusy` stopped growing, TX ok 1573 vs 4 failures, and 915 of 963 data frames left in under 5 ms during a successful transfer.

Full analysis: [research/awdl-tx-timing.md](research/awdl-tx-timing.md).

### Panics: one kext, two struct layouts (test 5)

The TX pump added 0x78 bytes in the middle of `struct ieee80211com`. The incremental build recompiled 3 files and kept 38 old objects. Both panics had this one cause. The fix is in the build system, guarded by a layout fingerprint and `tools/check-kext.sh`.

Full analysis: [research/panic-stale-struct-layout.md](research/panic-stale-struct-layout.md).

### Master election (tests 7 → 8)

In the successful run an Apple device was AWDL master and the Mac followed it. In the failed run the Mac was master (metric 539) for the whole boot, the iPhone did not sync to it, and in low-power mode (3 slots of 16) they had no common slots, so the upload connection never came. `-itlawdlfollow` adopts the peer's timeline when the Mac is master and the peer is not aligned. With it, the Mac as master transferred 2 of 2 with slot alignment error between −2.4 and +0.3 ms.

### Multicast (tests 8 → 9)

The net80211 hook only delivered frames addressed to `awdl0`'s MAC, so the iPhone's mDNS queries were dropped (29 group packets sent by the Mac, 0 received in 68 s). The Mac only reappeared in the AirDrop list when Bonjour re-announced by itself. After the fix `AWDLDataMcast rx` went from 0 to 60 and reappearance took 6 to 9 s, including the time to reopen the share sheet.

Full analysis: [research/airdrop-master-election-and-multicast.md](research/airdrop-master-election-and-multicast.md).

## Test matrix

| Area | Test | Status |
|---|---|---|
| AirDrop | iPhone → Mac, Mac as slave and as master | OK |
| AirDrop | Mac → iPhone, small file | OK |
| AirDrop | Mac reappears in the list after a transfer | OK (6 to 9 s) |
| AirDrop | file > 500 MB, 5+ transfers in a row, Mac ↔ Mac | not tested |
| Wi-Fi | 5 GHz 802.11ac 80 MHz with AWDL on | OK |
| Bluetooth | controller, BLE advertising sets | OK |
| Bluetooth | keyboard, mouse, AirPods, sleep/wake | not tested |
| Continuity | Handoff, Universal Clipboard, AirPlay to Mac, Universal Control | not tested |
| Stability | 24 h | not tested |
| Tahoe | Tahoe build of this fork | tested on hardware by the maintainer |

## Help wanted

Reports from other machines are the most useful contribution right now: other Intel cards (AX200, AX201, AX211), more Tahoe machines, other Sequoia builds, Intel CPUs, laptops, and SIP fully enabled. Use the "Test report" issue template.
