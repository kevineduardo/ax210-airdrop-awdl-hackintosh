# How AWDL works in this driver

AirDrop needs three things from the Mac:

1. BLE advertising, so nearby iPhones know an AirDrop-capable device is around. That is Bluetooth: `bluetoothd` plus BlueToolFixup's advertising-sets patch for non-Apple controllers.
2. AWDL (Apple Wireless Direct Link), a peer-to-peer Wi-Fi link that runs next to the normal Wi-Fi connection. Devices meet on agreed channels in short, synchronized time slots called availability windows (AW).
3. IP over AWDL: Bonjour finds `_airdrop._tcp` on `awdl0`, then the phone opens a TCP/TLS connection to port 8770 and runs the AirDrop HTTP exchange (DISCOVER, ASK, UPLOAD).

On a real Mac the Broadcom firmware does most of the AWDL timing. The Intel firmware knows nothing about AWDL, so this driver does that work on the host.

## The layers

```mermaid
flowchart TB
    subgraph user [User space]
        sharingd[sharingd / AirDrop]
        wifip2pd[wifip2pd]
        bluetoothd[bluetoothd + BlueToolFixup patches]
    end
    subgraph kernel [Kernel]
        ip[TCP/IP + Bonjour on awdl0]
        io80211[IO80211Family: AWDL peer manager, election, scheduling]
        awdlif["AirportItlwmAWDLInterface (awdl0)<br/>AW timeline, PSF/MI frames, data path"]
        sta["AirportItlwm (en0)<br/>station interface"]
        net80211["itl80211 (OpenBSD net80211 port)<br/>RX hook for AWDL frames"]
        hal["ItlIwx HAL<br/>P2P-device MAC, listen dwells, TX"]
    end
    fw[Intel AX210 firmware]
    sharingd --> ip
    wifip2pd --> io80211
    ip <--> awdlif
    io80211 <-->|IO80211AWDLProtocol ioctls| awdlif
    awdlif <-->|ic_awdl_* callbacks| net80211
    sta --- net80211
    net80211 <--> hal
    hal <--> fw
```

| Layer | Files | Role |
|---|---|---|
| IO80211Family (Apple) | in macOS | Owns AWDL: peer manager, master election, channel sequence, service discovery. It talks to the driver through the `IO80211AWDLProtocol` interface, the same one Apple's Broadcom driver implements. |
| `AirportItlwmAWDLInterface` | `driver/AirportItlwm/AirportItlwm/AirportItlwmAWDLInterface.{hpp,cpp}` | Our implementation of `IO80211AWDLProtocol`. Answers the AWDL ioctls, keeps the AW timeline on the host, builds and sends the periodic sync frames (PSF) and master indication frames (MI), and moves data frames between `awdl0` and the radio. |
| `AirportItlwmV2` / Skywalk interface | `AirportItlwmV2.cpp`, `AirportItlwmSkywalkInterface.cpp` | Announce the AWDL capability (only with `-itlawdl`) and create `awdl0` when IO80211Family asks for it (`VIRTUAL_IF_CREATE`). |
| net80211 hook | `itl80211/openbsd/net80211/ieee80211_input.c`, `ieee80211_var.h` | Diverts AWDL action frames (vendor-specific, Apple OUI) and AWDL data frames, unicast and group-addressed, to `awdl0` before the station code drops them as foreign BSS traffic. |
| HAL | `itlwm/hal_iwx/ItlIwx.cpp`, `if_iwxvar.h`, `if_iwxreg.h` | Turns "listen on channel X for N TU" and "send this frame now" into Intel firmware commands. |

## The HAL interface (what a new chip has to provide)

The AWDL code above the HAL only uses a handful of callbacks and fields in `struct ieee80211com` (`ieee80211_var.h`). This is the contract a port to another chip must implement:

