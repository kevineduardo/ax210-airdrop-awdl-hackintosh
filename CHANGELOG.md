# Changelog

All notable changes to this project. Versions follow [Semantic Versioning](https://semver.org/); pre-releases are marked `-beta.N`.

## [1.0.0-beta.1] - 2026-11-15

First public release. Experimental: tested on one machine (Intel AX210, macOS 15.8.1 Sequoia 24H32).

### Downloads

| File | What it is |
|---|---|
| `AirportItlwm-AWDL-Sequoia-Release.zip` | Kext for macOS 15.2+ (AWDL verified on 15.8.1). Use this one on Sequoia |
| `AirportItlwm-AWDL-Tahoe-Release.zip` | Kext for macOS 26 Tahoe. Builds with our fixes; **not tested on hardware yet** |
| `*-Debug.zip` | Verbose builds for bug reports |
| `*-dSYM.zip` | Debug symbols, to symbolicate panic reports |
| `BlueToolFixup-advsets-Release.zip` / `-Debug.zip` | BlueToolFixup with BLE advertising sets for 15.8.1 and 26.4.1 (`-btlfxadvsets`) |
| `awdlctl-universal.zip` | Command-line tool: AWDL counters and run-time switches |
| `SHA256SUMS.txt` | Checksums |

### What works on the test machine

- Native Wi-Fi (5 GHz 802.11ac 80 MHz) with AWDL on.
- AirDrop iPhone → Mac and Mac → iPhone (photos up to about 1 MB), with the Mac as AWDL master or slave.
- The Mac reappears in the iPhone's AirDrop list 6–9 s after a transfer.
- BLE advertising sets on 15.8.1.

Not tested yet: large files, many transfers in a row, Mac ↔ Mac, Handoff/Universal Clipboard, sleep/wake, 24 h stability, Tahoe, other Intel cards.

### Added (compared with crazyhulk/itlwm ff8db43 and crazyhulk/BlueToolFixup-FixA2DP-BLE 0fb661b)

- AWDL (`awdl0`) on macOS 15 Sequoia, with IO80211Family layouts checked against 15.8.1.
- TX pump (`-itlawdltxpump`, `itlawdllead=`, `itlawdlpumpus=`): reliable AWDL transmit despite late firmware dwells.
- Peer timeline following when the Mac is AWDL master (`-itlawdlfollow`).
- AWDL multicast (mDNS) delivered to `awdl0`.
- Bring-up switches `-itlawdlnoinject`, `-itlawdlsetif`, `-itlawdlhometx`, `itlawdldwellx=`.
- Struct-layout fingerprint and header dependency fix (prevents stale-object panics).
- UUID-checked BLE advertising-sets table for 15.8.1 in BlueToolFixup.
- `awdlctl`, `kcsyms.py`, `check-kext.sh`, `verify-advsets.pl`, `monitor-awdl.sh`, `collect-diagnostics.sh`.
- Tests and CI; documentation for installation, troubleshooting, porting to new macOS versions and to other Wi-Fi chips.
