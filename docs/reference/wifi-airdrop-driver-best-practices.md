# Best practices: a third-party Wi-Fi driver with AirDrop on macOS Tahoe

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

A working guide for anyone who develops or maintains a non-Apple Wi-Fi driver (here, the Intel AX210 with AirportItlwm) that has to support AWDL and AirDrop on macOS 26. It distills what this project learned; details and measurements are in the other notes in this folder and in [test-results.md](../test-results.md).

Applies to macOS 26.7.2 (25G332), Darwin 25.6, IO80211Family 1200.13.1. After any system update, redo section 1 from scratch. General kext advice that is not specific to AirDrop is in [macos-driver-development.md](../macos-driver-development.md).

## 1. Before writing code: the ABI of the build you run

- Check the vtables slot by slot against the installed kernel collection, not against headers from another version. `tools/kc/vtcheck.py` compares each kext class with its base class in `BootKernelExtensions.kc`. A misplaced slot causes a panic or silently calls the wrong method.
- Every import has to resolve in the KC (`tools/check-kext.sh`). A missing symbol makes OpenCore refuse the kext, and the machine boots without Wi-Fi.
- The layout of shared structures must be the same in every compile unit. Incremental builds with a different `#define` caused two panics. Always build clean and check (`check-kext.sh` does both checks).
- Prefer calls through exported symbols to hand-written vtable offsets. Where there is no alternative, record the offset, the build it was read from, and how to read it again.
- Make only the ABI version-dependent. The logic (gates, RX/TX fixes) should be shared by every supported version so it doesn't diverge.

Details: [io80211-driver-contract.md](io80211-driver-contract.md) §1, [porting-awdl-to-new-macos-versions.md](../porting-awdl-to-new-macos-versions.md).

## 2. The driver executes and reports; IO80211Family decides

Master election, synchronization, the peer list, the channel sequence and the AWDL state machine belong to the family. The driver switches channels on time, transmits, delivers what it receives and reports counters.

- List what Apple's driver calls in the family before implementing a subsystem (`tools/kc/kccalls.py` on `AppleBCMWLAN*`). Most bugs in this port came from information the driver never provided; few were execution errors.
- Feed the AWDL data counters (`updateInterfaceDataStats`). Without them the scheduler stays in `Low Power` (3 slots out of 16) and throughput does not get past ~400 kB/s.
- Feed the infrastructure traffic counters (`recordOutputPacket` / `recordInputPacket`). The `Infra Priority` state depends on them.
- Don't override system decisions. Example: the 1484 MTU on `awdl0` is set by macOS itself; the driver does not touch the MTU.
- `awdl0` must be a `HiddenInterface` (it is not a network service) and comes UP without help from user space.

Details: [io80211-driver-contract.md](io80211-driver-contract.md) §2 to §6.

## 3. AWDL: timing and peers

- An idle peer only listens during the Availability Window (16 TU at the start of each 64 TU slot). A frame sent outside it is lost. If the peer has had no traffic for more than ~300 ms, wait for its next AW.
- Keep state per peer, never global. Each peer has its own clock (slot, phase) and its own channel sequence, which changes with its state (resting, discovery, data, real-time infra). The decision to transmit has to be made per destination of each frame. A driver that tracks a single peer works with one device and fails when two are nearby (see the case study in section 7).
- The TX queue must not block behind an absent destination. Frames for a peer that is off-channel wait; frames for other peers go through. Order only has to be kept within each destination.
- The sync master always sends the PSF. The family's `aftx_mode 3` (Data and Infra Priority states) may only silence the PSF of a slave. A silent master is dropped by its peers within seconds, and because the election metric changes on every boot the bug looks random (case study, section 7).
- Use a real Mac as a probe. `dns-sd -includeAWDL -B/-L/-Q` on a Mac nearby gives the Add/Rmv timeline of our records; cross-checked with the local kernel log, it shows what peers really receive.
- Group frames (mDNS, ND) go out inside the AW, as in Apple's driver.
- Don't rely on fixed instants for TX. The dwell start in the Intel firmware is 10 to 40 ms late in ~20 % of cases. React to the actual moment the radio is on channel.
- A peer not heard from for ~10 s is no longer evidence: its extrapolated timeline is no longer good enough to hold frames for.
- The peer table has a fixed size with oldest-entry replacement; anywhere with many Apple devices there are always more peers than the ones that matter.

Details: [awdl-protocol-notes.md](awdl-protocol-notes.md) §2, §3, §5; [awdl-tx-timing.md](../research/awdl-tx-timing.md).

## 4. Receive path

