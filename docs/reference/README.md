# Reference notes

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

What we learned getting the Intel AX210 to work with the native macOS Wi-Fi stack (Sequoia 15.8.1 and Tahoe 26.7.2), written for anyone doing the same with another driver or other hardware. Each claim says how it was checked; anything that is still a hypothesis is marked as one.

These notes go deeper than the user guides. For the big picture read [how-awdl-works.md](../how-awdl-works.md) first; for installing and switches see [installation.md](../installation.md) and [boot-args.md](../boot-args.md).

| Note | What it covers |
|---|---|
| [wifi-airdrop-driver-best-practices.md](wifi-airdrop-driver-best-practices.md) | **Start here.** Best practices for a third-party Wi-Fi driver that has to support AWDL and AirDrop on Tahoe, with links to the details |
| [io80211-driver-contract.md](io80211-driver-contract.md) | What IO80211Family expects a third-party driver to provide: ABI, counters, statistics, buffers |
| [awdl-protocol-notes.md](awdl-protocol-notes.md) | How AWDL behaves in practice: who decides what, timing, channel sequences, RX/TX pitfalls |
| [skywalk-pf-quic.md](skywalk-pf-quic.md) | The pf → netagent → channel-flow → QUIC chain, and how to diagnose it |
| [opencore-patches.md](opencore-patches.md) | Quirks and kernel patches with side effects; how to write and confirm a patch |
| [bluetooth-ble-advsets.md](bluetooth-ble-advsets.md) | In-memory `bluetoothd` patches with Lilu: UUID gate, vnode identity, boot ordering |
| [methods-and-tools.md](methods-and-tools.md) | The repo's tools, DTrace and log recipes, and mistakes not to repeat |

Older, more detailed analyses (from the Sequoia port) are in [`../research/`](../research/): [awdl-sequoia-port-analysis.md](../research/awdl-sequoia-port-analysis.md) (Tahoe vs Sequoia ABI), [awdl-tx-timing.md](../research/awdl-tx-timing.md), [panic-stale-struct-layout.md](../research/panic-stale-struct-layout.md), [airdrop-master-election-and-multicast.md](../research/airdrop-master-election-and-multicast.md), [ble-advsets-analysis.md](../research/ble-advsets-analysis.md), [real-mac-reference.md](../research/real-mac-reference.md).

Unless stated otherwise, these notes apply to macOS 26.7.2 (25G332), Darwin kernel 25.6, IO80211Family 1200.13.1, OpenCore REL-108, Lilu 1.7.2, firmware `iwlwifi-ty-a0-gf-a0-68`. Offsets and byte patterns change between builds.

Some driver behaviour, counters and `awdlctl` switches named here come from development builds; check [CHANGELOG.md](../../CHANGELOG.md) for what a given release contains.
