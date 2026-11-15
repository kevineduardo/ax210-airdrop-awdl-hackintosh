# Intermittent AirDrop iPhone → Mac: success (test 6) vs. failure (test 7)

*Engineering notes written during the port (November 2026). Line numbers refer to the tree at the time of writing; the modified files later gained a two-line license notice at the top, so numbers may be off by two.*

Date: 2026-11-11. Tree: `driver/AirportItlwm/`. Kext loaded during the analysis: Release `7F4D5A99`.
Log excerpts used here come from captured logs (not published).

## Summary

- **The failure is not caused by the pump being on from boot.** What changed between the two boots was the outcome of the **AWDL master election**:
  - in the success, the iPhone was master and the Mac followed its timeline;
  - in the failure, the Mac became master (metric 539 vs ~530 for the iPhone) and the iPhone did **not** start following the Mac's timeline.
- With two different timelines, the Mac's **Low Power** sequence (3 active slots out of 16) no longer has any slot in common with the iPhone. The Discovery and Idle sequences (6–8 active slots) still overlap by chance.
- In the failure, DISCOVER and ASK went through in Discovery/Idle. The Mac switched to Low Power 0.83 s after the accept and the UPLOAD never arrived.
- This is the main hypothesis, **not proven**: kext 7F4D5A99 does not publish the offset between timelines. It is based on the election (proven by the logs), the dwell distribution (inference) and TCP behaviour.
- Driver changes:
  - new diagnostic `AWDLPeerSync`, always on, which measures the offset and shows who the peer says is master;
  - switch `-itlawdlfollow` / `awdlctl follow on|off` (off by default): when the Mac is master and the peer does not follow it, the Mac adopts the peer's timeline;
  - two direct fixes (see section 4).
- Also delivered: `scripts/monitor-awdl.sh`, tested without AirDrop (start, stop on timeout, Ctrl-C, SIGTERM).

---

## 1. Success vs. failure

### 1.1 Timeline

| | Success (00:29, previous boot) | Failure (00:38, current boot) |
|---|---|---|
| Pump | enabled at runtime (`awdlctl`) | enabled via boot-args |
| awdl0 MAC | (randomized, redacted) | (randomized, redacted) |
| Mac election metric | **514** | **539** |
| Metric of the test iPhone | 530 | does not appear in this boot's logs (530/537 in the previous one) |
| AWDL master at accept | **iPhone** (`HC:1 RMet:530 SMet:514`) | **the Mac itself** (`HC:0 RMet:539`), unchanged from 00:35:33 to 00:40 |
| `AWDLSyncUpdates` (driver followed a master) | > 0 (`AWDLSyncSeqErr` published) | **0** |
| AirDrop Bonjour server | running since 00:26:24 | started at 00:38:03.9, 8 s before the accept |
| Mac sequence at accept | Low Power, stable for minutes | Idle; switches to Low Power **0.83 s later** |
| ASK → accept | 31.520 → 33.459 | 09.433 → 12.172 |
| "UPLOAD connection is not active" | **yes, ×4** | yes, ×4 |
| UPLOAD received | 33.768 (0.31 s after the accept) | **never** (connection closed for inactivity at 00:38:42) |
| End | Importing END 38.362 | — |

Mac sequences (kernel `AWDL ON:` line):

- Discovery: `[44 44 44 44 0 0 0 0 6 44 44 44 0 0 0 0]`
- Idle: `[44 44 44 0 0 0 0 0 6 44 44 0 0 0 0 0]`
- Low Power: `[100 0 44 0 0 0 0 0 6 0 44 0 0 0 0 0]`
- iPhone (seen by the driver at 00:39:30): `[6 0 149 0 0 0 0 0 6 0 44 0 0 0 0 0]`

In the failure, the IO80211 scheduler went: Low Power → Discovery (00:38:04.630, `BonjourServiceAdded`) → Idle (07.000) → Low Power (12.999, `IdleTimeout`). This is a fixed timer chain, ~8.4 s after the Bonjour server starts. A first transfer right after opening AirDrop almost always lands on top of it.

### 1.2 "UPLOAD connection is not active" is not the symptom

The message appears in all three attempts, including the successful one, always 1 ms after the accept. So does `Cancelling ASK connection`. ASK and UPLOAD use the same TCP connection (same remote port, 55918 in the success). What distinguishes the failure is that `Received UPLOAD request` does not follow.

