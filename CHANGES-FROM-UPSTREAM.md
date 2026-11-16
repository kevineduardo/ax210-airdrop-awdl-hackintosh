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

Further changes made while testing on macOS 26.7.2 Tahoe (25G332), 12 to 14 November 2026:

| Area | Files | Change |
|---|---|---|
| `awdl0` like a real Mac | `AirportItlwmAWDLInterface.cpp`, `AirportItlwmEthernetInterface.cpp`, `WCL/WCL.{hpp,cpp}` | `awdl0` is a hidden interface (not a network service) and the driver brings it up itself (`-itlawdlnoup` to disable); MTU stays 1500 |
| AWDL RX | `AirportItlwmAWDLInterface.cpp`, `ieee80211_input.c` | 802.11 headers with HT Control / addr4 accepted (19 % of frames were rejected), Null frames handled apart, hardware-deaggregated A-MSDU subframes accepted; counters for rejected frames |
| AWDL TX | `AirportItlwmAWDLInterface.{hpp,cpp}`, `hal_iwx/ItlIwx.cpp` | TX gated by the peer's availability window (`-itlawdlnoawgate`); per-peer timeline and per-destination TX gate; RSSI trust "bubble" (`itlawdltrustrssi=`); the sync master always sends the PSF, even in AF TX mode 3; adaptive rate ladder for unicast data from the peer's HT/VHT capabilities (`-itlawdlfixedrate`, `-itlawdllegacyrate`) |
| Family integration | `AirportItlwmAWDLInterface.cpp`, `AirportItlwmSkywalkInterface.{hpp,cpp}`, `AirportItlwmV2.{hpp,cpp}`, `include/HAL/ItlDriverInfo.hpp` | Data statistics reported to the peer manager so it leaves Low Power (`-itlawdlnostats`); `awdl0` marked enabled and the unicast data peer cached with `cachePeer` (`-itlawdlnoifenable`, `-itlawdlnopeercache`); group frames, tracked peer and follow use the family's cached peers; answers to `MAX_NSS_FOR_AP`, AWDL master channels, election parameters, VHT capability, optional HE capability (`-itlhecap`) |
| Output model | `AirportItlwmV2.cpp`, `AirportItlwmAWDLInterface.cpp` | Pull output model for `awdl0` with its own output thread, so the stack paces Apple's QUIC (`-itlawdlpush` for the old model) |
| Listen dwells | `AirportItlwmAWDLInterface.cpp` | Default longest dwell 420 TU; optional short dwells while AWDL idles (`-itlawdlidledwell`) |
| Infra statistics | `AirportItlwmV2.cpp`, `AirportItlwmSkywalkInterface.cpp`, `hal_iwx/*` | Infra packet counters by traffic class and channel load fed to IO80211Family; LQM snapshot with RSSI and CCA; firmware statistics v14 behind `-itlfwstats` |
| Sequoia safety | `AirportItlwmAWDLInterface.cpp` | The calls that use Tahoe-only vtable slots or message layouts (`findPeer`, `updateInterfaceDataStats`, `cachePeer`, peer-cache completion message 0x33) are compiled out of the Sequoia build |
| Release logging | `hal_iwx/ItlIwx.cpp` | `iwx_debug` 0 in Release builds |
| AWDL build guard | `AirportItlwmAWDLBuildGuard.{hpp,cpp}` (new, ported from ForbiddenItlwm's AWDLBuildGuard by retropineaple, GPL-2.0), `AirportItlwmV2.cpp`, `AirportItlwmSkywalkInterface.cpp`, `itlwm.xcodeproj` | AWDL only starts when the `IO80211AWDLProtocol` vtable anchors match and the kernel is on the verified list (`-itlawdlanybuild` skips the list); otherwise the kext behaves as without `-itlawdl`. Result published as `AWDLBuildGuard` |
| AWDL coex backend, phase 0 | `itlwm/hal_iwx/ItlIwxAWDLCoex.{hpp,cpp}` (new), `include/AWDLCoex/*.hpp` (new, copied from ForbiddenItlwm by retropineaple, commit a5f1eb9, GPL-2.0), `hal_iwx/ItlIwx.{hpp,cpp}`, `ieee80211_var.h`, `ieee80211_input.c`, `AirportItlwmV2.cpp`, `AirportItlwmAWDLBuildGuard.{hpp,cpp}`, `AirportItlwmAWDLInterface.cpp` | `-itlawdlcoex` selects ForbiddenItlwm's listener engine as a second AWDL backend and hard-disables the P2P listen path for the boot (`ic_awdl_coex_on`, `AWDLBackend`); firmware capabilities and command versions published as `ITLAWDLCoexFw*`; `ic_awdl_coex_*` seam fields and beacon hints (`ic_beacon_hint`, from ForbiddenItlwm); `iwx_rx_tx_ba_notif` reclaims Block Ack notifications for other firmware stations after checking every record (`iwx_txq_reclaim_ok`, from ForbiddenItlwm) instead of dropping them; `iwx_flush_sta_tids` bounds the TID before indexing `sc_tid_data` (from ForbiddenItlwm) |
| AWDL coex backend, phase 1 | `itlwm/hal_iwx/ItlIwxAWDLCoex.{hpp,cpp}`, `hal_iwx/ItlIwx.{hpp,cpp}`, `AirportItlwmAWDLInterface.cpp` | Ported from ForbiddenItlwm (retropineaple, a5f1eb9, GPL-2.0): the listener MAC context with its own PHY context and binding (`awdlCoexListener`, `awdlCoexMacSend`, `awdlCoexPickOwn`), country notice answers (`iwx_mcc_request`, `awdlCoexCountryAnswer`), channel moves (`awdlCoexRetarget`), the 80 MHz width and control position of the listener's PHY context, the receive hook with promiscuous isolation, listener add at the end of `iwx_run` and removal at RUN exit, `-itlawdlcoexshared`, `-itlawdlsameband`, `ITLAWDLCoex*` counters on the controller. Our changes: delivery through `ic_vendor_action_handler`/`ic_awdl_data_handler`, the listener's address from `ic_awdl_addr`, REMOVE + ADD of PHY context 1 for a band change while it is unbound, `AWDLRx*` publication throttled to once a second in coex mode |
| AWDL coex backend, phase 2 | `itlwm/hal_iwx/ItlIwxAWDLCoex.{hpp,cpp}`, `hal_iwx/ItlIwx.{hpp,cpp}`, `AirportItlwmAWDLInterface.{hpp,cpp}`, `AirportItlwmAWDLFrame.hpp` (new), `AirportItlwmV2.{hpp,cpp}` | Ported from ForbiddenItlwm (retropineaple, a5f1eb9, GPL-2.0): the listener's own station and queue (`awdlCoexTxSetup`/`awdlCoexTxTeardown`, spare low queue number accepted in `iwx_tvqm_enable_txq` only when no host queue owns it), sync frames (`awdlCoexTxSend`), the single-frame data pump with the send gate (`awdlCoexDataPump`), the stuck-frame watchdog, fixed rates and the rate ladder (`iwx_tx_fill_cmd`), the coex queue argument and transmit gate in `iwx_tx`, per-slot TX response attribution (`iwx_rx_tx_cmd`), no en0 watchdog reset or en0 errors for coex frames, node references released when a coex ring is reset, `-itlawdlcoexnogate`, `-itlawdlcoexcalwait`. Ours: the AWDL interface builds the frames the HAL pulls (`coexBuildSync`, `coexPopData`, channel override in `buildAWDLFrame`), the data header moved to `AirportItlwmAWDLFrame.hpp` (used by `flushData` too, same bytes), `itlawdlcoexpull=` |
| AWDL coex backend, phase 3 | `itlwm/hal_iwx/ItlIwxAWDLCoex.{hpp,cpp}`, `hal_iwx/ItlIwx.{hpp,cpp}` | Ported from ForbiddenItlwm (retropineaple, a5f1eb9, GPL-2.0): the receive Block Ack responder (`awdlCoexBaAddPeer`, `awdlCoexBaRemovePeer`, `awdlCoexBaWidth`, `awdlCoexSendMgmt`, receive part of `awdlCoexBaHandle`: ADDBA answer, idle-TID DELBA with confirmation, peer retirement, promiscuous receive off while a session is open), ADDBA request and DELBA parsing in the receive hook, management frame tags per ring slot and their completions (answer acknowledged, DELBA result), the Block Ack event log. Ours: behind `-itlawdlcoexba` (off by default), arrival-order counters for the iPhone's QoS data (`ITLAWDLCoexRxSeq*`), `ITLAWDLCoexQueueStage` set to `ok` after a successful queue setup |
| AWDL coex backend, phase 4 | `itlwm/hal_iwx/ItlIwxAWDLCoex.{hpp,cpp}`, `hal_iwx/ItlIwx.{hpp,cpp}` | Ported from ForbiddenItlwm (retropineaple, a5f1eb9, GPL-2.0): send Block Ack (`awdlCoexBaSendStep`, from the send part of `awdlCoexBaHandle`: aggregation queue on TID 0, ADDBA request, agreement and `TID_DISABLE_TX`), ADDBA answer and DELBA-for-our-TID parsing, `iwx_tvqm_enable_txq` queue owner and spare-number rule for the peer station, `iwx_rs_init` rate control for the peer (VHT80 2SS MCS 4 to 9), the aggregate path of the data pump with the ordering rule and the paused-full deferral, the peer pause (`awdlCoexBaPauseStep`), the probe window and the same-channel probe (`awdlCoexMacSend`), Block Ack notification and TX response counters for the aggregation queue, node references released when it is reset, no en0 watchdog reset or en0 errors for it. Ours: the HE 160 MHz rate entries are cleared in the peer's rate control command; the refill on reclaim, `-itlawdlbaaskshared`, `-itlawdlnosharedba` and `-itlawdlcoexbafilterhyst` are not ported |
| BlueToolFixup | `BrcmPatchRAM/BlueToolFixup.cpp` | Patch table for 26.7.2 (25G332, UUID-gated); `bluetoothd` recognised by vnode identity (path lookup only as a fallback); late start: if Lilu's route arrives after `bluetoothd` started, restart it with its pages evicted so the patches apply; statistics line 30 s after start |

Engineering notes for each change are in [docs/research/](docs/research/) and [docs/test-results-tahoe.md](docs/test-results-tahoe.md).

## driver/BlueToolFixup

**Base:** [crazyhulk/BlueToolFixup-FixA2DP-BLE](https://github.com/crazyhulk/BlueToolFixup-FixA2DP-BLE) at `0fb661b` (release `v2.7.3-advsets-0fb661b`), a fork of [acidanthera/BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM). License: GPL-2.0.

Changes (November 2026), all in `BrcmPatchRAM/BlueToolFixup.cpp` and `Changelog.md`:

- `-btlfxadvsets` patch table for macOS 15.8.1 (24H32): six patterns (two exact, four masked) verified to match exactly once in that build's `bluetoothd`.
- Tables per macOS build (`kAdvSetsTables`), including 26.7.2 (25G332, added later), selected by kernel version; the 15.8.1 table is applied all-or-nothing, only when the `bluetoothd` `LC_UUID` matches, decided on the Mach-O header page before any code page is patched.
- `-btlfxadvsetsany` to apply patterns without the UUID check (testing only).
- The Tahoe 26.4.1 patterns are byte-for-byte unchanged and applied as before.
- Comment documenting a double match of the upstream `a2dpcheck` pattern on 15.8.1 (no behaviour change).

Details: [docs/research/ble-advsets-sequoia-changes.md](docs/research/ble-advsets-sequoia-changes.md).