| Member | Direction | Meaning |
|---|---|---|
| `ic_awdl_listen(ic, chan, dwell_tu)` | AWDL → HAL | Go to `chan` for `dwell_tu` time units, even while associated elsewhere, and deliver what is heard there. |
| `ic_awdl_dwell_state(ic, &chan)` | AWDL → HAL | Is the dwell idle, requested, or actually running (and on which channel)? The TX pump polls this. |
| `ic_awdl_tx(ic, chan, frame, len)` | AWDL → HAL | Send one raw 802.11 frame now, on `chan`. Return `EAGAIN` if the radio is not on that channel at this moment. |
| `ic_vendor_action_handler(ic, m, ...)` | HAL → AWDL | A received vendor action frame (AWDL PSF/MI and friends). |
| `ic_awdl_data_handler(ic, m, ...)` | HAL → AWDL | A received AWDL data frame for `awdl0` (takes ownership of the mbuf). |
| `ic_awdl_addr`, `ic_awdl_bssid`, `ic_awdl_addr_gen` | AWDL → HAL | The MAC address of `awdl0` and the AWDL BSSID. The HAL must receive and ACK frames sent to `ic_awdl_addr`. |
| `ic_awdl_want_data` | AWDL → HAL | An AWDL peer is active: let data frames for `ic_awdl_addr` through (on Intel: `IN_PROMISC` on the P2P MAC). |
| counters (`ic_awdl_roc_*`, `ic_awdl_tx_*`, ...) | HAL → AWDL | Diagnostics shown in `ioreg` and `awdlctl show`. Optional but very useful. |

[porting-to-other-wifi-chips.md](porting-to-other-wifi-chips.md) explains how to implement these on different hardware.

## How the Intel firmware is used

- Listening. Three methods were tried: auxiliary remain-on-channel (`HOT_SPOT_CMD`), scan-based dwells, and a P2P-device MAC. Only the last one works while the station is associated. It copies what Linux `iwlmvm` does for a P2P device: a second MAC context bound to its own PHY context, a broadcast station with a TX queue, and `SESSION_PROTECTION_CMD` to stay on the channel. `-itlawdlp2p` turns it on.
- Receiving data. The P2P MAC delivers only management frames unless `IN_PROMISC` is set. With `-itlawdlpromisc` the driver sets it while an AWDL peer is active, so the firmware delivers and ACKs data addressed to `awdl0`.
- Transmitting. Frames go out on the P2P MAC's queue, and only while the dwell is actually running on the right channel. The firmware starts a dwell with a variable delay (2 to 75 ms in our measurements), so the TX pump (`-itlawdltxpump`) polls the dwell state every millisecond and sends as soon as it starts, instead of at fixed offsets. That change moved most data frames from "> 1 s queued" to "< 5 ms". Details: [research/awdl-tx-timing.md](research/awdl-tx-timing.md).

## The coex backend

`-itlawdlcoex` selects a second, experimental way to run AWDL on the Intel firmware. It is a port of the "coex listener" engine from [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) by retropineaple (GPL-2.0, commit `a5f1eb9`). Without the switch nothing of it runs and the P2P path above works exactly as before.

### How it differs from the P2P path

| | P2P path (default) | Coex backend (`-itlawdlcoex`) |
|---|---|---|
| Second MAC context | `P2P_DEVICE`, MAC 1 | `LISTENER`, MAC 1, TSF B |
| Channel | Moves to each AW slot's channel for a short dwell (`SESSION_PROTECTION_CMD`), then back | Stays on one channel of its own for as long as en0 is associated: 149 at 80 MHz when the firmware allows it, else 6 at 20 MHz, else the station's channel |
| Who shares the radio | The host, by asking for dwells ahead of each slot (`itlawdllead`) | The firmware itself (`DYNAMIC_QUOTA`): two PHY contexts and two bindings, scheduled by the firmware |
| Timing on the host | AW timeline, listen timer, TX pump polling the dwell state | A HAL task every 1 to 113 ms; no dwell to wait for, so a frame can leave as soon as the peer is on our channel |
| Channel rules | The channel list macOS asks for | The firmware's own channel map, after answering its country notice (`MCC_UPDATE_CMD`, as Linux does), plus beacon hints for passive channels. DFS and no-ad-hoc channels are never used to start a transmission |
| Frames our sync frames advertise | The slots macOS chose | All 16 slots on the listener's channel (op class 128 at 80 MHz) |
| Data path | `flushData` pushes frames during a dwell | The HAL pulls frames from `awdl0` (`ic_awdl_coex_pop`) and sends them from the listener's queue, gated by the peer's schedule |
| Firmware objects | MAC 1, PHY 1, station 3 + its queue | MAC 1, PHY 1, station 2 (listener) + its queue; station 3 for the iPhone when Block Ack is on (`-itlawdlcoexba`) |