### 1.3 TCP on the ASK/UPLOAD connection

| | Success (port 55918) | Failure (port 53045) |
|---|---|---|
| bytes in/out | 1 060 166 / 1074 | 48 128 / 1081 |
| packets in/out | 755 / 4 | 36 / 3 |
| `pkt rxmit` | 0 | **1** |
| `dup bytes in` | 24 744 | 1412 |
| `ooo pkts` | 253 | 0 |
| rtt | 31 ms | 4 ms |
| RST in/out | 0/1 | 0/1 (from the Mac, when closing for inactivity) |

Reading the failure:

- Up to the accept the connection was fine (48 kB of ASK received, rtt 4 ms, 0 retransmissions).
- The ASK response was retransmitted **once** and then acknowledged. Had it not been acknowledged, there would have been 4 or more retransmissions in 30 s.
- So the iPhone did receive the response, a few hundred ms late, and from then on **nothing from the iPhone reached the Mac** for 30 s.

### 1.4 Outgoing Mac → iPhone connections in the failing boot

With the Finder AirDrop window open, `sharingd` tried to connect to the iPhone (`IPv6#5557b140`, the same address as the incoming connections) every 5 s:

- 00:36:32 → 00:38:01, Mac master in Low Power: 17 attempts. 10 stayed in `SYN_SENT` (`SYN in/out 0/10`); the others connected but stalled in TLS (for example, 0 bytes received and 6 retransmissions).
- The attempt started at 00:38:01.054 only connected at **00:38:04.673, 43 ms after** the Mac left Low Power for Discovery, and completed TLS in 35 ms.

In the successful boot (Mac slave, Low Power), the equivalent outgoing connections connected in 0.1–1.6 s.

### 1.5 Driver counters

| | Successful boot | Failing boot (00:39:30) |
|---|---|---|
| Dwells in peer slots / dwells requested | 269 / 605 (44 %, at 00:27) | **862 / 984 (88 %)** |
| Dwells per channel 6 / 44 / 149 | no TX on 149 during the transfer (`unii3` stuck at 97) | 48 % / 31 % / 21 % |
| `AWDLPeerPredHit/Miss` | — | 358 / 1 |
| `AWDLTxOkFailByBand` | 2g 582/2, unii1 2760/3, unii3 97/0 | 2g 376/31, unii1 1093/12, unii3 499/10 |
| Data latency < 5 ms | 942 / 991 | 178 / 331 |
| Latency ≥ 1 s / maximum | 10 / 77.8 s (before the pump) | 24 / 25.9 s |
| `AWDLRocEndFail` / dwells | 87 / 479 (00:27) | 288 / 850 |
| `AWDLTxFwBusy` | 110 (before the pump) | 0 |

- The driver predicted the iPhone's channel well (358 hits, 1 miss), so the iPhone's timeline was measured correctly.
- `scheduleOwnListen` was simulated for the 16 possible index offsets between the two Low Power sequences:
  - offset 0 (aligned): 25 % of dwells in peer slots, channels 6/44 at 50/50;
  - offset +1 or +9: **100 %** in peer slots, channels 6/44/149 at 50/25/25.
- The observed values (88 %, 48/31/21) match offset +1 or +9. **Inference:** the iPhone's slot numbering did not match the Mac's, and the Mac's own slots (2, 8, 10) were almost never visited.
- In today's live test (a neighbouring device at −80 dBm), the distribution matches offset 15. That peer is not aligned with the master Mac either.
- TX failures are spread across all three bands. It is not a single-band problem.

---

## 2. Probable cause

1. The Mac's election metric is drawn at random each time AWDL is activated (514 in one boot, 539 in the other). The iPhone's is around 530–537. When the Mac's is higher, the Mac's IO80211 stays master.
2. The iPhone does not adopt the Mac's timeline. Why is **not proven**. Hypothesis: on a real Mac the election is done by the Broadcom firmware, which first compares the master counter (the iPhone's was ~2500, the Mac's 3–60) and only then the metric; the host IO80211 compares only the metric. This rule is recalled from OWL (`election.c`) and was not checked against the code.
3. Two timelines remain. A peer only transmits to us in slots where its channel equals the one we advertise for that slot:
   - in Low Power (slots 2, 8 and 10; slot 0 is channel 100, where the driver does not even listen for AWDL), with an index offset there is no common slot;
   - in Discovery/Idle (6–8 slots in runs of 3–4) there is overlap by chance.
