# Porting native Wi-Fi and AWDL to other Wi-Fi chips (Realtek, MediaTek, Atheros…)

This is a plan for doing what this project did, on different hardware. It uses a Realtek PCIe card (for example RTL8852BE / RTL8852CE, Wi-Fi 6/6E) as the running example, but the steps are the same for MediaTek or Qualcomm Atheros chips.

Be realistic about the size of the job. The Intel work stands on years of OpenIntelWireless effort: a complete 802.11 stack port and a driver for the chip that already worked with macOS before AWDL was attempted. For a chip with no macOS driver at all, step 1 alone is a large project.

## The pieces

```
macOS IO80211Family  ←→  glue (AirportItlwm*)  ←→  802.11 stack (itl80211 = OpenBSD net80211)  ←→  HAL (ItlIwx…)  ←→  chip firmware
```

Only the HAL is chip-specific. The glue, the 802.11 stack and the AWDL timeline in this repo can be reused as they are. So the plan is: **write a new HAL**, first for normal Wi-Fi, then add the AWDL callbacks.

## Step 1. Pick a source driver to port

| Chip family | Best source | License | Notes |
|---|---|---|---|
| Realtek RTL8852A/B/C, RTL8851B, RTL8922A (Wi-Fi 6/6E/7) | Linux `rtw89` | GPL-2.0 OR BSD-3-Clause | Modern, supports remain-on-channel and P2P on recent kernels |
| Realtek RTL8822B/C, RTL8821C, RTL8723D (Wi-Fi 5) | Linux `rtw88`; OpenBSD `rtwn(4)` for older chips | GPL-2.0 OR BSD-3-Clause / ISC | `rtwn` already speaks net80211, which makes it the easiest start for the chips it supports |
| MediaTek MT7921 / MT7922 | Linux `mt76` | ISC (mostly) | Popular in laptops; P2P and ROC supported |
| Qualcomm Atheros QCA6174, QCA9377 | Linux `ath10k`, OpenBSD `qwx(4)` for WCN6855 | ISC / BSD | Firmware-heavy (most MAC logic in firmware) |

Prefer an OpenBSD or FreeBSD driver when one exists: itlwm's stack *is* OpenBSD net80211, so a BSD driver needs much less translation. Check the license of both the driver code and the firmware files you will bundle (Realtek and MediaTek firmware in `linux-firmware` come with their own redistribution terms; include the license file with the firmware, as `driver/AirportItlwm/iwlwifi-firmware-license` does for Intel).

## Step 2. Normal Wi-Fi through itlwm's framework

1. Fork this repo (or upstream itlwm) and add `itlwm/hal_rtw89/` next to `hal_iwx/`.
2. Implement the HAL service interface the other HALs implement (`ItlHalService`: init, start/stop, `get80211Controller()`, firmware loading, interrupt handling, TX/RX rings). `hal_iwx` and `hal_iwm` are the reference; read `itlwm/hal_iwx/ItlIwx.{hpp,cpp}` top to bottom first.
3. Add the PCI IDs to `Info.plist` personalities and teach the controller to pick your HAL.
4. Get the `itlwm` (Ethernet-style) target working first, then `AirportItlwm`. Expect weeks of work here: DMA, interrupts, power management, rate control, crypto offload.
5. Pass the usual tests: scan, WPA2/WPA3 association, sleep/wake, 24 h of traffic.

Use the same safety nets we use: boot-args that turn features on one at a time, `keepsyms=1 debug=0x100`, and `tools/check-kext.sh` after every build.

## Step 3. Find out what the chip can do for AWDL

AWDL needs these capabilities from the chip and its firmware. Check each one in the Linux driver and its firmware interface before writing code:

| Need | Why | What to look for in the Linux driver |
|---|---|---|
| **Off-channel dwells while associated**, with a precise start notification | AWDL slots are 16 TU long and move between channels; the station link stays up on the home channel | `remain_on_channel` / `cancel_remain_on_channel` ops; firmware "ROC" or "scan offload" with start/end events |
| **A second MAC address** that the chip ACKs | Peers send unicast to `awdl0`'s own MAC | P2P-device or multi-vif support (`NL80211_IFTYPE_P2P_DEVICE`, "port"/"MACID" concepts in Realtek firmware) |
| **Receive frames for a foreign BSSID** | AWDL frames use the AWDL BSSID `00:25:00:ff:94:73`, not your AP's | RX filter control / promiscuous or monitor-like filter per vif |
| **Transmit raw management/action frames and data on a chosen channel**, with TX status | PSF/MI frames and AirDrop data | `mgmt_tx` support, per-frame TX descriptors with rate/retry control |
| **TX timing you can observe** | Frames must leave inside the peer's slot | Some way to know "the radio is on channel X now" (dwell state, firmware event, or a timestamp) |
| A reliable host clock to align to peers | The AW timeline is kept on the host | TSF read or precise host timestamps on RX |

