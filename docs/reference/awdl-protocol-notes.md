# AWDL in practice

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

Notes on how AWDL behaves as seen from the driver, on macOS 15 and 26. They apply to any single-radio chip (one channel chain at a time). Measurements below are from 26.7.2 (25G332) unless a section says otherwise.

## 1. Who decides what

- **IO80211Family** (`IO80211AWDLPeerManager`, `IO80211AWDLPeer`): master election, synchronization, peer list, state machine, channel sequence, action frames. The driver receives the sequence and the sync frame template ready-made.
- **Driver**: switch channels on time, transmit the sync frame, hand received action and data frames to the family, transmit data at the right moment, and report counters (see [io80211-driver-contract.md](io80211-driver-contract.md)).
- **User space**: `sharingd` (AirDrop), `rapportd`, `mDNSResponder`. Discovery starts over BLE; only then is there mDNS over `awdl0`.

On Tahoe, AirDrop uses QUIC/HTTP3 (`AirDropNW`); on Sequoia it was HTTP over TCP/TLS.

## 2. Timing

- 1 TU = 1.024 ms. A slot is 64 TU; the first 16 TU are the **Availability Window (AW)**, the rest are extension windows.
- The sequence has 16 slots (~1.05 s). Each position is a channel; `0` means "no AWDL in this slot" (the radio goes back to the infrastructure channel).
- An idle peer is only required to listen during the AW. Extension windows only stay open while traffic is flowing.
- `tx_down_counter` counts down to the start of the next slot (as in OWL and in `IO80211AWDLPeer::calculatePeerTimeDiff`).

Consequence (the AW gate): a frame sent to an idle peer outside its AW is lost. This was why the Mac took ~2 minutes to appear in the share sheet: the iPhone asked 11 times for an SRV record that the Mac always answered in under 5 ms, but outside the window. Fix: when the peer has had no unicast traffic for more than 300 ms, frames wait for that peer's next AW and go out between +1.5 ms and AW − 5 ms. After the fix there were 0 retries and discovery took seconds.

## 3. State machine and sequences (infra channel 100, AWDL channels 44 and 6)

Seen in the kernel log (`AWDL ON:[infra(100) N%] … [sequence] state`), identical on the hackintosh and on a real Mac:

| State | Sequence | AWDL slots | When |
|---|---|---|---|
| Low Power | `100 0 44 0 0 0 0 0 6 0 44 0 0 0 0 0` | 3 | resting, after 5 to 6 s in Idle |
| Idle | `44 44 44 0 0 0 0 0 6 44 44 0 0 0 0 0` | 6 | peer present, no data |
| Discovery | `44 44 44 44 0 0 0 0 6 44 44 44 0 0 0 0` | 8 | Bonjour service added |
| Data + Infra Idle | `44 44 44 44 44 44 0 0 6 44 44 44 44 44 0 0` | 12 | data on AWDL, infra idle |
| Data + Infra Data | `44 44 44 44 100 100 100 100 6 44 44 44 100 100 100 100` | 8 | data on AWDL and the peer has real-time infra traffic |
| Infra Priority | `100 0 44 0 0 0 0 0 6 0 44 0 0 0 0 0` | 3 | infra traffic classified `Real Time` |

Transitions and what triggers them:

- `RptDataPathThres`: data counters above the threshold → `Data` (`monitorAWDLDataMode`, `timeinmSecToStayInDataMode:1536`).
- `IdleTimeout`: ~5.5 s without data → `Low Power`.
- `BonjourServiceAdded` → `Discovery`; `DiscoveryTimeout` re-evaluates ~3 s later.
- `UserTriggered`: a request from user space (AirDrop window, share picker).
- On entering `Data`, the family picks between `Infra Idle` and `Infra Data` with `isAirDropActiveWithInfraRTPeer`: `Infra Realtime(1) peer` → `Infra Data`. It is the peer that advertises that state.

Measured effect on throughput, over 4 transfers of 12 MB Mac → iPhone: `Data + Infra Idle` gives ~1 MB/s and `Data + Infra Data` ~250 to 320 kB/s.

Without the Finder AirDrop window open (or another active AWDL service), AWDL does not run at all (`AWDLAWRunning No`).

## 4. RX pitfalls

