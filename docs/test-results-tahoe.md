# Test results: macOS Tahoe 26.7.2

How the AirportItlwm AWDL port and BlueToolFixup were brought up on macOS Tahoe 26.7.2 (25G332), and what was measured along the way. Each test is one boot; the kext UUID (first 8 hex digits) identifies the exact build. The Sequoia results are in [test-results.md](test-results.md).

Offsets and byte patterns in this document are specific to 25G332. Recheck them after any macOS or OpenCore update (see [After an update](#after-a-macos-or-opencore-update)).

## Environment

| | |
|---|---|
| Machine | AMD Ryzen 5 5600G, SMBIOS MacPro7,1 |
| Bootloader | OpenCore 1.0.8, Lilu 1.7.2 |
| macOS | Tahoe 26.7.2 (25G332), Darwin 25.6, IO80211Family 1200.13.1 |
| Wi-Fi / Bluetooth | Intel AX210 (Wi-Fi over PCIe, Bluetooth over USB `8087:0032`), firmware `iwlwifi-ty-a0-gf-a0-68` |
| Kexts at the end of this round | `AirportItlwm-Tahoe` AC461473, `BlueToolFixup` 4C91E517 |
| Build toolchain | Command Line Tools only (SDK 26.5) + MacKernelSDK, no Xcode (`tools/build/build-clt.sh`) |
| Access point | 5 GHz, channel 100 |
| Peers | an iPhone on a current iOS, and a MacBook Air M2 (macOS 15.0.1) used as a reference peer, both on the same network |

## Summary of builds

AirportItlwm-Tahoe, in order. "Not booted" means built and checked but never run.

| Build | What changed | Result |
|---|---|---|
| `B6927465` | First Tahoe build, CLT-only. ABI checked slot by slot against the 25G332 kernel collection; 1087 imports resolve | Boots, native Wi-Fi on en0, `awdl0` UP/RUNNING |
| `68CA54FF`, `D1A17BD2` | `awdl0` is a `HiddenInterface` and the driver brings it up itself; driver-set MTU 1484 | The extra "Wi-Fi 2" network service disappears. The MTU change was later removed: macOS sets 1484 itself |
| `6F3B0081` | AWDL RX: 802.11 header length from `ieee80211_get_hdrlen()` instead of a fixed 26 bytes; rejected-frame diagnostics | HT Control hypothesis wrong (`AWDLDataRxHtc 0`); `AWDLDataRxBad 111` vs `AWDLDataRx 105` |
| `1944D7B3` | AWDL RX: accept A-MSDU subframes deaggregated by the hardware | `AWDLDataRxBad 0`. 494 KB photo iPhone → Mac: 13 s from accept to done (was 21 to 75 s) |
| `24791B19` | TX gate on the peer's availability window (AW) | Discovery 6.7 s (was ~2 min), 0 mDNS SRV retries (was 11), 494 KB in 1.8 s. Mac → iPhone OK, 8 s total |
| `86C736D6` | `iwx_debug` 0 in Release builds | Firmware command log no longer fills the kernel buffer |
| `54617AA1` → `B73BD5C4` | Short listen dwells when idle; then made opt-in | Infra ping 7 to 10 ms instead of 12 to 21 ms with no peers. Off by default (see [Infra latency](#infra-latency-while-awdl-is-searching)) |
| `EF8D7FC3` | Report AWDL data counters to the peer manager (`updateInterfaceDataStats`) | IO80211Family enters Data mode: 11 of 16 slots instead of 3 |
| `306339DC` | Feed infra packet counters and channel load to IO80211Family | OpenCore refused the kext (wrong hand-mangled import name); booted without Wi-Fi. Import check made mandatory |
| `66BCF71C` | Same, fixed import | Infra Priority schedule like a real Mac; 33.8 MB/s download with AWDL on, gateway ping 13 ms |
| `39282085` | AF TX mode 3 suppresses only the PSF, not the whole TX window | AirDrop both ways over `awdl0`: iPhone → Mac 5.2 MB in ~12 s, Mac → iPhone 973 KB at 169 kB/s, no retransmissions |
| `99864DC8`, `3B97A901`, `B2041FED`, `AD5082E2` | Firmware statistics v14 (`-itlfwstats`), CCA in the LQM snapshot, `MAX_NSS_FOR_AP`, AWDL master channels, VHT capability, informational notifications acknowledged | `wdutil` CCA 4 %, master channel 6/44, those requests no longer fail |
| `1F40DFD9` | AWDL election parameters, HE capability (opt-in `-itlhecap`) | Not booted |
| `00648198` | Infra TX counters by traffic class, not DSCP | Did not stop the "Real Time" infra classification: wrong diagnosis (real cause: no cached peer) |
| `E457CEC4` → `3DE74834` → `B3072F85` | Ask the family to cache the data peer (`cachePeer`); clear the stale data-disabled flag; mark `awdl0` enabled (`setInterfaceEnable`) | Peer cache works with `B3072F85` (`cachedPeerCount` 5 to 8). 80 kB/s in, 113 kB/s out. RSSI flapping −59/−83 dBm |
| `DD071138` | Only the AP's own frames update the station RSSI | RSSI stable at −58/−59 dBm. Mac → iPhone hung: family cache full (`CPC 8 max 8`) |
| `2F6793C8` | Report peer cache operations as done (message 0x33) | Cache count stays at 1. With `-itlawdlhometx`: 11.9 MB each way over AWDL, 170 kB/s in, 133 kB/s out, all frames at 6 Mbps |
| `61DA725C` | Adaptive legacy OFDM rate | Not booted |
| `23025417` | HT/VHT rate ladder from the cached peer's capabilities | Peer caps 0x1f (HT/VHT, 2 streams), but the rate fell from 24 to 6 Mbps on the first idle losses |
| `7FBBB54D` | No step down on isolated losses | Mac → iPhone 133 → 365 kB/s (173 Mbps PHY). With a 420 TU dwell set at run time: iPhone → Mac 148 → 443 kB/s; Mac → iPhone ~700 kB/s for 13 s, then the family uncached the peer "Due to Inactivity" |
| `288A3F7E`, `15DAFEEC` | Per-peer activity stamps, interface packet totals; default longest dwell 420 TU | Active peer no longer removed. iPhone → Mac 488 kB/s. Mac → iPhone stalls after ~13 s; ~2000 `quic_time_delta` errors per upload |
| `B6408479` | Pull output model on `awdl0` | Nothing left `awdl0`: the output thread was never started |
| `25CC4015` | Start the `awdl0` output thread | Used with the OpenCore fix below: QUIC paced end to end, 0 `quic_time_delta` errors. Mac → iPhone 12 MB at ~250 kB/s to ~1.0 MB/s |
| `D9B251D7` | Per-peer timeline table, TX gate per destination | 4 peers tracked; unicast per destination OK; group frames still followed one peer |
| `451959F1` | Group frames, tracked peer and follow use the family's cached peers | iPhone → Mac without a manual ping (1 transfer). The family only caches peers during data exchange, so the rule rarely applied |
| `C01AAFF5` | Trust by RSSI (≥ −60 dBm, heard in the last 10 s); per-peer master and alignment diagnostics | Not validated on its own; superseded by `AC461473` |
| `AC461473` | The sync master always sends the PSF, even in `aftx_mode 3` | `AWDLTxPSF 272` = `AWDLTxPsfMasterMode3 272` while master in mode 3 (was 0). Visibility on the reference peer still to confirm |

BlueToolFixup:

| Build | What changed | Result |
|---|---|---|
| `D322FC4A` | Pattern table for `bluetoothd` 25G332 (UUID-gated, all or nothing) | All 6 patches can apply, but only 3 of 6 at boot in some boots |
| `C4879663` | "Late start": restart `bluetoothd` with its pages evicted if it was already running when the hook was installed | Still 1 site missing in a boot where the hook was installed early |
| `2923646F` | Recognise `bluetoothd` by vnode identity, not by `vn_getpath()` on every page | First boot: 1 site missing (lookup ran before root was mounted) |
| `4C91E517` | Retry the vnode lookup until root is mounted | 6 clean boots in a row, all 6 patches applied, no helper needed |

## Details per problem

### Getting a Tahoe build at all

The Sequoia AirportItlwm has MaxKernel 24.99.99 and its `start()` refuses any `version_major` other than 24, so a Sequoia-only EFI leaves Tahoe without Wi-Fi. The `AirportItlwm-Tahoe` target already existed; the Sequoia/Tahoe `#if`s are ABI only and the AWDL fixes are shared by both targets.

- ABI checked with `tools/kc/vtcheck.py` against `BootKernelExtensions.kc` of 26.7.2, slot by slot: `AirportItlwm:IO80211Controller` (463 slots), `AirportItlwmSkywalkInterface:IO80211InfraProtocol` (668), `AirportItlwmAWDLInterface:IO80211AWDLProtocol` (568), `AirportItlwmEthernetInterface:IOEthernetInterface` (340), `IOPCIEDeviceWrapper:IOService` (266): 0 differences. The fixed AWDL RX slots (`IO80211BufferPool::getBuffer` +0x118, `IO80211Buffer::returnBuffer` +0x118, `setLength` +0x138, `getCapacity` +0x140, `getBytesNoCopy` +0x158) are unchanged.
- `kmutil libraries --undef-symbols`: all imports resolve.
- Build without Xcode: `tools/build/build-clt.sh` compiles with the Command Line Tools and MacKernelSDK, reading sources, defines and paths from the `.xcodeproj` (`tools/build/pbxinfo.py`). BlueToolFixup has the same in `driver/BlueToolFixup/build-clt.sh`.
- After `306339DC` (one wrong hand-mangled symbol, `17apple80211_channel` instead of 18 characters; OpenCore logged `Prelinked injection … Invalid Parameter` and the Mac booted without Wi-Fi), `tools/check-kext.sh` checks every import against the running kernel collection and `build-clt.sh` fails if one does not resolve.

### AWDL RX: rejected frames were A-MSDU subframes (`6F3B0081`, `1944D7B3`)

19 % of AWDL data frames were rejected (`AWDLDataRxBad` 934 of 3956). The first guess, an HT Control field the port did not account for, was wrong (`AWDLDataRxHtc 0`). The last rejected frame (`AWDLDataRxBadLast`, 129 bytes) showed an A-MSDU subframe: the Intel hardware deaggregates and only strips the subframe header when the payload starts with an RFC 1042 / bridge-tunnel SNAP. AWDL uses Apple's OUI (00:17:f2), so the subframe comes out as an 802.3 length frame wrapped in RFC 1042 (`aa aa 03 00 00 00 <len>`) in front of the real AWDL LLC. 26 + 8 + 95 = 129.

`inputAWDLData` now recognises and skips that 8-byte prefix (`AWDLDataRxAmsduSub`). Result: `AWDLDataRxBad 0`, 368 of 537 frames were such subframes, and the 494 KB photo took 13 s from accept to done (the body itself 0.7 s).

The MTU change made in the same round (1484 vs 1500) was a single test each way and proved nothing: macOS sets `awdl0` to 1484 by itself, and the driver no longer touches it.

### Discovery: TX gated on the peer's availability window (`24791B19`)

The iPhone asked 11 times for the Mac's `_appsvcprepair._tcp` SRV record. The Mac answered each in under 5 ms and none were received; discovery took ~2 min. A peer that is not exchanging data only has to listen in its AW (the first 16 TU of each 64 TU slot). Frames sent outside it are lost.

With no unicast traffic to the peer for more than 300 ms, frames (group and unicast) now wait for the peer's next AW and leave between +1.5 ms and AW − 5 ms. With traffic in progress nothing changes. Result: BLE seen → ASK in 6.7 s, including the user's tap; 0 SRV retries; SYN → SYN-ACK in 0.35 s; 494 KB in 1.8 s; `AWDLTxAwGate on 1 held 295 sent 350`. The same cause explained an 11 s pause seen earlier after accept (the accept response was sent 3 times outside the AW).

An earlier Mac → iPhone test with no response was invalid: the iPhone was locked, and iOS silently drops what arrives over `awdl0` for a closed listener. With the iPhone unlocked, Mac → iPhone took 8 s in total.

### BLE advertising sets at boot (BlueToolFixup `D322FC4A` → `4C91E517`)

The 26.4.1 table had no UUID gate and on 25G332 only matched patterns 1 and 2. That left BLE half patched (advertising sets capability and extended scan on, but no LE event mask, random address or Extended Create Connection), which is the combination that gives Command Disallowed and `bluetoothd` restarts. The new 25G332 table uses patterns 1, 2 and 6 from 26.4.1 and 3, 4 and 5 with the 15.x masks; each matches exactly once and none crosses a page. The bytes match the `mingww64/BlueToolFixup-FixA2DP-BLE` fork at `85eb3a8`. That fork's extra RPA / Filter Accept List patch (`929c7f6`) was not included: it injects code across several pages and is untested.

At boot only 3 of 6 patches applied in some boots. Restarting `bluetoothd` with its pages evicted applied all of them, so the patch table was fine. Two hypotheses were tested:

1. Lilu activating plugins late: wrong. The hook is installed at ~1.3 to 1.7 s, long before `bluetoothd` starts (~5 s) and initialises the controller (~17 to 18 s).
2. Confirmed: `patched_cs_validate_page` identified the binary with `vn_getpath()` on every page. `vn_getpath()` works from the name cache and fails now and then with `ENOENT` (1 failure in 30 s in the stats line). The page that is not recognised is never patched and never revalidated; it was almost always the LE event mask page.

Fix: a thread call opens `/usr/sbin/bluetoothd` once (retrying every 250 ms until root is mounted), keeps the vnode identity (pointer + vid) and resolves the UUID gate from the file header. The hook recognises `bluetoothd` by that identity; `vn_getpath()` is only a fallback. If `bluetoothd` is already running when the hook appears, it is restarted with its pages evicted (`-btlfxnolatefix` disables this). Result: 4 unattended reboots plus 2 earlier ones, all clean (`all 6 patches applied`, `bluetoothd pages by vnode 1502, by path 0`). Before the fix, 3, 1, 1 and 1 sites were missing in 4 of 6 boots. The temporary boot-time helper that restarted `bluetoothd` was removed; it also restarted it needlessly when it checked before `bluetoothd` had loaded those pages.

### Feeding IO80211Family what a native driver feeds it

The AWDL schedule on Tahoe is decided by Apple's IO80211Family, as on a real Mac. It only works if the driver reports what the family expects. Methods: disassembly of the 25G332 kernel collection (`tools/kc/kcdis.py`, `tools/kc/kccalls.py`) and comparison with the calls AppleBCMWLAN makes. See [reference/io80211-driver-contract.md](reference/io80211-driver-contract.md).

- AWDL data counters (`EF8D7FC3`). `IO80211AWDLPeerManager::monitorAWDLDataMode` reads two counters written only by `IO80211PeerManager::updateLocalCounters`, called from `IO80211SkywalkInterface::updateInterfaceDataStats`, which the firmware driver calls. The port never did: `HasActAirDrop 0`, always "Low Power" (3 of 16 slots), AirDrop at ~350 to 450 kB/s. `reportDataStats()` now calls it once per slot when the counters change. With a test multicast load the family moved to `Data` by itself: `[44 44 44 44 44 44 0 0 6 44 44 44 44 44 0 0]`, 11 of 16 slots.
- Infra packet counters (`66BCF71C`). `infraPriorityModeRequired()` needs the infra traffic class, computed from the interface monitor's IOReporter channels, which the driver feeds with `recordOutputPacket` / `recordInputPacket`. Without them infra was always `Idle`. With them: `Infra Traffic type has changed from Idle to Real Time` and `[100 0 44 0 0 0 0 0 6 0 44 0 0 0 0 0] Infra Priority` (infra 78 %), the same sequence as the real Mac. The first version classified by DSCP and counted ordinary traffic as voice; it now uses the packet's traffic class.
- Peer cache (`E457CEC4` → `2F6793C8`). With no cached peer the family treated 50 infra packets as Real Time and held Infra Priority during an AirDrop (~60 kB/s Mac → iPhone). A native driver calls `IO80211VirtualInterface::cachePeer` before unicast. Three blockers had to go: the peer manager's data-disabled flag (set by `reset()`, never cleared by the family), the virtual interface never being enabled (`setInterfaceEnable`), and the missing completion message for `setPEER_CACHE_CONTROL` (0x33: 1 added / 3 deleted + address), without which the cache filled up (`No space to cache peer, CPC 8 max 8`) and Mac → iPhone hung.
- Peer activity (`288A3F7E`, `15DAFEEC`). The family uncached the peer "Due to Inactivity" mid-transfer because its inactivity check uses per-peer stamps set by its own TX/RX paths, which `awdl0` data bypasses. The driver now sets them (`setLastQueuePacket`, `setLastRxUnicastLinkActivity`) and reports interface packet totals.
- Requests the system makes. From the family's IOC debug log: `MAX_NSS_FOR_AP`, AWDL master channels, VHT capability, CCA in the LQM snapshot, and success for informational notifications. Noise stays unanswered: the AX210 firmware has no noise measurement, and 0 dBm would give a bogus SNR.
- Station RSSI (`DD071138`). Every received frame updated the BSS RSSI, including AWDL peers' frames, so it flapped between −59 and −83 dBm. Only frames from the AP update it now.

### Unicast rate ladder (`2F6793C8` → `7FBBB54D`)

All AWDL frames went out at a fixed 6 Mbps. Unicast data now uses a ladder at 20 MHz (the P2P PHY context width): legacy 6/12/24, then for an HT peer MCS 4/5/7 one stream, MCS 4/5/7 two streams, and VHT two streams MCS 8 (39 to 173 Mbps); legacy-only peers go up to 54 Mbps. Capabilities come from `setPEER_CACHE_CONTROL`. The first version dropped from 24 to 6 Mbps on idle losses, because AWDL frames are also lost when the peer is not listening. It now steps down only on the third uncompensated bad frame, never below 24 Mbps, and holds a step off only when it fails right after stepping up. Mac → iPhone went from 133 to 365 kB/s. HE and 80 MHz are not used: they need an HE station context and a wider dwell.

`-itlawdlhometx` (already in the boot-args) is needed: with a cached peer on the same AP the family schedules AWDL slots on the infra channel (`Data + Infra Data`) and without it the driver skipped them and iPhone → Mac stalled.

### Pull output model on `awdl0` (`B6408479`, `25CC4015`)

`awdl0` used a registered output handler (push), so there was no interface send queue and QUIC's paced launch times were ignored. It now uses `configureOutputPullModel` like en0; `outputStart()` takes packets while fewer than 32 wait in the driver and resumes the stack with `signalOutputThread()`. The first build sent nothing (`FQ_CODEL qlength 41, dequeued 0`): the output thread starts stopped and must be started with `startOutputThread()`; en0 does it in `setLinkStatus`, `awdl0` never did. `-itlawdlpush` keeps the old model.

### QUIC errors: Skywalk, pf and the PowerTimeoutKernelPanic quirk

On Tahoe AirDrop uses QUIC/HTTP3 over `awdl0` (`AirDropNW`), not the HTTP/TCP of Sequoia. Each Mac → iPhone upload logged ~2170 `quic_time_delta start time X is after end time Y` errors in `sharingd` (13 when receiving).

- Where the message comes from (disassembly of `libquic.dylib`): only `quic_rtt_process_new_sample`, when an ACK arrives before the packet's recorded send time. The send time comes from the pacer and is used only with Prague / L4S active. The kernel's `fq_codel` honours a packet's departure time only if it reaches the queue; here `netstat -qq -I awdl0` showed `paced pkts: 0` on both `awdl0` and en0.
- System difference. On this Mac no connection used `channel-flow` (user-space networking); everything was `socket-flow`. On the reference MacBook Air: 0 errors, `channel-flow awdl0` only, FlowSwitch flags `0x4483` (here `0x4403`, missing `NEXUS_PROVIDER`), `net.link.generic.system.enable_netagent` 1 (here 0), `net.filter_state` empty (here `pf`).
- Mechanism (XNU source). `dlil_filter_event` turns off the flowswitch transport agent at run time when a filter subsystem is "incompatible". The kernel marked pf about 4 s after each boot, when `com.apple.pfctl` loads the stock `/etc/pf.conf`, with the `CUSTOM_RULES_PRESENT` flag, although the only anchors were Apple's own.
- Root cause (DTrace in the kernel, SIP temporarily relaxed). Loading a single `anchor "com.apple"` rule made the kernel run `strcmp(com.apple, not.apple)`: the `"com.apple"` literal used by `pf_check_compatible_anchor` was changed in memory. OpenCore's `Kernel > Quirks > PowerTimeoutKernelPanic` first tries an inline patch in `IOService::ackTimerTick`. On kernel 25.6 that pattern no longer exists, so the quirk falls back to `mPowerStateTimeoutPanicMasterPatch`, which replaces the first `"com.apple\0"` in the kernel with `"not.apple\0"`. The linker shares that literal between `ackTimerTick` and pf.
- Full chain. Quirk → literal changed → every pf anchor counts as custom → `net.filter_state = pf` → transport agent off → no `channel-flow` → QUIC over sockets → Prague pacer has no effect → ~2000 RTT errors per upload.
- Fix. `PowerTimeoutKernelPanic = false` plus a kernel patch on `ackTimerTick` with the same intended effect that does not touch the literal (see [OpenCore requirements](#opencore-requirements-for-tahoe)). Confirmed in the running kernel with DTrace (`… 85 c0 66 90 4c 8d 3d …`, original `85 c0 74 51 4c 8d 3d`).
- Result. `net.filter_state` empty, `enable_netagent = 1`, FlowSwitch `0x4483` on en0 and `awdl0`, 784 `channel-flow en0` and 0 `socket-flow` in 6 min, 0 `quic_time_delta` errors, `networkQuality` 194/106 Mbps.

Details: [reference/skywalk-pf-quic.md](reference/skywalk-pf-quic.md).

#### QUIC over `awdl0` between two Macs (`tools/airdrop/awdlquic.swift`)

Network.framework QUIC server (`includePeerToPeer`) on the Hackintosh, client on the MacBook Air connected through the Bonjour endpoint seen on `awdl0`, 12 MB each way, three runs:

| Run | Air → Hackintosh | Hackintosh → Air | Connection setup |
|---|---|---|---|
| 1 | 1205 kB/s | 1076 kB/s | 518 ms (AWDL waking up) |
| 2 | 1304 kB/s | 885 kB/s | 8 ms |
| 3 | 1383 kB/s | 977 kB/s | 33 ms |

On the Hackintosh: 9 `channel-flow awdl0`, 0 `quic_time_delta` errors, `paced pkts: 27800`, `L4S pkts: 46514`, `CE marked: 1398`. The Prague / L4S pacer works end to end on `awdl0` with this driver. Control over en0 between the same Macs: 2334 / 4569 kB/s. The `awdl0` limit (~1 to 1.4 MB/s with clean QUIC, 0 bytes retransmitted, PHY 173 Mbps) is on the AWDL/driver side.

#### AirDrop after the fix

Mac → iPhone, 12 MB, four uploads the same night, all with clean QUIC (`tools/airdrop/airdrop-measure.sh`):

| Body | Throughput | CE marks | Output stalls | AWDL schedule during the upload |
|---|---|---|---|---|
| 49.4 s | ~250 kB/s | 436 | +27 | `Data + Infra Data` |
| ~12 s | ~1.0 MB/s | 65 | +14 | `Data + Infra Idle` |
| ~12 s | ~1.0 MB/s | 33 | +14 | `Data + Infra Idle` |
| ~38 s | ~320 kB/s | 137 | +16 | `Data + Infra Data` |

Throughput follows the channel schedule the family picks, not QUIC:

- `Data + Infra Idle`: `[44 44 44 44 44 44 0 0 6 44 44 44 44 44 0 0]`, 12 of 16 slots on AWDL channels, duty cycle 76 %.
- `Data + Infra Data`: `[44 44 44 44 100 100 100 100 6 44 44 44 100 100 100 100]`, 8 of 16 slots, duty cycle 52 %.

The family picks `Infra Data` when the iPhone announces real-time infra traffic. CE marks are not the cause (intervals at ~350 kB/s with zero marks and zero stalls). Why half the slots give a third of the throughput, and the ~1 MB/s ceiling at 173 Mbps PHY, both point at the `awdl0` TX path.

iPhone → Mac, 12 MB, two samples: the body went over the infra network (en0), ~3.3 MB/s, 3.1 to 3.7 s, 0 errors. In the first sample the iPhone tried `awdl0` first: the first Initial packet arrived, the server stayed in `read_client_hello`, a second packet from the same address/port was rejected (`failed to request add nexus flow … File exists`), and 436 ms later the iPhone switched to en0. In the second sample it did not try `awdl0` at all.

The reference MacBook Air sends 12.8 MB Mac → iPhone in ~1.0 s (~12 MB/s) over `awdl0`.

### Per-peer state and the TX gate (`D9B251D7`, `451959F1`, `C01AAFF5`)

With two AWDL peers nearby, the iPhone and the Mac did not see each other. The driver tracked a single peer and used its timeline and channel sequence to gate TX for every destination; with the other peer tracked, `ping6` to the iPhone got 0/4, and 6/6 once the iPhone became the tracked peer.

- `D9B251D7`: a `PeerTl` table (16 peers, learned from every action frame) and `destGateOpen()` per destination (that destination's channel and AW; unknown destinations count as reachable). `flushData()` in one pass with per-destination ordering. Result: unicast per destination works. Group frames (mDNS answers) and the follow logic still depended on the tracked peer, picked by arrival order (once a device the family classifies as `Not Routable … outside bubble`).
- `451959F1`: group frames go out when any family-cached peer is on channel and in its AW; the tracked peer prefers a cached peer; follow only adopts a cached peer's or the data peer's timeline. One iPhone → Mac transfer worked without a manual ping. A 1-minute measurement then showed the driver hearing the iPhone continuously (max 668 ms since its last action frame) while the family kept `cached:0`: it only caches peers during a data exchange (6 to 7 s entries), so the rule almost never applied.
- `C01AAFF5`: a peer is trusted if it is family-cached, the data peer, or heard in the last 10 s at ≥ −60 dBm (`itlawdltrustrssi`). `AWDLPeers` shows each peer's announced master and its aligned/misaligned counts.

See [reference/wifi-airdrop-driver-best-practices.md](reference/wifi-airdrop-driver-best-practices.md).

### PSF in `aftx_mode 3` (`39282085`, `AC461473`)

Once the family reached Infra Priority it set `awdl_aftxmode 3` (suppress). The port skipped the whole TX window in that mode: no MIF, no data (256 queued, `AWDLDataTx 0`), and the Mac was invisible. `39282085` skips only the PSF.

That was still wrong when the Mac is the sync master. Measured with the MacBook Air as a read-only probe (`dns-sd -includeAWDL`, `sharingd` log) and a capture on the Hackintosh's `awdl0`: the Air found the Hackintosh's `_airdrop._tcp`, completed DISCOVER, then ~14 s later removed all its records at once (`Missing TXT record`; kernel: `Peer <hackintosh-awdl-mac> not cached on awdl0`). It kept asking for SRV/TXT, and the Hackintosh answered almost every query within the same millisecond. Correlation:

| Time window | Hackintosh role | `aftx_mode` | Air sees the service |
|---|---|---|---|
| 71 s | master (metric 535) | 0 | yes |
| 2.5 min | master | 3 | no, not once |
| switch 3 → 0 | master | 3 → 0 | added 127 ms later |
| following minutes | slave of the iPhone (537 > 535) | alternates 0/3 | yes, continuously |

As master in mode 3 the peers that elected it got no PSF, could not sync, and removed it. It depended on the election metric drawn at each boot, which is why it was intermittent. The family's election itself was right. An A/B test of the TX switches (`awgate`, `hometx`, `follow`, 1 min each) showed no effect of its own.

`AC461473` suppresses the PSF in mode 3 only when the Mac is **not** the sync master. After 27 min as master in mode 3: `AWDLTxPSF 272`, `AWDLTxPsfMasterMode3 272` (0 before the fix). No AWDL peer was in range then, so visibility on the reference peer is still to confirm.

### Infra latency while AWDL is searching

With `awdl0` up and a peer nearby, gateway ping averaged 93 to 144 ms (max 334 to 745 ms); with `awdl0` down, 6 ms. Each listen dwell covered a full slot plus lead (65 + 30 ms), three times per 1.05 s cycle, leaving the infra link idle for 250 to 490 ms at a time. Short idle dwells (AW only, 16 TU + 6 ms, lead ≤ 12 ms) brought it to 7 to 10 ms with no peers (12 to 21 ms with long dwells). A first test suggested they broke discovery, but `sharingd` had stopped advertising AirDrop at the time; a clean test transferred a 4.9 MB HEIC in 10.8 s (14.3 s with long dwells). They stay opt-in (`-itlawdlidledwell`) because the firmware starts the dwell 10 to 40 ms late in ~20 % of cases. With the iPhone synced to the same channel sequence, ping is 17 to 20 ms with either setting; the reference Mac measures 19 ms.

Without the Finder AirDrop window open, AWDL does not run at all (`AWDLAWRunning No`).

### Other notes

- Firmware `iwlwifi-ty-a0-gf-a0-68` reports statistics v14: no noise, and no channel load in the old layout. Capabilities 18 (aux ROC), 26, 34 and 54 (`SESSION_PROTECTION_CMD` v1) are set; no `TIME_EVENT_CMD`, no `ROC_CMD`, no CDB. NAN was dropped for the AX210: the iwlmvm NAN code was removed upstream in July 2025 as not officially supported on MVM devices.
- Without an Apple ID the iPhone tries to move QUIC to the infra network and the Mac refuses (`Did not find a matching interface`, no IDS account). With both devices on the same network and that path working, the body goes over en0, as on a real Mac.
- Do not mount the sealed system snapshot of the running system. A failed `mount_apfs -s` left the kernel unable to verify compressed system binaries (`decmpfs … err 22`, SIGBUS in `networksetup`, `diskutil` and others) until reboot. Nothing was written; a reboot cleared it.
- The macOS network setup needs no helper: the Wi-Fi network is kept in preferred networks and the system keychain.

Captured logs for all of the above are not published.

## Works

| Feature | Status | Measured |
|---|---|---|
| Infra Wi-Fi (en0) through the native stack | OK | `networkQuality` ~194 to 207 / ~106 Mbps; gateway ping 4 to 6 ms with AWDL stopped |
| User-space networking (Skywalk) | OK, needs the OpenCore change | `net.filter_state` empty, `enable_netagent = 1`, `channel-flow` on en0 and `awdl0` |
| QUIC | OK | 0 `quic_time_delta` errors on every upload (was ~2170 per 12 MB) |
| AirDrop iPhone → Mac | OK | 12 MB in 3.1 to 3.7 s (~3.3 MB/s), body over infra when both are on the same network; over `awdl0` 5.2 MB in ~12 s |
| AirDrop Mac → iPhone | OK, slow over AWDL | 12 MB in ~12 s (~1 MB/s) or 38 to 49 s (~250 to 320 kB/s), depending on the AWDL schedule; 60 MB in ≤ 20 s when the body went over infra |
| AirDrop discovery | OK, with failures | seconds when it works (6.7 s BLE → ASK); see "Needs improvement" 1 |
| AWDL as master and as slave | OK | complete transfers in both roles |
| AWDL schedule decided by IO80211Family | OK | Idle / Low Power / Data / Infra Priority states and sequences match the real Mac |
| QUIC over `awdl0` Mac ↔ Mac | OK | 0.9 to 1.4 MB/s each way, 0 errors, pacer active |
| AWDL RX | OK | `AWDLDataRxBad 0` |
| BLE advertising sets (Continuity / AirDrop over BLE) | OK | 6 patches applied in 6 unattended boots in a row |
| Infra Wi-Fi with AWDL on | OK | gateway ping 17 to 20 ms average with an iPhone nearby (real Mac: 19 ms) |

## Needs improvement

In order of impact on daily use.

1. The Mac disappears from the iPhone's AirDrop list. It happens with the AirDrop Bonjour server ready, `_airdrop._tcp` announced on `awdl0` and the iPhone cached as an AWDL peer, sometimes right after boot. Workaround: `killall sharingd` and open the Finder AirDrop window. The PSF-in-mode-3 fix (`AC461473`) addresses one confirmed cause; it still has to be confirmed with the reference peer.
2. Upload throughput on `awdl0`. ~1 MB/s at best with a 173 Mbps PHY rate (the reference MacBook Air does ~12 MB/s with the iPhone). Suspects, not yet measured: frame-by-frame TX without A-MPDU aggregation, channel switch cost, output queue stalls (`AWDLOutputModel stalls`). With `Data + Infra Data` (half the slots) throughput drops to a third, not to two thirds.
3. Incoming QUIC handshake on `awdl0` stalled (seen once). The first Initial packet arrived, the server stayed in `read_client_hello`, and a second packet was rejected with `failed to request add nexus flow … File exists`. A capture is needed to know whether the rest of the ClientHello reaches the driver.
4. Reported `awdl0` rate. `reportDataTransferRates` is not fed; the system shows 54 Mbps.
5. Noise and CCA. `wdutil` shows noise 0 dBm and CCA 0 %. The -68 firmware (statistics v14) has no noise measurement; a build that estimates CCA from rx/tx/on-time is built but untested.
6. Infra latency while AWDL searches. Short dwells (`-itlawdlidledwell`) help but are off by default because of late dwell starts in the firmware.
7. Cleanup when AWDL goes down. `stopOutputThread` / `flushOutputQueue` when `awdl0` goes down; quiet mode (`preQueuePacketCheckQuietMode`); per-peer TX status and RX statistics.
8. HE / 80 MHz rates on AWDL, Bluetooth coexistence, power save, roaming and background scan: to review.
9. The AWDL peer is re-cached every 5 s (visible in the log): to investigate.

## Not tested

- Files over 500 MB and several transfers in a row.
- Stability over several hours and after sleep/wake.
- Classic Bluetooth (keyboard, mouse, AirPods).
- Anything that needs an Apple ID: Handoff, Universal Clipboard, Sidecar, AirPlay, Apple Watch unlock.
- AirDrop with the devices on different networks (forces the body over AWDL both ways).
- The RPA / Filter Accept List `bluetoothd` patch (separate branch, untested).

## Coex backend: to test on hardware

The coex backend (`-itlawdlcoex`, ported from ForbiddenItlwm, see [how-awdl-works.md](how-awdl-works.md#the-coex-backend)) lands in phases. Kevin runs these checks on the test machine (Ryzen 5 5600G, AX210, Tahoe 26.7.2 25G332, access point on channel 100, a DFS channel, in Portugal) after CI has built each phase. Record the numbers in this file.

Boot sets used below:

- **Set A** (regression): the recommended Tahoe set from [boot-args.md](boot-args.md), no new switch.
- **Set B** (coex): Set A plus `-itlawdlcoex`. The P2P switches in Set A then do nothing; they can stay.

Keep `keepsyms=1 debug=0x100` while testing.

### Every phase: regression with Set A

| Check | Command | Expected |
|---|---|---|
| No coex state without the switch | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -c ITLAWDLCoex` | `0` |
| Backend name | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep AWDLBackend` | `"p2p"` |
| Normal Wi-Fi | `networkQuality -s`; `ping -c 50 <gateway>` | Within 10 % of the last baseline (about 200/106 Mbps, ping 7 to 13 ms) |
| P2P AirDrop | 12 MB photo each way with `tools/airdrop/airdrop-measure.sh` | Same as 1.0.0-beta.2 (iPhone to Mac 12 MB in 3 to 4 s; Mac to iPhone about 1 MB/s) |
| Wi-Fi off/on, reconnect | Wi-Fi menu off/on 5 times; switch network once | No panic, en0 back each time |

### Phase 0: groundwork (Set B)

Nothing is sent from a listener yet. AirDrop is not expected to work with Set B.

| Check | Command | Expected |
|---|---|---|
| Backend selected | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'AWDLBackend\|ITLAWDLCoexSelected'` | `AWDLBackend = "coex"`, `ITLAWDLCoexSelected = Yes` |
| Same on `awdl0` | `ioreg -lw0 -r -c AirportItlwmAWDLInterface -d1 \| grep AWDLBackend` | `"coex"` |
| Firmware capabilities | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep ITLAWDLCoexFw` | `ITLAWDLCoexFwDynamicQuota = Yes` (needed for the listener's own channel). Record every `ITLAWDLCoexFwVer*` here. Expected: `TxCmd` above 8 (else the fixed VHT rates are skipped), `TlcConfig` 4, `AddSta` 12 or more. 99 means the firmware does not list the command |
| P2P path is off | `ioreg -lw0 -r -c AirportItlwmAWDLInterface -d1 \| grep -E 'AWDLRoc(Sent\|Started)\|AWDLListenStarted'` | all 0 (or absent) |
| Normal Wi-Fi | `networkQuality -s` | same as Set A |
| Kernel log | `log show --last boot --predicate 'eventMessage CONTAINS "coex"'` | `AWDL coex backend selected` once, no firmware errors |

Results: not run yet.

### Phase 1: listener and receive (Set B)

Before the boot: unlock the iPhone, set AirDrop to Everyone for 10 minutes, and open the AirDrop window in Finder. The Mac still does not send AWDL frames in this phase, so it does not appear on the iPhone.

| Check | Command | Expected |
|---|---|---|
| Listener added | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(Added\|Adds\|MultiState)'` | `ITLAWDLCoexAdded = Yes` while en0 is associated; `ITLAWDLCoexMultiState = "own-channel"` (not `no-dynamic-quota`, `phy-uncertain`, `fallback-same`) |
| Firmware commands | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(Mac\|Bind\|MultiBind\|Phy)Result'` | `1` each |
| Channel | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(Channel\|OwnWidth)'` | `149` / `80` is the goal. `6` / `20` means 149 to 161 are passive or not allowed at 80 MHz: then record `ITLAWDLCoexCountryAnswers`, `ITLAWDLCoexCountryLearned`, `ITLAWDLCoexCountryLastStatus` and `ITLAWDLCoexBeaconHints` (bits 0 to 3 are 149, 153, 157, 161; bit 4 is 6) |
| Country | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep ITLAWDLCoexCountry` | `ITLAWDLCoexCountryNotices` and `ITLAWDLCoexCountryAnswers` 1 or more after association; `ITLAWDLCoexCountryFailures` 0. en0 must stay associated through the first answer (no deauth in `log show --last 5m --predicate 'process == "kernel"'`) |
| Receive | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(AwdlAction\|ActionOnChannel\|ActionOffChannel\|Forwarded\|ForwardMalformed\|ForwardRefused)'`, twice, 10 s apart | `ITLAWDLCoexAwdlAction` rising by several per second with the iPhone awake nearby; `ActionOnChannel` much larger than `ActionOffChannel`; `Forwarded` follows `AwdlAction`; `ForwardMalformed` under 1 % |
| Promiscuous | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep ITLAWDLCoexPromisc` | `ITLAWDLCoexPromiscLive = Yes` (en0 is on channel 100, the listener elsewhere); `ITLAWDLCoexPromiscUncertain = No` |
| Errors | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(TeardownErrors\|Broken)'` | `0` / `No` |
| On `awdl0` | `ioreg -lw0 -r -c AirportItlwmAWDLInterface -d1 \| grep -E 'AWDLRxFrames\|AWDLRxChan149\|AWDLRxChan6\|AWDLRxGap\|AWDLPeers'` | `AWDLRxFrames` rising all the time; `AWDLRxChan149` (or `AWDLRxChan6` on the fallback) carries almost all of them; the largest gap per peer in `AWDLRxGap` well under 1000 ms; `AWDLPeers` lists the iPhone |
| macOS sees the peer | `/usr/bin/log show --last 2m --predicate 'process == "wifip2pd" OR process == "sharingd"'` | The iPhone's AWDL peer is added, with a plausible RSSI in dBm |
| Wi-Fi off/on | Wi-Fi menu off/on 5 times | No panic; `ITLAWDLCoexAdds` rises by one per association; `ITLAWDLCoexTeardownErrors` stays 0 |
| Wi-Fi cost (record only) | `networkQuality -s` with Set B, then with Set A | Record both. ForbiddenItlwm reports 30 to 45 % less en0 throughput while the listener runs |

Risk to watch in this phase: if the listener moves between 6 and 149 while running (`ITLAWDLCoexMoves` rises), check the kernel log for firmware errors. That move is a PHY MODIFY across bands on a bound context, which ForbiddenItlwm's notes say can crash the firmware. To force it, boot with the AP's country unknown to the card (first boot after an NVRAM reset) and watch `ITLAWDLCoexChannel` go from 6 to 149 when the country is learned.

Results: not run yet.

### Phase 2: transmit and AirDrop (Set B, then Set B plus `-itlawdlcoexnogate`)

AirDrop is expected to work both ways in this phase, without Block Ack.

| Check | Command | Expected |
|---|---|---|
| Station and queue | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(StaResult\|QueueResult\|QueueStage)'` | `1`, a queue number (not `0x8000....`), `"ok"` |
| Sync frames | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoexTx(Built\|Submitted\|NotReady)'`, twice, 10 s apart | `TxBuilt` and `TxSubmitted` rise by about 10 to 22 per second; `TxNotReady` stops rising once macOS has sent its template |
| Sync completions | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoexTx(OK\|Failed\|Stuck\|Halted)\|WedgedRebuilds\|BadCompletions'` | `TxOK` rising, `TxFailed` near 0, `TxStuck` 0, `TxHalted = No`, `WedgedRebuilds` 0, `BadCompletions` 0 |
| Address | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoexAddr'` | `ITLAWDLCoexAddrApplied = Yes`; `ITLAWDLCoexAddrChanges` small (one per `awdl0` address change) |
| Channel refusals | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(RefusedReason\|LastRefusal)'` | `RefusedReason = 0` while sending. Otherwise the bits say why: 2 passive, 4 DFS, 16 inactive, 64 move pending |
| On `awdl0` | `ioreg -lw0 -r -c AirportItlwmAWDLInterface -d1 \| grep -E 'AWDLCoexSync(PSF\|MI)\|AWDLTx(PSF\|MI)\b\|AWDLCoexPull'` | `AWDLCoexSyncPSF` and `AWDLCoexSyncMI` rising; `AWDLTxPSF` and `AWDLTxMI` stay 0 (the P2P push path is idle); `AWDLCoexPull = 192` |
| Discovery | iPhone share sheet with the Mac's AirDrop window open | The Mac appears. Record the time from opening the share sheet |
| Mac to iPhone | `tools/airdrop/airdrop-measure.sh` with a 12 MB photo | Completes; at least the 1.0.0-beta.2 speed (about 1 MB/s) and no stalls |
| iPhone to Mac | 12 MB photo | Completes over AWDL: `log show --last 5m --predicate 'process == "sharingd"' \| grep -i awdl` names `awdl0` for the transfer |
| Data counters during a transfer | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(DataTx\|UcastOK\|UcastFailed\|RateStep\|VhtFrames)'` | `UcastFailed` under 10 % of `UcastOK + UcastFailed`; `RateStep` climbs to 4 or 5 on 149 at 80 MHz; `VhtFrames` above 0 |
| iPhone schedule | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep -E 'ITLAWDLCoex(ScheduleKnown\|PeerSyncFrames\|PeerPresentEntries)'` | `Yes`, rising, 5 to 15 entries on our channel during data |
| Send gate | `ioreg -lw0 -r -c AirportItlwm -d1 \| grep ITLAWDLCoexGate` | `GateHolds` above 0 when the iPhone's schedule has away entries; `GateForced` small |
| `awdl0` queue | `ioreg -lw0 -r -c AirportItlwmAWDLInterface -d1 \| grep -E 'AWDLDataTx\b\|AWDLDataRx\b\|AWDLDataTxDropFull'` | rising / rising / 0 |
| Idle cost (record only) | `top -l 5 -stats pid,command,cpu -o cpu \| head -20` and `pmset -g thermlog`, with the listener up and no AirDrop activity for 5 minutes | Record the kernel_task CPU share with Set B and with Set A |
| A/B | Repeat the two 12 MB transfers with Set B plus `-itlawdlcoexnogate` | Record both directions; compare with the gate on |

Results: not run yet.

### Phase 3: receive Block Ack (Set B plus `-itlawdlcoexba`)

The iPhone can now send to the Mac in A-MPDUs. Test with a 12 MB photo and a 60 MB video, iPhone to Mac, first with Set B (phase 2 numbers, no Block Ack) and then with Set B plus `-itlawdlcoexba`. Run `ioreg -lw0 -r -c AirportItlwm -d1 | grep -E 'ITLAWDLCoex(Ba|Mgmt|RxSeq|RxVht|RxWide|DataRx)'` right after each transfer.

| Check | Command or key | Expected |
|---|---|---|
| Switch seen | `ITLAWDLCoexBaRx` | `Yes` |
| Request answered | `ITLAWDLCoexBaRequests`, `ITLAWDLCoexBaSessions`, `ITLAWDLCoexBaAnswers` | 1 or more each per transfer; `ITLAWDLCoexBaAnswerErrors` small (each error is a 20 ms retry) |
| Firmware session | `ITLAWDLCoexBaStatus` | the ADD_STA status with the BAID-valid bit (`0x8000` set) and status bits 0 (success); `ITLAWDLCoexBaBroken = No` |
| Peer station | `ITLAWDLCoexBaPeerAdded`, `ITLAWDLCoexBaPeerWidth` | `Yes` during and after the transfer, `80` on 149 at 80 MHz (`20` on channel 6) |
| Answer acknowledged | `ITLAWDLCoexBaPeerAccepted`, `ITLAWDLCoexMgmtOK` | `Yes`, 1 or more; `ITLAWDLCoexMgmtFailed` small |
| Receive filter | `ITLAWDLCoexBaFilterOff`, `ITLAWDLCoexPromiscLive` | `Yes` and `No` while a session is open; back to `No` and `Yes` after the session ends (60 s idle, `ITLAWDLCoexBaRxIdleTeardowns` and `ITLAWDLCoexDelbaConfirmed` rise) |
| iPhone sends wide | `ITLAWDLCoexRxVht80TwoStream`, `ITLAWDLCoexRxWide80` | both rising during the transfer: the iPhone sends VHT 80 MHz two streams |
| Fewer retries | `ITLAWDLCoexDataRxRetry` divided by `ITLAWDLCoexDataRxUcast` | lower than in phase 2 |
| Arrival order (record) | `ITLAWDLCoexRxSeqFrames`, `ITLAWDLCoexRxSeqOutOfOrder`, `ITLAWDLCoexRxSeqMaxBehind`, `ITLAWDLCoexRxSeqDuplicates` | Record them. If `RxSeqOutOfOrder` is above 1 % of `RxSeqFrames` during a transfer, a host reorder buffer is worth adding (see phase 5) |
| Speed, iPhone to Mac | `tools/airdrop/airdrop-measure.sh` (12 MB and 60 MB) | Clearly faster than phase 2. Target: 5 MB/s or more (ForbiddenItlwm measured about 1 to 5 MB/s from receive Block Ack alone on a shared channel, and 17 to 24 MB/s with everything on its own channel). Record MB/s for both sizes |
| Multicast while a session is open | Start a second transfer right after the first; open the share sheet on a second Apple device during a transfer | Both still work. Promiscuous receive is off while a session is open, so mDNS from the iPhone is not received then; record any extra delay |
| Errors | `ITLAWDLCoexTeardownErrors`, `ITLAWDLCoexBroken`, kernel log | `0`, `No`, no firmware errors |
| Wi-Fi off/on during an open session | Wi-Fi menu off and on during a transfer, 3 times | No panic; `ITLAWDLCoexBaPeerAdded = No` after Wi-Fi comes back, until the next request |

Results: not run yet.

### Phase 4: send Block Ack and A-MPDU (Set B plus `-itlawdlcoexba -itlawdlcoexbatx -itlawdlcoexpeerpause`)

Mac to iPhone in A-MPDUs. Test with a 12 MB photo and a 60 MB video, Mac to iPhone, twice: with the iPhone on the home AP (channel 100), and with the iPhone's Wi-Fi off. Then A/B: the same set without `-itlawdlcoexpeerpause`, and the same set plus `-itlawdlcoexnoorder`. Run `ioreg -lw0 -r -c AirportItlwm -d1 | grep -E 'ITLAWDLCoex(BaTx|BaAsk|Peer|Order|GateQueued)'` right after each transfer.

| Check | Command or key | Expected |
|---|---|---|
| Switches seen | `ITLAWDLCoexBaTxEnabled`, `ITLAWDLCoexPeerPauseEnabled`, `ITLAWDLCoexOrder` | `Yes`, `Yes`, `Yes` |
| Agreement | `ITLAWDLCoexBaTxStateName` | `"asking"` before the first transfer, `"agreed"` during it. `"failed"`: read `ITLAWDLCoexBaTxQueueResult` and `ITLAWDLCoexBaTxAggEnableStatus` |
| Asks | `ITLAWDLCoexBaTxAsks`, `ITLAWDLCoexBaTxAgreements`, `ITLAWDLCoexBaTxRefusals`, `ITLAWDLCoexBaAskOK` | agreement within a few asks; refusals 0; `BaAskOK` 1 or more |
| Queue | `ITLAWDLCoexBaTxQueueResult` | a queue number (not `0x8000....` or `0x4000....`) |
| Aggregation enabled | `ITLAWDLCoexBaTxAggEnableStatus` | status bits 0 (success) |
| Frames on the queue | `ITLAWDLCoexBaTxFrames`, `ITLAWDLCoexBaTxNotifs` | both rising during the transfer; `ITLAWDLCoexBaTxSubmitErrors` 0 |
| A-MPDU proof | `ITLAWDLCoexBaTxSentAir` divided by `ITLAWDLCoexBaTxNotifs`; `ITLAWDLCoexBaTxPeakReportedFrames` | well above 1 (frames per Block Ack); peak 16 or more |
| Acknowledged | `ITLAWDLCoexBaTxAcked` divided by `ITLAWDLCoexBaTxSentAir` | above 90 % |
| Rate | `ITLAWDLCoexBaTxVht80TwoStreamNotifs` compared with `ITLAWDLCoexBaTxNotifs`; `ITLAWDLCoexBaTxLastRate` | most notifications at VHT 80 MHz two streams. `LastRate`: bits 8 to 10 are the format (3 = VHT), bits 11 to 13 the width (2 = 80 MHz), bit 4 set for two streams |
| Peer pause | `ITLAWDLCoexPeerPauses`, `ITLAWDLCoexPeerForcedResumes`, `ITLAWDLCoexPauseResumeFailures`, `ITLAWDLCoexResumeRetirements` | pauses above 0 when the iPhone's schedule has away entries, forced resumes few, failures 0, retirements 0 |
| Ordering | `ITLAWDLCoexOrderSingleBehindAgg`, `ITLAWDLCoexOrderViaAgg` | single-behind near 0; via-agg rising. With `-itlawdlcoexnoorder`, single-behind rises |
| Reclaim | `ITLAWDLCoexReclaimRefused` | 0 |
| Single responses on the queue | `ITLAWDLCoexBaTxRespFailed` compared with `ITLAWDLCoexBaTxRespOK` | failures small |
| Speed, Mac to iPhone | `tools/airdrop/airdrop-measure.sh` (12 MB and 60 MB) | 5 times the 1.0.0-beta.2 speed (about 1 MB/s) or more, steady over 60 MB. Target 15 MB/s with the iPhone on a quiet channel; ForbiddenItlwm reports 21 to 26 MB/s on an AX211 when the iPhone's schedule allows it, and about 15 MB/s with 11 of 16 slots on our channel. Record MB/s per run |
| Speed, iPhone to Mac | same tool, 60 MB | No slower than phase 3 |
| en0 during a transfer | `ping -c 30 <gateway>` while sending 60 MB | Wi-Fi keeps working; record the loss and the time |
| Errors | `ITLAWDLCoexBaBroken`, `ITLAWDLCoexTeardownErrors`, `ITLAWDLCoexTxHalted`, kernel log | `No`, `0`, `No`, no firmware errors |

Results: not run yet.

### Phase 5: optional additions (each off by default, each tested on its own)

Add each switch to the best set from phase 4 (Set B plus `-itlawdlcoexba -itlawdlcoexbatx -itlawdlcoexpeerpause`) and compare with that set.

#### 44 at 80 MHz (`itlawdlcoexchan=149`, then `itlawdlcoexchan=44`)

For this machine first: in Portugal, with the AP on channel 100 (DFS), 149 to 161 may stay passive. Check phase 1's `ITLAWDLCoexChannel` first.

| Check | Command or key | Expected |
|---|---|---|
| Switch seen | `ITLAWDLCoexChanPreference` | `149` or `44` |
| Where 149 works (`ITLAWDLCoexChannel = 149` in phase 1) | boot with `itlawdlcoexchan=149` | Same as phase 4: `ITLAWDLCoexChannel = 149`, `ITLAWDLCoexOwnWidth = 80` (44 is only a fallback) |
| Where phase 1 ended on 6 | boot with `itlawdlcoexchan=149` | `ITLAWDLCoexChannel = 44`, `ITLAWDLCoexOwnWidth = 80`, `ITLAWDLCoexMultiState = "own-channel"`, `ITLAWDLCoexPhyResult = 1`. If it stays on 6, record `ITLAWDLCoexBeaconHints` (bits 5 to 8 are 36, 40, 44, 48) and `ITLAWDLCoexCountryLearned`: 36 to 48 are passive too, and an AP beaconing there at 80 MHz (centre 42) or on each of them is needed |
| 44 first | boot with `itlawdlcoexchan=44` | `ITLAWDLCoexChannel = 44`, `OwnWidth = 80` whenever 36 to 48 are allowed |
| Frames name 44 | `ioreg -lw0 -r -c AirportItlwmAWDLInterface -d1 \| grep -E 'AWDLRxChan\|AWDLPeers'`; the iPhone's sequence `ITLAWDLCoexPeerSequence` | The iPhone puts some of its slots on 44 (`2c` in the sequence bytes); `ITLAWDLCoexPeerPresentEntries` 5 or more during data |
| Sending allowed | `ITLAWDLCoexRefusedReason` | `0` while sending (not 2 passive, 4 DFS) |
| Rates | `ITLAWDLCoexRateStep`, `ITLAWDLCoexBaTxVht80TwoStreamNotifs`, `ITLAWDLCoexRxVht80TwoStream` | VHT 80 MHz two streams both ways, as on 149 |
| Moves | `ITLAWDLCoexMoves`, `ITLAWDLCoexMoveFailures`, kernel log | Moves between 44 and 149 are in the same band (PHY MODIFY); a move from 6 to 44 crosses bands, watch for firmware errors as in phase 1 |
| Speed | 12 MB and 60 MB both ways | Record. Expected close to phase 4 on 149 when the iPhone is idle; 6 at 20 MHz is the number to beat |
| Wi-Fi | `networkQuality -s` | Record. en0 on 100 and the listener on 44 share the 5 GHz radio as with 149 |

Results: not run yet.

#### AW-timed sync frames (`-itlawdlawsync`)

| Check | Command or key | Expected |
|---|---|---|
| Switch seen | `ITLAWDLCoexAwSync` | `Yes` |
| Frames | `ITLAWDLCoexAwSyncFrames`, twice 10 s apart with the iPhone idle (screen on, share sheet closed) | rising whenever `ITLAWDLCoexScheduleKnown = Yes`; `ITLAWDLCoexIdleSyncFrames` keeps rising too |
| Cold discovery | Lock the iPhone for 2 minutes, unlock, open the share sheet; 5 times with and 5 times without the switch | Record the seconds until the Mac appears. Expected: shorter with the switch |
| Cold connect | First transfer after the Mac appears | Record the time from tapping the Mac to the progress ring starting |
| CPU (record) | `top -l 5 -stats pid,command,cpu -o cpu \| head -20`, idle | kernel_task share close to phase 2 (the task wakes once per iPhone AW on our channel) |

Results: not run yet.

## OpenCore requirements for Tahoe

- `Kernel > Quirks > PowerTimeoutKernelPanic = false`. On kernel 25.6 the quirk falls back to replacing the kernel's first `"com.apple\0"` with `"not.apple\0"`, which breaks pf anchor checks, turns off Skywalk's transport agent and breaks QUIC pacing.
- If the effect of that quirk is still wanted, use a kernel patch instead: Base `__ZN9IOService12ackTimerTickEv`, Limit 4096, Find `85 C0 74 ?? 4C 8D 3D ?? ?? ?? ?? 4C 89 FF E8` (mask on the `??` bytes), replace the `je` with `66 90`, MinKernel 25.0.0, MaxKernel 25.99.99. It matches once in the function (offset 0x5C1 on 25G332).
- `AirportItlwm-Tahoe.kext` with MinKernel 25.0.0 / MaxKernel 25.99.99. The Sequoia AirportItlwm is limited to 24.x and refuses to start on 25.x.
- `NVRAM > Delete` includes `boot-args`, so the config value is always applied.
- The AMD patch "thread_invoke, thread_dispatch | Remove non-monotonic time panic" is identical to upstream AMD_Vanilla, but on kernel 25.6 its first two matches land in `_kperf_lazy_wait_sample` and `_vmx_hv_support`, not in the functions in its title (offline simulation of Find/Mask on the kernel). Recorded only; see [reference/opencore-patches.md](reference/opencore-patches.md).

## After a macOS or OpenCore update

1. `sysctl net.filter_state net.link.generic.system.enable_netagent` should give empty and `1`. If it gives `pf` and `0`, see [reference/skywalk-pf-quic.md](reference/skywalk-pf-quic.md).
2. `PowerTimeoutKernelPanic` must stay `false`.
3. Check the kext ABI against the new kernel collection: `tools/kc/vtcheck.py` and `tools/check-kext.sh` ("imports: none missing").
4. The BlueToolFixup pattern table is UUID-gated: a new `bluetoothd` needs a new table ([reference/bluetooth-ble-advsets.md](reference/bluetooth-ble-advsets.md)).
5. The `ackTimerTick` kernel patch is limited to 25.0.0 to 25.99.99 and its pattern must match exactly once.
