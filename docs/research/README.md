# Engineering notes

Notes written while porting AWDL to Sequoia and making AirDrop work. They are detailed on purpose: the reasoning, measurements and dead ends are as useful as the result when you port this to another macOS build or another chip.

| Note | Topic |
|---|---|
| [awdl-sequoia-port-analysis.md](awdl-sequoia-port-analysis.md) | What in crazyhulk's Tahoe AWDL code depends on the IO80211Family ABI, and how macOS 15 differs |
| [awdl-sequoia-port-changes.md](awdl-sequoia-port-changes.md) | The Sequoia port itself, and how each layout was verified against the 15.8.1 kernel |
| [awdl-tx-timing.md](awdl-tx-timing.md) | Why Mac → iPhone transmit failed (late firmware dwells) and the TX pump |
| [panic-stale-struct-layout.md](panic-stale-struct-layout.md) | Two boot panics caused by objects compiled against different struct layouts |
| [airdrop-master-election-and-multicast.md](airdrop-master-election-and-multicast.md) | AirDrop failing when the Mac is AWDL master; multicast not reaching `awdl0` |
| [ble-advsets-analysis.md](ble-advsets-analysis.md) | How BLE advertising sets are gated in `bluetoothd` and how the patches work |
| [ble-advsets-sequoia-changes.md](ble-advsets-sequoia-changes.md) | The UUID-checked patch table for macOS 15.8.1 |
| [real-mac-reference.md](real-mac-reference.md) | Baseline AWDL values from a real Mac (states, sequences, latency, AirDrop throughput) |

Newer, topic-based notes (driver contract, AWDL timing, pf/QUIC, OpenCore patches) are in [`../reference/`](../reference/).

Line numbers in these notes refer to the tree at the time of writing.