4. The driver made this worse: dwells in the peer's slots occupied the radio, and our own slots, which are the ones we advertise, were left unvisited.

What this explains:

- the failed outgoing connections in Low Power and the one that connected 43 ms after entering Discovery;
- DISCOVER and ASK working in Discovery/Idle;
- total silence from the iPhone after the Mac entered Low Power;
- the 00:29 success in Low Power with the Mac as slave;
- the intermittency between boots (depends on the metric draw).

What is still needed to prove it: see `slotoff`/`err` and `peermaster` in `AWDLPeerSync` with the Mac as master. That is the goal of the next test.

---

## 3. Hypotheses evaluated

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Pump from boot ≠ pump at runtime | **Rejected** | Same code path; `fTxPump` and `fListenLeadUs` are read in `init()` before the first dwell; counters at zero; `norun 1`; ROC delay the same in both boots (64–65 % below 5 ms) |
| Mac master without the iPhone following it | **Main** | Sections 1 and 2 |
| Sequence change mid-transfer (Idle → Low Power) | Trigger, not cause | In stable Low Power with the Mac as slave the transfer completed |
| Frames stuck while the peer is "away" (25.9 s) | **Real bug, fixed**; not the direct cause | `peerChannelAt()` had no age limit: a peer no longer heard kept blocking the queue. The ≥ 1 s latencies in this boot come mostly from the SYNs of the failed outgoing connections |
| Mac SYN-ACK/ACK delayed | Rejected for this failure | ASK connection with rtt 4 ms and 0 retransmissions up to the accept |
| Drift in peer tracking | Rejected | 358 hits, 1 miss; 87 µs error |
| TX failures on one band | Rejected | Failures on 2.4 GHz, UNII-1 and UNII-3 |
| Promisc state / P2P MAC | Rejected | `AWDLPromisc Yes`, `PromiscChanges 1`, `P2PAddrChanges 0`, `FwForeignData 0`, `DataRxBad 0` |
| Accept timing in `sharingd` | Rejected | Manual accept in all three cases (1.9 s, 2.7 s, 3.6 s); same message sequence |
| Wrong peer followed (there are AWDL neighbours) | Latent, **fixed** | In this boot the followed peer was the iPhone (`AWDLPeerUpdates 387` = frames from the iPhone). But the driver only switched peer after 10 s of silence from the current one, and right now it is following the neighbour |

Problems found and **not** changed (left for after the next test):

- `pumpCanSend` treats "peer in a slot 0" as "can send". Unicast sent there only arrives if the peer's infra channel is the dwell channel. This may explain part of `TxFwFail`.
- A frame accepted by the firmware at the end of a dwell goes out in the next dwell, possibly on another channel. For PSF/MI this means stale sync parameters.
- The Low Power sequence advertises channel 100 in slot 0, but the driver does not listen for AWDL there (only with `-itlawdlhometx`).
- There is a single data queue for all destinations and the gate follows a single peer.

---

## 4. Code changes

Files: `AirportItlwm/AirportItlwmAWDLInterface.{cpp,hpp}` and `tools/awdlctl.c`. No net80211 headers were touched (`ieee80211com` is still 0x1bd8).

### 4.1 Behind a switch (off by default)

- **`-itlawdlfollow`** (boot-arg) / **`awdlctl follow on|off`** (runtime, `AWDLFollowPeerOn`).
- Acts only when: the Mac is master, the followed peer is misaligned (different slot index or boundary more than 6 ms off) in 3 consecutive frames, and the peer does **not** name the Mac as master.
- In that case it adopts the peer's timeline (`fRefUs`/`fRefSeq`), as `syncFromFrame` already does with a real master, and re-plans the dwells.
- If the peer names the Mac as master and is still misaligned, it does nothing (avoids mutual chasing) and counts it in `followsusmis`.

### 4.2 Direct fixes

1. **TX gate with an age limit** (`peerGateChannel`). A peer not heard for more than 10 s now counts as unknown, which is the original behaviour for "no peer". Before, the extrapolated timeline of a vanished peer held all frames for all destinations indefinitely (maximums of 25.9 s and 77.8 s). This is a stuck queue, so it was fixed without a switch.
2. **Choice of followed peer.** The peer with unicast data traffic in the last 10 s (in either direction) is followed starting with its next frame, and is not replaced by a neighbour while it is still heard. Without data traffic the behaviour is unchanged. This is wrong state selection (dwells and the gate were following a device unrelated to the transfer), so it also ships without a switch. **There is no evidence that it caused the 00:38 failure.**

