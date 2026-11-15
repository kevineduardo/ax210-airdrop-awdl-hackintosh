# The contract between IO80211Family and the driver

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

IO80211Family owns almost all of the logic (AWDL, election, scheduling, traffic classification). The firmware driver only executes and reports. Most bugs in this port came from information the driver never provided, which the family needs to make the same decisions it makes on a real Mac; few were execution errors.

Rule of thumb: before the first build of a subsystem, list everything Apple's driver (`AppleBCMWLAN*`, inside the KC) calls in the family and compare it with what ours calls. `tools/kc/kccalls.py` produces that list.

Offsets in this note were read from the 26.7.2 (25G332) kernel collection unless stated otherwise. They are interoperability facts; re-read them on your own build with the tools in [methods-and-tools.md](methods-and-tools.md).

## 1. ABI: vtables and symbols

- The base classes (`IO80211Controller`, `IO80211InfraProtocol`, `IO80211AWDLProtocol`, `IO80211SkywalkInterface`) change layout between versions. A misplaced slot causes a panic or, worse, silently calls the wrong method.
- Check slot by slot against the KC you run: `tools/kc/vtcheck.py` compares the vtable of each kext class with its base class in `BootKernelExtensions.kc`. On 26.7.2: `AirportItlwm:IO80211Controller` 463 slots, `AirportItlwmSkywalkInterface:IO80211InfraProtocol` 668, `AirportItlwmAWDLInterface:IO80211AWDLProtocol` 568, 0 differences.
- Every import has to resolve. A wrong symbol name (`17apple80211_channel` was written where the name has 18 letters) makes OpenCore refuse the kext with `Prelinked injection … Invalid Parameter`, and the machine boots without Wi-Fi. `tools/check-kext.sh` checks the imports against the system KC ("imports: none missing") and `tools/build/build-clt.sh` fails if any of them does not resolve.
- Every compile unit must see the same struct layout. A `#define` that changes a structure's size in only some files caused the two panics of test 5 ([panic-stale-struct-layout.md](../research/panic-stale-struct-layout.md)). `check-kext.sh` compares the sizes across units.
- Fixed slots used on the AWDL RX path, identical on 24H32 and 25G332: `IO80211BufferPool::getBuffer` +0x118, `IO80211Buffer::returnBuffer` +0x118, `setLength` +0x138, `getCapacity` +0x140, `getBytesNoCopy` +0x158. Prefer calls by symbol where they exist.
- Tahoe ↔ Sequoia ABI differences (message sizes, `apple80211_awdl_sync_params` with 20 or 24 bytes, `apple80211_capability_data` with 21 or 19): table in [awdl-sequoia-port-analysis.md](../research/awdl-sequoia-port-analysis.md) §3.

## 2. AWDL data counters → data mode

**Symptom:** AirDrop at ~350 to 450 kB/s, scheduler always in `Low Power` (3 slots out of 16), `HasActAirDrop 0`, log `In AW: Tx:0 Rx:0 … indatamode 0`.

**Mechanism (worked out from the 25G332 KC):**

- `IO80211AWDLPeerManager::monitorAWDLDataMode` reads two peer-manager counters, at `+0x79c` and `+0x7b0`, and compares them with the previous values.
- They are written by `IO80211PeerManager::updateLocalCounters(apple80211_data_path_interface_stats*)`, which copies `stats+0x08` → `+0x7b0` and `stats+0x18` → `+0x79c`.
- That is only called from `IO80211SkywalkInterface::updateInterfaceDataStats` (vtable `+0xe28`). No family code calls that slot, so the driver has to call it. (`getDataPathInterfaceStats`, `+0xe08`, returns `kIOReturnUnsupported` in the base class.)

To reproduce on your own machine: find the log string `monitorAWDLDataMode` in the KC, disassemble the function with `tools/kc/kcdis.py`, and follow the two fields back to their writer.