- Don't compute the 802.11 header length by hand. There is QoS, HT Control (Order bit, +4 bytes) and addr4. Use the net80211 function (`ieee80211_get_hdrlen`).
- A-MSDU subframes with Apple's LLC: Intel hardware de-aggregates A-MSDU and only strips the subframe header when the payload starts with RFC 1042 SNAP or bridge-tunnel. AWDL uses OUI 00:17:f2, so each subframe arrives as a length-type 802.3 frame, wrapped in `aa aa 03 00 00 00 <len>` before the real AWDL LLC (`aa aa 03 00 17 f2 08 00`). The driver has to recognize and skip those 8 bytes. Before the fix, 19 % of data frames were rejected; after it, 0.
- Keep the last rejected frame. The `AWDLDataRxBadLast` counter (length + first 64 bytes) is what revealed the A-MSDU issue in minutes, after a wrong hypothesis (HT Control).
- Multicast: the slow-reappearance bug in the share sheet was multicast reception (see [airdrop-master-election-and-multicast.md](../research/airdrop-master-election-and-multicast.md) §7).

## 5. TX pitfalls

- The start of a dwell (remain-on-channel) in the Intel firmware is imprecise: in ~20 % of cases it starts 10 to 40 ms after the slot, and the error against the peer's timeline reached 9.5 ms. TX tied to fixed instants fails (see [awdl-tx-timing.md](../research/awdl-tx-timing.md)). Long dwells (whole slot + 30 ms lead) hide the imprecision; dwells cut down to the AW are on the edge.
- Long dwells cost infra latency: 3 dwells of ~95 ms per supercycle; slots 8 and 10 end up almost back to back (~225 ms off the infra channel).
- Firmware `-68` has aux ROC (capacity 18) and `SESSION_PROTECTION_CMD` v1; it has neither `TIME_EVENT_CMD` nor `ROC_CMD`. NAN is not an option on the AX210 (Linux iwlmvm removed that code in 2025; current NAN support is in iwlmld, for newer chips).
- Throughput on `awdl0` is capped at ~1 MB/s with a 173 Mbps PHY rate: the TX path sends one frame at a time. This is still unsolved.

## 6. System behaviour that looks like a driver bug

Check these before blaming the driver:

- iPhone locked: iOS silently drops what arrives over `awdl0` for a closed listener. A SYN with no answer while the iPhone is locked proves nothing.
- `sharingd` stuck: it can stop advertising over BLE and stop logging detections for minutes. `killall sharingd` and reopening the AirDrop window restores it. Check `AirDrop advertise` / `Found BLE device` in the log first.
- The AirDrop body may go over the infrastructure network. With both devices on the same network, QUIC is set up on, or migrates to, `en0` (seen in both iPhone → Mac transfers and in one 60 MB Mac → iPhone transfer). `awdl0` is then used for discovery and the request only. Look at `interface:` in the `flow:start_connect` lines before measuring "AWDL throughput".
- Without an Apple Account, the Mac refuses the migration to infra (`Did not find a matching interface`; `Pseudonym service does not have an active IDS account`) and the iPhone goes back to AWDL seconds later.
- `ping6` to an fe80 address on `awdl0`, and direct connections that skip the Bonjour browse, fail with "No network route": the system only enables the AWDL path for peers found through discovery with `includePeerToPeer`.

## 7. Where to look

```sh
# state, sequence and transition reasons
/usr/bin/log show --last 5m --style compact --predicate \
  'process == "kernel" AND (eventMessage CONTAINS "AwdlStateMachine: From" OR eventMessage CONTAINS "AWDL ON:" OR eventMessage CONTAINS "Data + Infra")'

# what the family sees of our traffic (same command, other predicates)
… eventMessage CONTAINS "monitorAWDLDataMode"        # In AW: Tx/Rx … indatamode
… eventMessage CONTAINS "currentInfraTrafficType"    # inputPackets/outputPackets/packetThreshold
… eventMessage CONTAINS "Traffic type has changed"

# driver counters
ioreg -r -c AirportItlwmAWDLInterface -l | grep AWDL
sudo tools/awdlctl/awdlctl …                         # run-time switches (changing them needs sudo)
```

`sudo scripts/monitor-awdl.sh -n <name>` collects all of this plus a pcap of `awdl0`.