### 4.3 New diagnostic (always on)

`AWDLPeerSync`, in `ioreg` and in `awdlctl show`:

```
role master|slave peer <mac> age <ms> peermaster <mac> slotoff <0-15> err <µs>
aligned N misaligned N followsusmis N adopts N gatestale N dataswitch N
```

- `slotoff`: peer sequence index minus the Mac's (0 = aligned).
- `err`: difference between slot boundaries.
- `peermaster`: master the peer advertises in the sync TLV.
- `AWDLTxModes` gained `, follow 0|1`.
- `awdlctl show` also shows `AWDLSyncMaster`, `AWDLChanSeq`, `AWDLPeerChanSeq` and `AWDLListenByChan`.

### 4.4 Builds

Clean build (`Intermediates.noindex` deleted), all four `BUILD SUCCEEDED`.

| Build | UUID | Executable SHA-256 |
|---|---|---|
| Release/Sequoia | `0C0630E7-3AB1-37BE-AECA-9345F403D5BC` | `6691799ac6cbb1b8af1a5a6ebf118eddcaff958f59a55fe28055f1bd42bf2991` |
| Debug/Sequoia | `EC333928-3672-39B7-8C1C-ED0F7C72828B` | `00ce653515453887c72a9619183dcdc2d6ca3e8ae13f008c319c1635558c25c4` |
| Release/Tahoe | `3279F7CB-D033-39FC-B04B-83871645C625` | `0d8218eeb8a083ce848246966433b91e7ce2b23e82bb2016e80cfd20b5b4b913` |
| Debug/Tahoe | `3681C2C7-2058-369D-8A75-610859A512FE` | `2b50375fadfa1cac6fc24c1cbbe42a394473e6c187816d4f2fff39bdb836bf80` |

`tools/check-kext.sh`:

- Release/Sequoia: layout tag ok (1 defined, 0 undefined); sizes equal across all units (`ieee80211com 0x1bd8` in 41, `AirportItlwmAWDLInterface 0x1e18` in 2); imports ok (1088, none missing).
- Debug/Sequoia: layout tag ok; imports ok (1087); DWARF check skipped because Debug does not produce a dSYM.

`tools/awdlctl/awdlctl` rebuilt and tested with `show` against the current kext (the new keys appear empty on 7F4D5A99).

**None of this has been tested on hardware.** The new kext has not been loaded.

Note: the build replaced `build/dd-port/Build/Products/Release/Sequoia/AirportItlwm.kext`, which was 7F4D5A99.

---

## 5. Next test (test 8)

### 5.1 Setup

- Kext: `build/dd-port/Build/Products/Release/Sequoia/AirportItlwm.kext` (UUID `0C0630E7`).
- Boot-args: the current ones plus **`-itlawdlfollow`**:

```
-v keepsyms=1 debug=0x100 alcid=1 amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0 -btlfxadvsets -itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow
```

### 5.2 Procedure

1. After boot, confirm the kext and the modes:
   ```sh
   kmutil showloaded | grep -i itlwm               # 0C0630E7
   tools/awdlctl/awdlctl show | head -1            # ... follow 1
   ```
2. Start the capture: `scripts/monitor-awdl.sh -n test8`.
3. **Attempt A (transition):** open AirDrop on the iPhone, send a photo and accept on the Mac as soon as the request appears.
4. **Attempt B (stable Low Power):** wait 20 s and send another.
5. Ctrl-C and open `summary.md` in the reported folder. For each attempt, check:
   - the AWDL master (MASTER or SLAVE);
   - the peer alignment (`slotoff`, `err`, `peermaster`, `adopts`).
6. **If the Mac is SLAVE**, the test does not exercise `follow` (that is the case that already worked). To force a new metric draw: close the AirDrop windows in Finder and on the iPhone, wait ~1 min until AWDL turns off, and reopen. The metric appears in `logstream.txt` as `Updating Election metric to N`. Repeat until N is higher than the iPhone's.
7. **A/B with the Mac as MASTER** (requires sudo):
   ```sh
   sudo tools/awdlctl/awdlctl follow off   # repeat attempt B: failure expected
   sudo tools/awdlctl/awdlctl follow on    # repeat: success expected
   ```