**What the driver does (`reportDataStats`):** once per slot, when the counters have changed, it calls `updateInterfaceDataStats` with a zeroed 0x800-byte structure, data frames sent at `+0x08` and received at `+0x18`.

**Measured effect:** `Prev Tx:109 Rx:39` → `setScheduleState … RptDataPathThres sc:Data` → `From Idle -> To Data`, sequence with 11 to 12 slots out of 16. Throughput ~0.4 → ≥1 MB/s.

## 3. Infrastructure traffic counters → `Infra Priority`

**Symptom:** during an AirDrop our AWDL stayed in `Data + Infra Idle` (infra gets 24 % of the time); a real Mac goes to `Infra Priority` (78 %).

**Mechanism:**

- `IO80211AWDLPeerManager::infraPriorityModeRequired()` requires the infra traffic to be classified as `Real Time` or `Ultra Low Latency`.
- The classification comes from `gatherInfraStats()`, which reads the IOReporter channels of the interface monitor: `IfTxPk*` / `IfTxBy*` per queue, `IfRxPkts` / `IfRxByts`.
- The driver feeds those channels with `IO80211SkywalkInterface::recordOutputPacket(ac, packets, bytes)` and `recordInputPacket(packets, bytes)`.

**What the driver does:** TX in `AirportItlwm::enqueuePacket` (queue chosen from net80211's DSCP classification), RX in `AirportItlwmEthernetInterface::inputPacket`, for `en0` only.

**Measured effect:** `currentInfraTrafficType … inputPackets:32 outputPackets:35 packetThreshold:50`, `Infra Traffic type has changed from Idle to Real Time`, sequence `[100 0 44 0 0 0 0 0 6 0 44 0 0 0 0 0] Infra Priority`, the same as on a real Mac.

The threshold is 50 packets per ~1 s interval (input plus output), visible in the log itself.

## 4. AWDL interface: properties the system expects

- `awdl0` has to show up as `HiddenInterface`; otherwise macOS creates a "Wi-Fi 2" network service. That service cannot be removed, and disabling it brings `awdl0` down.
- The 1484 MTU on `awdl0` is set by macOS itself on Tahoe (on a Mac with 15.0.1 it is 1500). The driver should not touch the MTU.
- `awdl0` only uses IPv6 link-local.
- For user-space networking, the interface agent should show `FlowSwitch flags:0x4483` in `ifconfig -v awdl0`. `0x4403` (missing `NEXUS_PROVIDER`, 0x80) means the transport agent is off: see [skywalk-pf-quic.md](skywalk-pf-quic.md).

## 5. What is not given to the family yet

Work for later; each item is a place where behaviour can still differ from a real Mac.

- `reportDataTransferRates` (`awdl0` shows 54 Mbps).
- Per-peer TX state and per-peer RX statistics.
- Noise and channel occupancy (CCA) for `en0`.
- `APPLE80211_IOC_WCL_LOW_LATENCY_INFO_STATS` returns `0xe00002c7` (unsupported); it shows up in the log every 5 s.
- AWDL quiet mode (`preQueuePacketCheckQuietMode`) and stopping/flushing the TX queue when `awdl0` goes down.

## 6. How to find what is missing

1. Collect the kernel log of a real Mac in the same situation (the `com.apple.p2p.awdl0` and `InterfaceLogs` messages give the states and values the family sees).
2. Compare it with ours: the first value that stays at zero, or in a different state, is the lead.
3. Find the log string in the KC, disassemble the function (`tools/kc/kcdis.py`) and see which field the value comes from and who writes it.
4. If the writer is a virtual method the family never calls, the driver has to call it. Confirm with the calls made by `AppleBCMWLAN` (`tools/kc/kccalls.py`).

Run these on your own licensed copy of macOS and keep the raw disassembly on your machine (see [macos-driver-development.md](../macos-driver-development.md#legal-boundaries-we-follow)).
