# Changelog

All notable changes to this project. Versions follow [Semantic Versioning](https://semver.org/); pre-releases are marked `-beta.N`.

## [Unreleased]

### Added

- AWDL build guard (ported from ForbiddenItlwm by retropineaple): AWDL only starts when Apple's `IO80211AWDLProtocol` vtable matches at fixed anchor slots and the running kernel is one we verified (15.8.1 24H32 for the Sequoia kext; 26.7.2 25G332 and 26.7.3 25G342 for the Tahoe kext). On any other build the kext acts as if `-itlawdl` was not set, so normal Wi-Fi keeps working. The result is in the IORegistry property `AWDLBuildGuard`. `-itlawdlanybuild` skips the kernel list (not the vtable check).
- `scripts/check-compat.sh`: a read-only check to run before and after installing. It reports the macOS build and kernel, the Intel Wi-Fi card, the loaded kexts and `awdl0`, the boot arguments, the BLE advertising-sets log line and the pf/netagent state, with OK/WARN/FAIL lines and a verdict. Exit code 1 when something FAILs.

## [1.0.0-beta.2] - 2026-11-15

Adds the work done on macOS 26 Tahoe. Tested with an Intel AX210 on Tahoe 26.7.2 (25G332).

### Downloads

| File | What it is |
|---|---|
| `AirportItlwm-AWDL-Tahoe-Release.zip` | Kext for macOS 26 Tahoe. Use this one on Tahoe, and turn `PowerTimeoutKernelPanic` off in OpenCore |
| `AirportItlwm-AWDL-Sequoia-Release.zip` | Kext for macOS 15.2+. Carries the Tahoe fixes with the Tahoe-only kernel calls compiled out; **not yet tested on Sequoia hardware**. The tested Sequoia kext is the one in 1.0.0-beta.1 |
| `*-Debug.zip`, `*-dSYM.zip` | Verbose builds and debug symbols for bug reports |
| `BlueToolFixup-advsets-Release.zip` / `-Debug.zip` | BLE advertising sets for 15.8.1, 26.7.2 and 26.4.1 (`-btlfxadvsets`) |
| `awdlctl-universal.zip` | AWDL counters and run-time switches |
| `SHA256SUMS.txt` | Checksums |

### Measured on Tahoe 26.7.2

- AirDrop iPhone → Mac: 12 MB in 3.1 to 3.7 s. Mac → iPhone works but is slow: about 1 MB/s at best over AWDL.
- Complete transfers with the Mac as AWDL master and as slave; QUIC with 0 timing errors.
- BLE advertising sets applied in six boots in a row.
- Known problem: the Mac sometimes drops out of the iPhone's AirDrop list (workaround: `killall sharingd`).

### Added

- `awdl0` behaves like on a real Mac: hidden interface, brought up by the driver, MTU 1500.
- AWDL receive accepts 802.11 headers with HT Control/addr4 and hardware-deaggregated A-MSDU subframes.
- Pull output model on `awdl0`, so the network stack paces Apple's QUIC (`-itlawdlpush` for the old model).
- IO80211Family integration on Tahoe: data statistics to the peer manager, `setInterfaceEnable`, `cachePeer` with completion messages, peer activity stamps.
- Transmit gated by the peer's availability window, per-peer timeline and TX gate, RSSI trust threshold (`itlawdltrustrssi=`), PSF always sent by the sync master.
- Adaptive rate ladder for unicast AWDL data from the peer's HT/VHT capabilities.
- Infra counters, channel load and link-quality data reported to macOS; optional HE capability (`-itlhecap`) and firmware statistics (`-itlfwstats`).
- BlueToolFixup: UUID-gated table for 26.7.2 (25G332), `bluetoothd` recognised by vnode, restart of a `bluetoothd` that started before Lilu (`-btlfxnolatefix` to disable).
- `awdlctl`: `awgate`, `idledwell`, `datastats`, `dwell`. `check-kext.sh` checks imports against the running kernel by default.
- Tools: `vtcheck.py`, `kccalls.py`, `kcdis.py` (kernel collection), `pbxinfo.py` and Command Line Tools build scripts, `awdlquic` and AirDrop measurement helpers.
- Docs: Tahoe installation steps (OpenCore `PowerTimeoutKernelPanic` and the `ackTimerTick` patch), Tahoe test results, reference notes on the IO80211 driver contract, AWDL, Skywalk/pf/QUIC and OpenCore patches.

### Changed

- Default longest listen dwell is 420 TU.
- Release builds no longer log every firmware command.

## [1.0.0-beta.1] - 2026-11-15

First public release. Experimental: tested with an Intel AX210 on macOS 15.8.1 Sequoia (24H32). For Tahoe, use 1.0.0-beta.2 or later.

### Downloads

| File | What it is |
|---|---|
| `AirportItlwm-AWDL-Sequoia-Release.zip` | Kext for macOS 15.2+ (AWDL verified on 15.8.1). Use this one on Sequoia |
| `AirportItlwm-AWDL-Tahoe-Release.zip` | Kext for macOS 26 Tahoe. Not tested on hardware; superseded by the Tahoe kext in 1.0.0-beta.2 |
| `*-Debug.zip` | Verbose builds for bug reports |
| `*-dSYM.zip` | Debug symbols, to symbolicate panic reports |
| `BlueToolFixup-advsets-Release.zip` / `-Debug.zip` | BlueToolFixup with BLE advertising sets for 15.8.1 and 26.4.1 (`-btlfxadvsets`) |
| `awdlctl-universal.zip` | Command-line tool: AWDL counters and run-time switches |
| `SHA256SUMS.txt` | Checksums |

### What works on the test machine

- Native Wi-Fi (5 GHz 802.11ac 80 MHz) with AWDL on.
- AirDrop iPhone → Mac and Mac → iPhone (photos up to about 1 MB), with the Mac as AWDL master or slave.
- The Mac reappears in the iPhone's AirDrop list 6 to 9 s after a transfer.
- BLE advertising sets on 15.8.1.

Not tested yet: large files, many transfers in a row, Mac ↔ Mac, Handoff/Universal Clipboard, sleep/wake, 24 h stability, other Intel cards.

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