- Compute the 802.11 header length with the net80211 function (`ieee80211_get_hdrlen`): there is QoS, HT Control and addr4.
- A-MSDU subframes with Apple's LLC. Intel hardware only strips the subframe header for RFC 1042 SNAP; AWDL uses OUI 00:17:f2, so 8 extra bytes arrive before the real LLC. Without handling this, ~19 % of data frames were rejected and transfers stalled for tens of seconds.
- `awdl0` has to receive multicast (mDNS queries from peers): without it the Mac is only rediscovered when Bonjour re-announces.
- Null / QoS-Null are not errors: count them separately.
- Keep the last rejected frame (length and first bytes) in a counter visible in `ioreg`. That is what revealed the A-MSDU issue in minutes, after a wrong hypothesis.

Details: [awdl-protocol-notes.md](awdl-protocol-notes.md) §4.

## 5. What AirDrop needs around the driver

AirDrop on Tahoe is a chain; the driver is one link.

1. BLE discovery. With a third-party controller, `bluetoothd` needs patches (BlueToolFixup `-btlfxadvsets`). Rules: table chosen by the binary's UUID, all patches or none, identify the binary by its vnode and not its path, and check at boot that all of them were applied. Half-patched BLE is worse than none. See [bluetooth-ble-advsets.md](bluetooth-ble-advsets.md).
2. mDNS over `awdl0` (multicast in both directions, inside the AW).
3. QUIC/HTTP3 (`AirDropNW`), not the HTTP/TCP used on Sequoia. It tolerates pauses and loss less well than TCP.
4. User-space networking (Skywalk). Connections have to be `channel-flow`. `pf` rules with unknown anchors turn off the transport agent and QUIC degrades. Check: `sysctl net.filter_state` empty and `enable_netagent = 1`. See [skywalk-pf-quic.md](skywalk-pf-quic.md).
5. With both devices on the same network, the transfer body may go over infrastructure Wi-Fi; `awdl0` then only carries discovery and the request. Check which interface was used before talking about "AWDL throughput".
6. Without an Apple Account there is a verification code per device and the Mac refuses to migrate the connection to infra. That is system behaviour, not the driver.

## 6. Diagnosis: observe before changing

- `ioreg` counters for every driver decision (sent, held, why, per band, queue latency, last error). They are cheap and save reboots.
- Run-time switches (`awdlctl`) and boot-args for every new behaviour, off by default until measured. They allow A/B tests within the same boot.
- Compare with a real Mac in the same situation. The kernel log (`com.apple.p2p.awdl0`) shows the states and values the family sees; the first value that differs is the lead. See [real-mac-reference.md](../research/real-mac-reference.md).
- Turn on unredacted logging during diagnosis (`Enable-Private-Data` in `/Library/Preferences/Logging/com.apple.system.logging.plist`) and turn it off afterwards.
- Enlarge the kernel buffer (`msgbuf=`) when boot messages matter; with `-v` and other chatty drivers they get lost.
- Check the baseline before concluding:
  - is the peer unlocked? (iOS silently drops what arrives for a closed listener);
  - is `sharingd` advertising and detecting over BLE?
  - is the AirDrop window or share sheet open? (with no active service, AWDL does not run);
  - how many AWDL peers are nearby, and which one is the destination of the failing frame?
- Treat one sample as a hypothesis. Repeat before writing "cause". In this project, three conclusions drawn from a single measurement were wrong (MTU, HT Control, CE marks).
- Test with more than one device nearby. A single peer hides every global-state bug.

Details and recipes (DTrace, log, pcap): [methods-and-tools.md](methods-and-tools.md). User-facing symptoms: [troubleshooting.md](../troubleshooting.md).

## 7. Install and test without losing the machine

- Clean build → `check-kext.sh` → `vtcheck.py` → only then install.
- Always keep a bootable EFI with the last good build. Install the new build on a separate EFI (for example a USB stick) and keep the other as recovery. Bring them in line only after measuring.
- Back up `config.plist` and run `plutil -lint` after every edit; never `plutil -insert` without an index.
- One reboot per coherent set of changes. Read the whole contract of a subsystem first, instead of fixing one function at a time.
- After booting, confirm the UUID of the loaded kext before measuring.
- Don't mount the sealed snapshot of the running system (system binaries keep failing until the next reboot).
- Log every measurement with time and build.

### Case study, 2026-11-14: "the iPhone and the Mac can't see the hackintosh"

- **Symptom:** intermittent from boot to boot; the hackintosh sees the others, the others don't see it.
- **First hypothesis (wrong as the main cause):** global peer state in the driver. It was a real bug (fixed: peer table, per-destination gate), but fixing it did not fix the symptom. Lesson: the hypothesis came from a `ping6` to an iPhone, which proves nothing.
- **What cracked it:** a real Mac as a probe (`dns-sd -includeAWDL`) and correlating its timeline with `set aftx_mode` and `programChipWithNewMaster` in our log. The service appeared 127 ms after the mode changed from 3 to 0, and never while the hackintosh was master in mode 3.
- **Cause:** PSF suppressed in `aftx_mode 3` even when we are master.
- **Fix:** always send the PSF while master. It had not been validated yet when this note was written; see [test-results.md](../test-results.md) for the current status.