### 5.3 How to read the result

| Observation with the Mac as MASTER | Conclusion | Next step |
|---|---|---|
| `adopts` ≥ 1, `slotoff 0`, transfers A and B complete; fails with `follow off` | Hypothesis confirmed and worked around | Leave `-itlawdlfollow` on by default |
| `peermaster` = the Mac's MAC and `followsusmis` growing | The iPhone follows us but is shifted: the sync parameters we send are biased | Fix `buildAWDLFrame` using the measured `slotoff`/`err` |
| `slotoff 0` and small `err` from the start, and it still fails in Low Power | Hypothesis wrong | Check in the attempt: `TxFwFail` per band, latency, `ListenBusy`; evaluate the unchanged items in section 3 |
| `adopts` grows without stopping | `follow` is oscillating between timelines | `awdlctl follow off` and analyse `deltas.log` |

If a panic appears or infra Wi-Fi degrades, go back to the test 7 kext.

---

## 6. `monitor-awdl.sh`

- Location: `scripts/monitor-awdl.sh`. Options in the header (`-h`).
- Read-only (`ioreg`, `ifconfig`, `log stream`, `awdlctl show`). Does not use sudo.
- Writes to `./awdl-monitor-<date>-<time>[-name]/`:
  - `samples.jsonl` and `deltas.log`: driver counters every 1 s, only what changed;
  - `logstream.txt`: filtered `sharingd`, `wifip2pd`, kernel (AWDL, p2p, election, TCP on awdl0) and `bluetoothd`;
  - `ioreg-start.txt`, `ioreg-end.txt`, `awdlctl-start.txt`, `awdlctl-end.txt` and `meta.txt`;
  - `summary.md`: one section per attempt (timeline, master, sequence, counters, latency, TCP) and the list of anomalies.
- `scripts/monitor-awdl.sh -s <folder>` regenerates `summary.md`.
- `tcpdump` is skipped when `/dev/bpf0` is not readable without sudo, which is the case on the test machine.
- Tests done: 36 s with a time limit, Ctrl-C (SIGINT to the group) and SIGTERM, with no orphan processes. The report generator was validated against the real logs from both boots: it identifies the 00:38 failure ("UPLOAD never arrived", Mac MASTER, sequence change +0.83 s) and the 00:29 success (Mac SLAVE).
- Volume: about 1.5 kB/s of log and 1.1 kB/s of samples.

---

## 7. Test 8 and the delay before the Mac reappears in the share sheet (2026-11-11, 02:25)

### 7.1 Test 8 result

- Kext `0C0630E7`, boot-args with `-itlawdlfollow`. Captured logs (not published).
- iPhone → Mac: 2 out of 2 successful, with the **Mac as MASTER** (metric 536). `adopts 4`, `slotoff 0`, `peermaster` = the iPhone itself.
- This confirms section 2: the iPhone does not elect the Mac (it advertises itself as master) and the transfer goes through when the Mac adopts its timeline.
- The A/B with `follow off` in the same boot is still missing to close the causality.

### 7.2 New symptom

After a transfer, the Mac takes a long time to reappear in the iPhone's share sheet.

### 7.3 What the data shows

Timeline after the 1st transfer (`logstream.txt` and `awdl0.pcap` from the capture):

| Time | Δ since end | Event |
|---|---|---|
| 02:12:28.2 | −0.7 s | AirDrop BLE starts advertising again (181 ms) |
| 02:12:28.934 | 0 | Importing END |
| 02:12:30.000 | +1.1 s | Mac switches to Low Power; the AirDrop Bonjour server stays active |
| 02:12:36.0–36.3 | +7.1–7.4 s | iPhone "absent" and immediately "present" (its AWDL restarted: share sheet reopened, probably) |
| 02:12:36 → 02:13:05 | | **29 s without any SYN or DISCOVER from the iPhone** |
| 02:13:05.870 | +36.9 s | `Discoverable mode changed` → Bonjour server stops |
| 02:13:08.462 | +39.5 s | Bonjour server starts; mDNS announcements at 09.400 and 10.400; Discovery sequence at 09.151 |
| 02:13:10.494 | +41.6 s | SYN from the iPhone on port 8770 |
| 02:13:10.893 | +42.0 s | DISCOVER received |