Because both paths use MAC 1, PHY context 1 and firmware station 3, they never run in the same boot. The front end decides before the HAL attaches (`ic_awdl_coex_on`); with it set the HAL never installs `ic_awdl_listen`, `ic_awdl_tx` and `ic_awdl_dwell_state`, refuses `iwx_awdl_listen`, and reports the P2P path as disabled. `AWDLBackend` on the controller and on `awdl0` shows which backend runs (`coex`, `p2p`, `scan` or `none`). The AWDL build guard still applies: on a build it blocks, `-itlawdlcoex` does nothing either.

### Where the code is

| Part | Files |
|---|---|
| Engine (HAL) | `itlwm/hal_iwx/ItlIwxAWDLCoex.hpp` (state) and `ItlIwxAWDLCoex.cpp` (functions). ItlIwx.cpp includes both, so the engine shares its statics; hooks in `iwx_tx`, `iwx_rx_frame`, `iwx_rx_tx_cmd` and friends are one-line calls under `#ifdef AIRPORT_WCL` |
| Pure helpers | `include/AWDLCoex/*.hpp` (frame parsers and builders, peer schedule, regulatory, Block Ack), copied from ForbiddenItlwm, host-tested in `tests/unit/awdlcoex/` |
| Hooks | End of `iwx_run` (add the listener), RUN exit in `iwx_newstate_task` (remove it), `iwx_mcc_update` (country notice), `iwx_rx_mpdu_mq` and `iwx_rx_frame` (receive), `iwx_get_channel_width`, `iwx_get_ctrl_pos` and `iwx_phy_ctxt_cmd` (the listener's 80 MHz PHY context) |
| Seam to the AWDL interface | `ic_awdl_coex_*` in `ieee80211_var.h`: the HAL writes the listener's channel, width and op class; the AWDL interface provides the pull callbacks |
| Beacon hints | `ieee80211_input.c` marks channels where an AP beacons on its own channel (`ic_beacon_hint`) |

### Differences from ForbiddenItlwm

- Received frames go to `awdl0` through the existing `ic_vendor_action_handler` and `ic_awdl_data_handler`, without the four FCS bytes ForbiddenItlwm adds. Data frames are not required to pass ForbiddenItlwm's IPv6 decoder, because `awdl0`'s input path here also takes QoS data and A-MSDU subframes; the decoder only counts (`ITLAWDLCoexDataRxNotIPv6`).
- The listener's address is `awdl0`'s address (`ic_awdl_addr`) from the start, instead of being learned from the first sync frame macOS builds. A change is applied to the firmware from the engine's task.
- The first PHY command for the listener's own channel removes and adds PHY context 1 when the band changes (it was added on channel 1 at firmware start and nothing binds it yet). The P2P path found that band changes need this on the AX210. ForbiddenItlwm sends MODIFY. Later moves (`awdlCoexRetarget`) keep ForbiddenItlwm's MODIFY.
- Counters are published on the AirportItlwm controller, not on the registry root.
- Sync frames come from this driver's own builder (`buildAWDLFrame`): when the listener has a channel, every slot of the channel sequence, the next AW channel and the master channel name it, with op class 128 at 80 MHz. A PSF is left out where the P2P path leaves it out (AF TX mode 3 and not the sync master).
- `awdl0`'s data is framed by `AirportItlwmAWDLFrame.hpp`, which the P2P path's `flushData` now uses too (same bytes). A host test checks that ForbiddenItlwm's decoder, which guards the listener's queue, accepts it.
- The receive Block Ack responder is off unless `-itlawdlcoexba` is set; ForbiddenItlwm runs it whenever its data mode is on.
- Send Block Ack (`-itlawdlcoexbatx`), the peer pause (`-itlawdlcoexpeerpause`) and the same-channel probe window (`-itlawdlcoexsamechprobe`) are off by default, as in ForbiddenItlwm. ForbiddenItlwm's optional refill on reclaim (`-itlawdlcoexbarefill`, no gain measured there), its shared-channel ask switch and its receive-filter hysteresis switch are not ported.
- The AW-timed idle sync frames (`-itlawdlawsync`) and the service-record merge are later phases of the port.

### Block Ack

Without Block Ack every AWDL data frame is sent and acknowledged on its own, which caps a transfer at a few MB/s even at VHT 80 MHz. With it, a sender puts up to 64 frames in one A-MPDU and the receiver answers them all with one Block Ack.

**Receive (`-itlawdlcoexba`).** When the iPhone wants to send in A-MPDUs it sends an ADDBA request to `awdl0`'s address. The receive hook takes it (from a peer heard on our channel in the last 60 s) and the engine's task:

1. adds a firmware station for the iPhone (station 3, a `LINK` station bound to the listener's MAC context, 80 MHz when the listener is at 80 MHz, two streams, A-MPDU density 0),
2. opens a firmware receive session on the request's TID with its window and start sequence (`ADD_STA` with `ADD_BA_TID`; the firmware must answer with a valid BAID),
3. sends the 33-byte ADDBA response from the listener's queue, retried every 20 ms until it is queued; the iPhone's acknowledgement of it is recorded (`ITLAWDLCoexBaPeerAccepted`).

From then on the firmware acknowledges the iPhone's A-MPDUs itself. Frames reach `awdl0` in the order they arrive; there is no host reorder buffer, as in ForbiddenItlwm (the order is measured, see `ITLAWDLCoexRxSeq*`). A session ends when the iPhone sends a DELBA, or after 60 s without data on its TID, with our own DELBA (reason 39, confirmed by its acknowledgement, at most 3 tries or 3 s). The iPhone's station goes after 60 s without hearing it, after 10 minutes without data, when the listener moves or is removed, and with every firmware restart.

While a receive session is open, the listener's MAC context drops promiscuous receive: the firmware does not acknowledge a session's frames with it on. A listener only gets group data in promiscuous mode, so mDNS multicast is not received while a session is open. Promiscuous receive comes back when the last session ends.

**Send (`-itlawdlcoexbatx`).** Sending in A-MPDUs needs the iPhone's agreement and a firmware queue that aggregates:

1. The trigger is the iPhone acknowledging our receive answer, or a bulk stream (200 payload frames of 1200 bytes or more in a second, 50 in a quarter second once agreed) to a peer heard on our channel; the iPhone's station is added for it if receive Block Ack has not done so yet.
2. The station gets a firmware aggregation queue on TID 0 (`SCD_QUEUE_CFG` for station 3). The firmware hands out a number no host queue owns; the request carries its owner (generation, listener, peer station), and an answer that comes back after any of them changed installs nothing.
3. The firmware's rate control (`TLC_MNG_CONFIG_CMD` for station 3) gets high rates only: VHT MCS 4 to 9, one or two streams, at 80 MHz when the listener is at 80 MHz, else HT MCS 4 to 7 at 20 MHz. The iPhone leaves our channel often, and rate control would otherwise read that as a bad link.
4. Our ADDBA request (TID 0, window 64, start sequence from the queue) goes out from the listener's queue at most every 200 ms, never while the iPhone's schedule says it is away, 40 per round, then 20 s of rest; asking pauses after 30 s without traffic.
5. When the iPhone agrees, TID 0 is enabled for aggregation on its station (`ADD_STA` with `TID_DISABLE_TX`). A window under 64 in its answer caps the frames outstanding.

From then on the data pump converts payload frames for the iPhone into QoS data on TID 0 with our own sequence numbers and puts them on the aggregation queue, without a fixed rate. The firmware builds the A-MPDUs and reports each Block Ack (`BA_NOTIF`); those notifications, which the driver used to drop for any station but en0's, now reclaim the queue after every record is checked.

**Send gate and peer pause.** Outside an aggregated bulk send, the phase 2 send gate holds frames for the iPhone while its schedule says it is away or dozing. During a bulk send the gate stands aside and, with `-itlawdlcoexpeerpause`, the firmware is told to stop sending to the iPhone's station (`DISABLE_TX`) while the schedule says it is away, then to start again; frames already queued wait in the firmware instead of being retried into empty air. A resume is confirmed with the firmware's status and retried every 50 ms; after 5 failures the station is removed.

**Ordering rule.** While the aggregation queue is paused, full or still draining, every unicast frame for the iPhone waits behind it, short ones included (`-itlawdlcoexnoorder` turns this off). Otherwise a short QUIC packet could overtake held payload on the single-frame queue, and QUIC takes three packets acknowledged ahead of one as a loss.

**Probe window.** With a receive session open, promiscuous receive is off, and on some setups the iPhone's answer to our request then never arrives. Once per dialog, after three unanswered asks, promiscuous receive is opened for 250 ms. On en0's own channel that only happens with `-itlawdlcoexsamechprobe`.

### Channel fallback

The listener's channel is picked from the firmware's own channel map, after the firmware's country notice has been answered: a channel qualifies when it is active, not passive (or an AP was heard beaconing on it, the beacon hint), not a radar (DFS) channel, and allows ad hoc. At 80 MHz all four 20 MHz channels of the block have to qualify. The default order is ForbiddenItlwm's: 149 at 80 MHz, else 6 at 20 MHz, else en0's own channel (a shared listener, which can never send when en0 is on a DFS channel).

`itlawdlcoexchan=` adds 44 at 80 MHz (36 to 48, centre 42) and changes the order: with `149`, 44 is tried before falling back to 6; with `44`, 44 comes first. This is for countries and setups where 149 to 161 stay passive, such as the test machine in Portugal with its AP on channel 100: without it the listener ends on 6 at 20 MHz, and Block Ack there tops out far lower. The listener's PHY context then uses control position 1 above centre 42; sync frames name 44 in every slot with op class 128. When the map changes, the engine moves the listener (at most every 10 s to an 80 MHz channel, at once down to 6).

### Safety rules kept from ForbiddenItlwm

- Every firmware command can sleep in the command gate. After each one the engine checks its token, the firmware generation and the stop flag before it touches state again.
- A command that fails without a readable answer leaves the firmware state unknown. The engine then stops using that part (listener, own channel, Block Ack) until the firmware restarts.
- Frames on the listener's queues never reset en0's TX watchdog and never count as en0 errors.
- While the listener receives promiscuously, frames that belong to it never reach net80211.
- `LOW_LATENCY_CMD` is not ported: it took Wi-Fi down on an AX211.

### Phases

| Phase | What it adds |
|---|---|
| 0 | Backend selection, P2P path off, firmware capabilities published (`ITLAWDLCoexFw*`), beacon hints, helpers and host tests, two HAL fixes (Block Ack notifications for other firmware stations are reclaimed; `iwx_flush_sta_tids` no longer reads past `sc_tid_data`) |
| 1 | The listener: its own PHY context and binding on 149 at 80 MHz (or 6 at 20 MHz, or en0's channel), country answers (`MCC_UPDATE_CMD`) and channel moves, receive of AWDL action and data frames into `awdl0` (data only from peers heard on our channel in the last 60 s), promiscuous isolation so the station never sees the listener's frames. No transmit |
| 2 | Transmit: the listener's own firmware station (2) and queue, macOS's sync frames (MI and PSF alternating, every 45 ms idle and 100 ms during data, built by the AWDL interface with all 16 slots on the listener's channel), `awdl0`'s data pulled one frame at a time (`ic_awdl_coex_pop`) with fixed rates (6 Mbit/s group, 24 Mbit/s, then HT MCS 4, 5, 7, then VHT80 2SS MCS 4 and 7) and a rate ladder fed by the TX responses, the stuck-frame watchdog, and the send gate that holds frames for the iPhone while its schedule says it is away or dozing |
| 3 | Receive Block Ack (`-itlawdlcoexba`): ADDBA answers, the iPhone's station and firmware session, DELBA and idle teardown, promiscuous receive off while a session is open, arrival-order counters |
| 4 | Send Block Ack and A-MPDU (`-itlawdlcoexbatx`): aggregation queue on TID 0, firmware rate control at VHT80 two streams, our ADDBA request, QoS conversion in the data pump, Block Ack notifications counted, peer pause (`-itlawdlcoexpeerpause`), ordering rule, probe window |
| 5 | Optional, each off by default: 44 at 80 MHz as a fallback or first choice (`itlawdlcoexchan=`) |

Hardware checks per phase: [test-results-tahoe.md](test-results-tahoe.md#coex-backend-to-test-on-hardware).

## The availability-window timeline

AWDL devices agree on a timeline: a sequence of 16 slots, each one AW-period long, each assigned to a channel (for example `149 0 0 0 6 0 ...`, where 0 means "not on AWDL"). The elected master defines the timeline and everyone else synchronizes to it from the PSF/MI frames.

The driver keeps this timeline in host microseconds (`AirportItlwmAWDLInterface`, "Host-side availability window timeline"). Before each slot where AWDL is on a channel, it requests a listen dwell `itlawdllead` microseconds early, transmits PSF/MI and queued data during the dwell, and tracks when it last heard each peer (IO80211Family drops a peer after about 6 s of silence).

Testing turned up two problems. Both are fixed:

- The Mac wins the election but the iPhone keeps its own timeline. The Mac and the iPhone have few or no common slots in low-power mode and the AirDrop upload never arrives. `-itlawdlfollow` makes the Mac adopt the peer's timeline in that case. See [research/airdrop-master-election-and-multicast.md](research/airdrop-master-election-and-multicast.md).
- Multicast never reached `awdl0`. The net80211 hook only accepted frames addressed to `awdl0`'s MAC, so mDNS queries from the iPhone were dropped and the Mac only reappeared in the AirDrop list when Bonjour re-announced by itself (40+ s). Group-addressed AWDL data with the AWDL BSSID is now delivered to `awdl0`.

## Sequoia vs Tahoe

The AWDL logic is the same for both. What differs is the IO80211Family ABI: class layouts, vtable slot offsets, and whether some objects have virtual methods at all. `driver/AirportItlwm/AirportItlwm/AirportItlwmAWDLConfig.h` picks the ABI at compile time (`AIRPORT_AWDL_ABI_SEQUOIA` or `AIRPORT_AWDL_ABI_TAHOE`), and the version shims in `include/Airport/` select the matching headers. The full list of ABI-dependent places is in [research/awdl-sequoia-port-analysis.md](research/awdl-sequoia-port-analysis.md).

## BLE advertising sets

Without advertising sets, `bluetoothd` refuses to run the BLE advertising AirDrop and Handoff need when the controller is not an Apple one. BlueToolFixup patches `bluetoothd` in memory as its pages are loaded. On macOS 15.8.1 the patch table is applied only if the `bluetoothd` binary's UUID matches the one the patterns were verified against; otherwise nothing is patched. Details: [research/ble-advsets-analysis.md](research/ble-advsets-analysis.md) and [research/ble-advsets-sequoia-changes.md](research/ble-advsets-sequoia-changes.md).

## Further reading

- Milan Stute et al., *One Billion Apples' Secret Sauce: Recipe for the Apple Wireless Direct Link Ad hoc Protocol* (MobiCom 2018), and the [Open Wireless Link](https://owlink.org/) project: the public description of AWDL frames, election and synchronization.
- Linux `iwlmvm` P2P-device code (`drivers/net/wireless/intel/iwlwifi/mvm/`), for the firmware commands used by the listen path.