If the chip has P2P-device + ROC + mgmt TX in Linux, it can almost certainly do AWDL with this design. If it can only do one channel at a time with no off-channel support while associated, AWDL is only possible while disconnected from the AP.

## Step 4. Implement the AWDL callbacks

Implement the five callbacks and fields described in [how-awdl-works.md](how-awdl-works.md#the-hal-interface-what-a-new-chip-has-to-provide):

```c
ic->ic_awdl_listen       = rtw89_awdl_listen;       // go to chan for dwell_tu, deliver what is heard
ic->ic_awdl_dwell_state  = rtw89_awdl_dwell_state;  // idle / requested / running (+ channel)
ic->ic_awdl_tx           = rtw89_awdl_tx;           // send one raw frame now, EAGAIN if not on chan
// RX: call ic->ic_vendor_action_handler for AWDL action frames,
//     ic->ic_awdl_data_handler for data addressed to ic->ic_awdl_addr
// Honour ic->ic_awdl_addr / ic_awdl_bssid / ic_awdl_addr_gen / ic_awdl_want_data
```

Map them to the chip:

| Callback | Intel implementation (`hal_iwx`) | Realtek `rtw89` equivalent to study |
|---|---|---|
| `ic_awdl_listen` | P2P-device MAC + PHY context + binding + `SESSION_PROTECTION_CMD` (`iwx_p2p_setup`, `iwx_p2p_roc`) | `rtw89_ops_remain_on_channel`, `rtw89_roc_start` (switches channel for the ROC vif) |
| `ic_awdl_dwell_state` | Session-protection start/end notifications (`ic_awdl_roc_start_us`/`end_us`) | ROC state machine (`rtw89_roc_*` state, channel-switch completion) |
| `ic_awdl_tx` | Frame on the P2P MAC's TX queue with the broadcast station (`iwx_awdl_tx_frame`) | `rtw89_core_tx_write` with the ROC vif's MACID; mgmt TX path |
| `ic_awdl_want_data` | `IN_PROMISC` on the P2P MAC filter | RX filter configuration for the second port (`rtw89_*_set_rx_filter`) |
| Second MAC address | MAC context with `ic_awdl_addr` | A second "port" / MACID with its own address |

Start with RX only (`-itlawdl -itlawdlaw -itlawdllisten` and your listen implementation, `-itlawdlnoinject` while bringing it up). When `awdl0` sees peers, add TX, then data. The counters in `ieee80211com` (`ic_awdl_roc_*`, `ic_awdl_tx_*`) are shown by `awdlctl show` and `scripts/monitor-awdl.sh`; fill them from the start, they are how every problem in this project was found.

## Step 5. Measure, then tune

What made AirDrop work on Intel was measurement, not guessing:

- histogram of "dwell requested → dwell really started" (`AWDLRocDelayHistMs`): Intel's firmware was late by 10 to 75 ms half of the time, which is why the TX pump exists;
- TX outcome by dwell state and band;
- data latency from queue to air (`AWDLDataLatencyMs`);
- who is AWDL master, and whether the peer is aligned with us (`AWDLPeerSync`).

Your chip will have its own quirks. Record a session with `scripts/monitor-awdl.sh`, compare a successful AirDrop with a failed one, and fix the biggest difference first. [docs/test-results.md](test-results.md) shows how that went for the AX210.

## Bluetooth

AirDrop discovery also needs BLE advertising sets. If the chip's Bluetooth is a USB controller that macOS accepts (most Realtek/MediaTek combo cards need a firmware loader kext first), `BlueToolFixup -btlfxadvsets` works the same way as for Intel. If the controller lacks LE extended advertising (Bluetooth 5.0+), AirDrop discovery will not work.

## What to send back

If you get anywhere, even only to "scan works", please open a pull request or a discussion. New HALs are welcome in this repo under GPL-2.0, with the same rules as everything else: tests, CI green, and AI use disclosed.