After the 2nd transfer there was no further contact from the iPhone in the remaining 36 s of the capture.

Facts:

1. **awdl0 receives no multicast at all.** In `awdl0.pcap` (1760 packets, 68 s) there are 29 group packets sent by the Mac (mDNS to `ff02::fb`) and **0 received**. All incoming packets are unicast to the awdl0 MAC.
2. The net80211 `wrongdir` counter rises in parallel (34 at the end of the capture, 47 at 02:19). It counts data frames without DS bits arriving at the station: the format of AWDL group data frames.
3. In the code, `ieee80211_input()` only delivered to awdl0 frames with `addr1 == ic_awdl_addr`. Group frames (`addr1` multicast, `addr3` = AWDL BSSID `00:25:00:ff:94:73`) went down the station path and were dropped there.
4. The iPhone only contacted the Mac again 2.0 s after the Bonjour server restarted, i.e. after an **unsolicited announcement** from the Mac and the switch to Discovery. The first discovery of each session happens the same way (announcements at 02:12:21.7–23.8, DISCOVER at 22.1).
5. BLE is not the problem: advertising resumed before the transfer ended.

### 7.4 Probable cause

The Mac never hears the iPhone's mDNS queries (`PTR _airdrop._tcp.local` over multicast), so `mDNSResponder` never answers them. The iPhone only learns about the service when the Mac announces it on its own, and that only happens when `sharingd` (re)starts the Bonjour server. After a transfer the server keeps running, there is no new announcement, and the iPhone's query goes unanswered until something restarts the server.

Confidence: the RX defect is proven (pcap and code). That it is **the** cause of the delay is inference; the test in section 7.7 measures it directly.

Alternative hypotheses, in order:

| Hypothesis | Status |
|---|---|
| Outgoing multicast held by the peer gate | No evidence in this capture: 11 frames went out in the interval and none stayed pending. Now measured (`AWDLDataMcast`) |
| Low Power with few common slots | Possible contributor. `ListenBusy` was 45 out of 141 dwells in the interval (32 %): with aligned timelines, the slot 8 dwell still occupies the radio when the slot 10 request arrives |
| Followed peer switches to the neighbour after 10 s without data | Happened at the end of the capture (`peer <neighbour>`, `gatestale 90`). With `follow`, the neighbour was also aligned (`slotoff 0`), so the effect should be small |
| Old TCP connections hanging for 30 s | They close on their own (RST at 02:12:52); they do not block new ones |
| Cached state on the iPhone | Not observable from the Mac |

The tail of the latency histogram (≥ 1 s: 12, maximum 125 s) comes from AWDL starting at 02:12:06: 7 frames that had been queued since before the timeline existed went out 2 minutes late. It is unrelated to the transfers.

### 7.5 Changes

Direct fixes (no switch):

1. **AWDL multicast RX** (`itl80211/openbsd/net80211/ieee80211_input.c`). Data frames without DS bits, with a group `addr1`, `addr3` equal to the AWDL BSSID and a source other than awdl0 itself now go to `ic_awdl_data_handler`, like unicast ones. `inputAWDLData` already validated the `00:17:f2` LLC/SNAP and rebuilds the Ethernet header with the group destination. Before, these frames were always dropped, so awdl0 had no multicast reception: a clear functional bug.
2. **Old frames are not sent.** In `flushData`, a frame that has been in the queue for more than 10 s is dropped and counted (`dropstale`) instead of being sent. Sending 2-minute-old mDNS queries helps nobody, and TCP has long since retransmitted.

New diagnostic (always on), `AWDLDataMcast` in `ioreg` and in `awdlctl show`:

```
rx N queued N sent N held N heldmc N dropstale N lat <5:.. <20:.. <50:.. <100:.. <200:.. <500:.. <1000:.. >=1000:..
```

- `rx`: group frames delivered to awdl0.
- `queued` / `sent` / `lat`: outgoing multicast and its queue → firmware latency (the general histogram still includes everything).
- `held`: pump ticks with the dwell running, data queued and the peer off-channel; `heldmc`: the same with multicast at the head of the queue.

The multicast TX policy was not changed (same queue, same gate): there is no evidence that it fails. If `heldmc` or multicast latency are high in the next test, the next step is to send multicast at the start of each of our own slots, without depending on the followed peer.

