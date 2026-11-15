# Reference: AWDL on a real Mac (MacBook Air M2, Mac14,2, macOS 15.0.1 24A348)

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

Baseline values from a real Mac, collected on 2026-11-12 on the same network as the hackintosh (infrastructure on channel 100, 80 MHz, -68 dBm). The real Mac ran macOS 15.0.1 (24A348); the hackintosh ran 26.7.2 (25G332). Use these numbers when comparing a driver's behaviour against Apple's (see [wifi-airdrop-driver-best-practices.md](../reference/wifi-airdrop-driver-best-practices.md) §6).

## At rest (AWDL active, no AirDrop in progress)

- `awdl0`: UP, RUNNING, `status: active`, mtu 1500 (on Tahoe 26.7.2 the system sets 1484 on ours).
- `ping` to the gateway, 100 × 0.1 s: mean 19.3 ms, max 235 ms, std dev 35 ms, 0 % loss.
- Channel sequence in Idle: `[44 44 44 0 0 0 0 0 6 44 44 0 0 0 0 0]`, `infra(100) 60%`.
- States seen: `Infra Priority`, `Idle` (`RptDataPathThres`, `FaceTimeStateChange`).
- `monitorAWDLDataMode`: `Prev Tx:2 Rx:2`, `indatamode 1 infraPriority 1`.

## Comparison with the hackintosh (AX210, AirportItlwm with AWDL)

| | Real Mac | Hackintosh |
|---|---|---|
| Ping to gateway with AWDL active | 19 ms (max 235) | 17 to 20 ms (max 89 to 125) with an iPhone nearby; 12 to 21 ms with no peers |
| Sequence in Idle | `44 44 44 0 0 0 0 0 6 44 44 0 0 0 0 0` | same (set by IO80211Family) |
| Sequence in Data | see below | `44 44 44 44 44 44 0 0 6 44 44 44 44 44 0 0` (`Data + Infra Idle`, infra 24 %) |

The hackintosh column is from a build before the infrastructure traffic counters were reported; with them, the hackintosh also reaches `Infra Priority` (see [io80211-driver-contract.md](../reference/io80211-driver-contract.md) §3).

## During an AirDrop (iPhone → MacBook Air, 11.9 MB MP4, 2026-11-12)

- Received in ~3.3 s after accepting (≈3.6 MB/s).
- Sequence during data: `[infra(100) 78%] [100 0 44 0 0 0 0 0 6 0 44 0 0 0 0 0] Infra Priority`; `sc:Data` (`RptDataPathThres`) ~2 per second while data flows; `HasActAirDrop 0`, `HighThroughput 0`, AWDL Active Time 21 %.
- `ping` to the gateway during the transfer: mean 10.5 ms, max 89 ms (before: 14.8 / 101). The transfer does not make infra latency worse.
- With the AirDrop window open for 10 minutes and no transfer: mean 12.6 ms, p90 45, p99 91, max 147 ms; 9 % above 50 ms.
