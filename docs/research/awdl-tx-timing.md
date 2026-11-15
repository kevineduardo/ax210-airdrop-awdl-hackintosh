# AWDL TX from Mac to peer: cause and fixes

*Engineering notes written during the port (November 2026). Line numbers refer to the tree at the time of writing; the modified files later gained a two-line license notice at the top, so numbers may be off by two.*

Date: 2026-11-10. Tree: `driver/AirportItlwm/`.

**Builds:** all 4 combinations compile (Sequoia/Tahoe × Debug/Release), with `-derivedDataPath build/dd-port`.

**Hardware tests:** none yet. All new behaviour is behind boot-args, and without them the kext does exactly the same as the test 4 kext.

---

## 1. Diagnosis

### 1.1 What the data shows

Sources (captured logs, not published):

- The test 4 snapshot (`ioreg`).
- The kernel timeline.
- New read-only sampling of the test 4 kext while it was still loaded (ROC delay samples, 2026-11-10).

| Fact | Evidence |
|---|---|
| The AWDL sequence has 16 slots of 4 AW × 16 TU = 65.5 ms, in a 1.048 s supercycle | `AWDLPresenceMode 4`, `AWDLChanSeq " 100 0 44 0 0 0 0 0 6 0 44 0 0 0 0 0"`; the peer's is the same (`AWDLPeerChanSeq`) |
| Only 3 of the 16 slots can be used for TX: slot 2 (44), slot 8 (6) and slot 10 (44) | Slot 0 is channel 100, the STA channel, and is always skipped (`AWDLListenHomeChannel 100`; `scheduleOwnListen` does `chan == home → continue`). The other 12 are 0 |
| Maximum gap between two TX opportunities: slot 10 → slot 2 of the next cycle = 524 ms | Arithmetic on the sequence above |
| Within each dwell, TX happens at fixed points: PSF at +1 ms and MI at +4 ms from the planned slot start, plus a single `flushData()` at +4 ms | `scheduleTx` / `txTimerFired` (crazyhulk's `AirportItlwmAWDLInterface.cpp`) |
| `iwx_awdl_tx` only accepts while the firmware P2P session is running on the channel; otherwise it returns EAGAIN | `ItlIwx.cpp` `iwx_awdl_tx`; `AWDLTxFwBusy 1610` vs `AWDLTxFwOk 831`; `AWDLTxRefused 1500` with `AWDLTxLastErr 35` |
| The firmware starts the dwell after a highly variable delay from the request | Live sampling of `AWDLRocDelayUs` (request → START; n = 44): 2 to 5 ms: 41 %, 5 to 10 ms: 5 %, 10 to 20 ms: 11 %, 20 to 40 ms: 18 %, 40 to 80 ms: 25 % (maximum seen 74.5 ms). The test snapshot had 79.6 ms |
| Dwell duration, counted from START, is fixed (~66.5 ms) | `AWDLRocLastUs` ≈ 66 500 in every sample |
| Data does eventually go out, but late | `AWDLDataTx 402 = AWDLDataTxQueued 402`, `DropFull 0`; `AWDLDataTxDeferred 110` (flush attempted before the dwell had started) |
| Unacknowledged unicast is rare | `AWDLTxFwFail 19`, status 0x83 (long retry limit) |
| RX is fine | `AWDLDataRx 324`, `RxBad 0`, `RxNoBuf 0` |

### 1.2 Main cause: TX tied to fixed instants while the dwell start is unpredictable

The scheduler requests the dwell 6 ms before the slot (`kListenLeadUs`) and tries to transmit exactly at +1 ms and +4 ms. The fixed points only work if the firmware starts the dwell within ~7 ms (PSF) or ~10 ms (MI and data). According to the samples, that happens in ~45 % of dwells.

In the other 55 %, the dwell starts 10 to 75 ms later, and everything scheduled fails with EAGAIN. Data then waits for the next dwell, which is 131 to 524 ms away and again has a ~55 % chance of failing. Even when the dwell starts on time, the flush only runs at +4 ms. Anything queued mid-slot waits for the next slot: this is the case for TCP ACKs and for TLS/HTTP responses to packets the iPhone just sent.

Expected latency from enqueue to send: hundreds of ms, often more than 1 s. In order, this explains:

- Outgoing SYN with up to 9 attempts: the SYN waits and the stack retransmits (`SYN in/out: 1/9` in the log).
- iPhone retransmissions and `dup bytes in`: the Mac's ACKs arrive too late.
- `bytes out` far below `bytes in`.
- RST from the iPhone after 3 to 5 s, and finally `UPLOAD connection is not active`.

RX works because it does not depend on these instants: it only needs the dwell to overlap the peer's slot.

### 1.3 Why the dwell start varies (hypothesis)

The P2P session (`SESSION_PROTECTION_CMD`, conf `P2P_DEVICE_DISCOV`) has to be scheduled by the firmware around the BSS connection. Here the BSS is on channel 100 at 80 MHz with a 102.4 ms beacon interval. The distribution up to ~75 ms is consistent with the firmware only leaving the BSS channel after the current quantum or TBTT. There is no way to ask the firmware for an exact instant. So the driver has to react to the actual instant the dwell starts, instead of assuming it starts on time.

### 1.4 Secondary factors

1. **The slot on the home channel (100) is never used.** The iPhone is also on 100 in that slot, because it is its infra channel. A real Mac transmits AWDL there. That is 25 % of opportunities lost.
2. **IO80211Family does not see our data traffic.** The log shows "AWDL on … with no TX … or RX" and `HasActAirDrop 0`, and the scheduler stays in Low Power (22 %). Our data goes through the legacy awdl0 `ifnet` and not through the IO80211 Skywalk datapath, so the peer manager never moves up to a mode with more slots. Not fixed now: it would require feeding the IO80211 datapath statistics, which have not been reconstructed. Left as a hypothesis.
3. **0x83 (19 cases)**: unacknowledged unicast. Consistent with sends where the dwell was active but the peer was no longer on the channel (a late dwell spilling into the next slot). Fix 2.1 only sends when `peerChannelAt(now) == dwell channel`.
4. **Sequoia vs Tahoe:** nothing found is Sequoia-specific. The TX path (`hal_iwx`) is the same on both. The difference from crazyhulk's result probably comes from the network: with the infra on DFS 100/80 MHz, firmware scheduling is delayed more and the home channel enters the sequence. crazyhulk's tests used channel 161.

---

## 2. Changes

By default, all behaviours are off. Diagnostics are always on and only count.

### 2.1 New boot-args

| Boot-arg | Default | Effect |
|---|---|---|
| `-itlawdltxpump` | off | **TX pump.** During each dwell's window (from the planned start until the requested duration + 100 ms), a high-priority timer polls the firmware every `itlawdlpumpus`. As soon as the dwell is running on the right channel and the peer is on that channel, it sends the PSF, then the MI, and drains the data queue. Packets queued during the dwell go out immediately (for example, TCP ACKs), without waiting for the next slot. The window ends when the firmware reports END |
| `itlawdlpumpus=<µs>` | 1000 | Pump polling period (250 to 10 000) |
| `itlawdllead=<µs>` | 6000 | How far ahead of the slot the dwell is requested (1000 to 60 000). The requested dwell becomes `run × slot + lead`, so a larger lead covers the slot even when START arrives late (see 3.2) |
| `itlawdldwellx=<TU>` | 0 | Extra TU added to each dwell (0 to 200) |
| `-itlawdlhometx` | off | **Experimental.** Also uses slots whose channel is the STA channel (here 100): the P2P MAC dwells on its own home channel. Risk: the firmware may refuse P2P sessions or TX on a DFS channel. The per-band counters show it (`5g-mid`) |

All of them can be changed at runtime, without rebooting, with the new `awdlctl` tool (see 2.4). This only works with `-itlawdllisten`, like the existing tunables.

### 2.2 New diagnostics (always on, in `ioreg -r -c AirportItlwmAWDLInterface -l`)

| Property | Content |
|---|---|
| `AWDLTxModes` | Active switches |
| `AWDLRocDelayHistMs` | Histogram of request → START delay (firmware) |
| `AWDLRocStartVsSlotMs` | Histogram of actual START − planned slot start (<0 = early) |
| `AWDLTxVsDwell` | Calls to `iwx_awdl_tx` by type (AF/data) × dwell state (requested without START / running on the channel / running on another channel / inactive) |
| `AWDLTxOkFailByBand` | Ok/fail completions by dwell band: 2.4G, UNII-1, 5G-mid/DFS, UNII-3 |
| `AWDLDataLatencyMs`, `AWDLDataLatencyMaxMs` | Histogram of data enqueue → firmware, and the maximum |
| `AWDLTxPump` | Windows, ticks, windows without a dwell, ticks with the peer off-channel, immediate flushes, AFs sent after +4 ms |

### 2.3 Files changed in this round

| File | Change |
|---|---|
| `itl80211/openbsd/net80211/ieee80211_var.h` | +18 lines: `ic_awdl_dwell_state()`, ROC delay histogram, START and END instants, TX × state histogram, ok/fail by band |
| `itlwm/hal_iwx/ItlIwx.cpp` | `iwx_awdl_dwell_state` (reads `sc_aux_roc_running/pending`), `iwx_awdl_band`, `iwx_awdl_delay_hist`. Counters in `iwx_awdl_tx`, in P2P queue completions and in session START/END. Does not change any driver decision |
| `AirportItlwm/AirportItlwmAWDLInterface.hpp` | Pump members, switches and diagnostics. `sendAWDLFrame` now returns `bool` |
| `AirportItlwm/AirportItlwmAWDLInterface.cpp` | See list below |
| `tools/awdlctl.c` (new) | Command-line tool for runtime A/B and for reading the diagnostics |

Changes in `AirportItlwmAWDLInterface.cpp`:

- boot-arg parsing in `init()`;
- `pumpTick`, `pumpCanSend` and `noteRocStart`;
- `scheduleTx` with pump mode;
- `enqueueData` with timestamp and immediate flush in pump mode;
- `noteDataSent`;
- `publishTxDiag`;
- `scheduleOwnListen` / `listenTimerFired` with `fHomeTx` and `fDwellExtraTU`;
- `kListenLeadUs` becomes `fListenLeadUs` (default 6000, same as before);
- new keys in `setProperties`.

### 2.4 `awdlctl`

Source in `tools/awdlctl/awdlctl.c`; binary at `tools/awdlctl/awdlctl` (see `tools/awdlctl/Makefile`). Build:

```sh
clang -O2 -framework IOKit -framework CoreFoundation -o tools/awdlctl/awdlctl tools/awdlctl/awdlctl.c
```

Usage:

```sh
awdlctl show            # show the diagnostics
awdlctl pump on|off
awdlctl hometx on|off
awdlctl lead 30000
awdlctl dwellx 0
awdlctl pumpus 1000
```

Tested against the current kext: `show` works.

### 2.5 Checks done on the built binaries (resolve open items from the port notes)

- **`AirportItlwmAWDLInterface` vtable (Sequoia Release)**, read with `otool -rv`:
  - slot 0xe60 = our `init`;
  - 0xe68 = `IO80211VirtualInterface::setLinkState`;
  - 0xf18 = `freePendingPackets`;
  - 0xf98 = `_RESERVED15`;
  - 0xfa0 and 0x1190 = our AWDL methods.

  The size is 0x11a8 + 8 of alignment, i.e. 16 + 0x1198, as expected.
- **`nm -u` of Sequoia Release:** every undefined symbol exists as exported (0x0f) in some entry of the 24H32 `BootKernelExtensions.kc`. This includes `IO80211Buffer::{returnBuffer,setLength,getBytesNoCopy,getCapacity}`, `IO80211Controller::setAwdlInterface` and `IO80211AWDLProtocol` C2/D2/gMetaClass.

---

## 3. Recommendation for the next test (a single reboot)

### 3.1 Boot-args

```
-v keepsyms=1 debug=0x100 -btlfxadvsets -itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000
```

(Boot-args unrelated to Wi-Fi and Bluetooth are omitted.)

These are the current boot-args plus `-itlawdltxpump itlawdllead=30000`. The kext to install is `build/dd-port/Build/Products/Release/Sequoia/AirportItlwm.kext`.

### 3.2 Why `lead=30000`

With START arriving 4 to 75 ms after the request, the part of the slot covered is roughly 65.5 − max(0, delay − lead) ms.

- **lead 6 ms:** full coverage in only ~45 % of dwells; with 40 to 80 ms of delay it drops to 0 to 30 ms.
- **lead 30 ms:** full coverage in ~65 to 70 % of dwells; in the worst case seen (75 ms), still ~20 ms.

The dwell stays at ~93 TU (below the 160 ceiling). The pump uses whatever part overlaps. A larger lead (≥50 ms) would start colliding with the next dwell in slots 8 → 10, which are 131 ms apart, and would return EBUSY.

### 3.3 A/B within the same boot (with `awdlctl`)

1. **Phase A**, boot-arg configuration: AirDrop iPhone → Mac. Save `awdlctl show` and the full `ioreg`.
2. **Phase B**: `awdlctl lead 6000` (pump with the old lead). Repeat. Isolates the effect of the lead.
3. **Phase C**: `awdlctl pump off` (test 4 behaviour, with lead 6000). Repeat. Gives the baseline with the new diagnostics.
4. **Phase D, optional and experimental**: `awdlctl pump on; awdlctl lead 30000; awdlctl hometx on`. Repeat. Check `AWDLTxOkFailByBand` (`5g-mid`) and `AWDLRocStartFail`. If infra Wi-Fi degrades, go back to `awdlctl hometx off`.

**What to expect if the analysis is right:**

- in phase A, `AWDLDataLatencyMs` concentrates below 100 ms;
- `AWDLTxVsDwell` shows most attempts in "run" (today most fall in "req");
- `AWDLTxFwBusy` grows much less;
- TCP connections on awdl0 stop getting RST.

### 3.4 Not recommended yet

- **`itlawdldwellx`**: only if phase A shows dwells ending before the end of the slot.
- **`-itlawdlhometx` as a boot-arg**: use `awdlctl` so it can be turned off without rebooting.

---

## 4. Build

All four commands were run and all returned `BUILD SUCCEEDED`. From `driver/AirportItlwm`:

```sh
for s in AirportItlwm-Sequoia AirportItlwm-Tahoe; do for c in Debug Release; do
  xcodebuild -project itlwm.xcodeproj -scheme $s -configuration $c ARCHS=x86_64 \
    CODE_SIGNING_ALLOWED=NO -derivedDataPath build/dd-port build
done; done
```

Products in `build/dd-port/Build/Products/{Debug,Release}/{Sequoia,Tahoe}/AirportItlwm.kext`.

No new warnings in the touched files; the ones that appear already existed (`-Winconsistent-missing-override`, `-Winvalid-offsetof`).

Notes:

- The build generates `include/FwBinary.cpp` and `itlwm.xcodeproj/project.xcworkspace` inside the kext tree. They are build artifacts and are excluded from the diff.
