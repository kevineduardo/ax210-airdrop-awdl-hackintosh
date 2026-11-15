# Changes from upstream

This file is the change notice required by GPL-2.0 section 2(a) for the two GPL-2.0 trees under `driver/`. Every changed or added source file there also starts with a two-line notice.

The git history makes the changes reviewable: the first commit, "Import upstream sources unmodified", is a verbatim copy of the upstream trees. Compare against it:

```sh
git diff $(git log --format=%H --grep 'Import upstream sources unmodified' | tail -1) -- driver/
```

## driver/AirportItlwm

**Base:** [crazyhulk/itlwm](https://github.com/crazyhulk/itlwm) at `ff8db43` ("AirportItlwm (Tahoe): experimental AWDL (awdl0) for AirDrop"), which builds on [laobamac/itlwm](https://github.com/laobamac/itlwm) and [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm). License: GPL-2.0.

Changes (November 2026):

| Area | Files | Change |
|---|---|---|
| Sequoia AWDL | `AirportItlwm/AirportItlwmAWDLConfig.h` (new), `AirportItlwmV2.{hpp,cpp}`, `AirportItlwmSkywalkInterface.hpp`, `itlwm.xcodeproj` | AWDL code is built for the Sequoia (15.2+) target too, not only Tahoe; `AIRPORT_AWDL_ABI_SEQUOIA` / `_TAHOE` select ABI-specific code |
| Sequoia headers | `include/Airport/macOS15/IO80211VirtualInterface.h`, `IO80211AWDLProtocol.h` (new, derived from Unopcoooo/itlwm), `include/Airport/IO80211VirtualInterfaceWCL.h`, `IO80211AWDLProtocol.h` (new shims), `macOS15/IO80211ControllerV2.h`, `apple80211_ioctl.h` | macOS 15 layouts checked against the 15.8.1 (24H32) kernel; `setAwdlInterface`; 24-byte `apple80211_awdl_sync_params` on Sequoia |
| Buffers on Sequoia | `AirportItlwmAWDLInterface.{hpp,cpp}` | `IO80211Buffer` methods called by symbol (no vtable on 24H32); null-buffer check |
| Bring-up switches | `AirportItlwmAWDLInterface.cpp` | `-itlawdlnoinject`, `-itlawdlsetif` |
| TX pump | `AirportItlwmAWDLInterface.{hpp,cpp}`, `ieee80211_var.h`, `hal_iwx/ItlIwx.cpp` | `-itlawdltxpump`, `itlawdlpumpus=`, `itlawdllead=`, `itlawdldwellx=`, `-itlawdlhometx`; dwell-state callback; TX/latency/dwell-delay counters |
| Peer timeline | `AirportItlwmAWDLInterface.{hpp,cpp}` | `-itlawdlfollow` (adopt the peer's timeline when we are master and it is not aligned); TX gate ignores peers silent > 10 s; reference peer = unicast data peer; `AWDLPeerSync` diagnostics |
| Multicast | `itl80211/openbsd/net80211/ieee80211_input.c`, `AirportItlwmAWDLInterface.cpp` | Group-addressed AWDL data delivered to `awdl0`; queued data older than 10 s dropped; `AWDLDataMcast` counter |
| Build safety | `ieee80211_var.h`, `ieee80211.c`, `itlwm.xcodeproj` | Struct-layout fingerprint symbol; `-Xclang -sys-header-deps` so header changes rebuild every dependent object |
| Shutdown | `AirportItlwmAWDLInterface.cpp`, `AirportItlwmV2.cpp` | `destroyAWDLInterface` unpublishes pointers and stops timers inside the command gate; `halIc()` tolerates NULL |
| Security | `AirportItlwmAWDLInterface.cpp` | Run-time AWDL switches (`setProperties`) require an administrator |
| Identification | `AirportItlwm/WCL/Info.plist`, `WCL/Sequoia-Info.plist` | `CFBundleGetInfoString` names this fork. CI sets `GIT_COMMIT=-awdl-<commit>`, so the driver reports `itlwm: 2.4.0-awdl-<commit>`. Bundle identifier, class names and kext file name are unchanged on purpose, so the kext stays a drop-in replacement and cannot load next to another AirportItlwm |

Engineering notes for each change are in [docs/research/](docs/research/).

## driver/BlueToolFixup

**Base:** [crazyhulk/BlueToolFixup-FixA2DP-BLE](https://github.com/crazyhulk/BlueToolFixup-FixA2DP-BLE) at `0fb661b` (release `v2.7.3-advsets-0fb661b`), a fork of [acidanthera/BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM). License: GPL-2.0.

Changes (November 2026), all in `BrcmPatchRAM/BlueToolFixup.cpp` and `Changelog.md`:

- `-btlfxadvsets` patch table for macOS 15.8.1 (24H32): six patterns (two exact, four masked) verified to match exactly once in that build's `bluetoothd`.
- Tables per macOS build (`kAdvSetsTables`), selected by kernel version; the 15.8.1 table is applied all-or-nothing, only when the `bluetoothd` `LC_UUID` matches, decided on the Mach-O header page before any code page is patched.
- `-btlfxadvsetsany` to apply patterns without the UUID check (testing only).
- The Tahoe 26.4.1 patterns are byte-for-byte unchanged and applied as before.
- Comment documenting a double match of the upstream `a2dpcheck` pattern on 15.8.1 (no behaviour change).

Details: [docs/research/ble-advsets-sequoia-changes.md](docs/research/ble-advsets-sequoia-changes.md).