No new switch. `awdlctl` was rebuilt only to show `AWDLDataMcast`.

### 7.6 Builds (replace those in section 4.4)

Clean build, all four `BUILD SUCCEEDED`. Not tested on hardware.

| Build | UUID | Executable SHA-256 |
|---|---|---|
| Release/Sequoia | `17DB71E3-F90C-34BF-9F1F-EFD61DE0010A` | `7c3a9422a41a59eeb9ec445761364f137ee4cfecf3452af6a5b239b91d19c1fc` |
| Debug/Sequoia | `8464F9CF-503F-36B9-9B32-F3A44BA7EE05` | `9787442d8a2923620b4b5f259e278ae1f7baece9ecd7506d4a3c0f5e3eb9a429` |
| Release/Tahoe | `7F8221D7-EAA4-36A6-A6A3-2DA33CD3A7DA` | `633e628137206f2e68cd4db6e49b2cfd4ff2a1c490186bfa45e0db36f5845aa8` |
| Debug/Tahoe | `42922F4B-D59A-31BA-87BB-823027F21CF0` | `5d7ffab9b4c84b3a134f87285edeaa30692354ffbe508e16294ffc7ea25ca107` |

`tools/check-kext.sh` with the kernel-collection symbol list and the baseline undefined-symbol list:

- Release/Sequoia: layout tag ok, structure sizes equal across all units, imports ok (1088, none missing).
- Debug/Sequoia: layout tag ok, imports ok (1087); DWARF check skipped (no dSYM).

`ieee80211_var.h` did not change (the net80211 change is only in the `.c`).

### 7.7 `monitor-awdl.sh`

- New section in `summary.md`, **"Reappearance after each transfer"**. For each transfer end (`Importing END` on receive, `Received UPLOAD response` on send) it shows:
  - time to the first incoming SYN on awdl0 port 8770;
  - time to the next DISCOVER and the next ASK;
  - what happened in the interval (Bonjour server, discovery mode, BLE, sequence, peer presence, election);
  - whether contact only resumed after the Bonjour server restarted;
  - multicast received/sent/held and the latency, when the kext publishes `AWDLDataMcast`.
- If `awdl0.pcap` exists, it counts group packets sent and received.
- New anomalies: contact only after a Bonjour restart, more than 15 s until contact, no multicast received in the pcap.
- Tighter log filter (the test 8 capture was 758 kB in 2 min, almost all `sharingd` progress lines).
- Validated with a copy of the test 8 capture (it detects the 42.0 s and the Bonjour restart) and with a 12 s live capture.
- Limitation: times are counted from the end of the transfer. The script does not know when the share sheet was reopened; that is why the times in the test below are fixed.

### 7.8 Next test (test 9)

- Kext: Release/Sequoia `17DB71E3`. Boot-args the same as test 8 (with `-itlawdlfollow`).
- On the Mac: `sudo scripts/monitor-awdl.sh -n test9` (with sudo so that `awdl0.pcap` is captured; without sudo it also works, only the pcap is lost). Keep the Finder AirDrop window closed and do not change AirDrop settings during the test.
- On the iPhone, with a stopwatch:
  1. t = 0: open a photo → Share → AirDrop. Note how many seconds the Mac takes to appear. Send and accept on the Mac.
  2. When the iPhone shows "Sent", tap "OK" and **close** the share sheet.
  3. Wait **10 s** and open Share → AirDrop again. Note the seconds until the Mac appears (60 s maximum). Do not send.
  4. Close the sheet, wait **30 s** and repeat step 3.
  5. Send a second photo, and repeat steps 2 and 3 once.
- Ctrl-C the monitor.

How to read:

| Observation | Conclusion |
|---|---|
| `AWDLDataMcast rx` > 0, pcap with received multicast, and the Mac reappears within a few seconds without a Bonjour restart | Fixed |
| `rx` > 0 but the Mac still takes long | Reception is fine; check `heldmc` and multicast latency (Mac's response held) and `ListenBusy` in Low Power |
| `rx` = 0 and `wrongdir` keeps rising | The firmware delivers the frames but the new filter does not catch them (different BSSID): check `AWDLRadioBssid` |
| `rx` = 0 and `wrongdir` stopped | The iPhone did not send multicast while the Mac was on the channel: look at slot coverage in Low Power |
