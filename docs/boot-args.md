# Boot arguments

All AWDL code is compiled into the Sequoia and Tahoe builds of `AirportItlwm.kext`, but **nothing runs unless you add `-itlawdl`**. Without it the kext behaves like the upstream AirportItlwm: normal Wi-Fi only.

The AWDL work was brought up in stages, and each stage still has its own switch. That makes it possible to bisect a problem on new hardware or a new macOS build without rebuilding the kext.

## Recommended sets (what we test with)

**macOS 15 Sequoia** (tested on 15.8.1):

```
-itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -btlfxadvsets
```

**macOS 26 Tahoe** (tested on 26.7.2): the same, plus `-itlawdlhometx`. The test machine also boots with `itlwm_he=1 -itlhecap -itlfwstats` (802.11ax and the experimental HE capability / firmware-statistics reporting) and `-ibtcompatbeta` (IntelBluetoothFirmware on Tahoe). Tahoe also needs two OpenCore settings for AirDrop over QUIC; see [installation.md](installation.md).

```
-itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -itlawdlhometx -btlfxadvsets -ibtcompatbeta
```

While testing, also add `keepsyms=1 debug=0x100` so a kernel panic leaves a readable backtrace.

`-btlfxadvsets` belongs to BlueToolFixup, not to AirportItlwm. It turns on BLE advertising sets, which AirDrop needs so that iPhones can discover the Mac. See [BlueToolFixup](#bluetoolfixup) below.

## AirportItlwm: AWDL switches

| Boot argument | Default | What it does |
|---|---|---|
| `-itlawdl` | off | Master switch. Announces the AWDL capability to macOS and lets IO80211Family create `awdl0`. Every other switch below needs it. Only takes effect on a macOS build that passes the AWDL build guard (see `-itlawdlanybuild`). |
| `-itlawdlanybuild` | off | Lets AWDL start on a kernel that is not on the verified list. Without it, on an unverified kernel the kext behaves as if `-itlawdl` was not set: normal Wi-Fi only. The vtable check still runs and still blocks AWDL if Apple's `IO80211AWDLProtocol` table changed. For testers of a new macOS build who accept the risk of a kernel panic. The guard's decision is in `ioreg -l -w0 \| grep AWDLBuildGuard`. |
| `-itlawdlaw` | off | Stage 2c: runs the AWDL availability-window (AW) timeline. Without it `awdl0` only receives action frames. |
| `-itlawdllisten` | off | Stage 2b: leaves the station channel for short listen dwells on the AWDL channel when the station is elsewhere. Also enables the run-time properties used by `awdlctl`. |
| `-itlawdlp2p` | off | Listens through a dedicated firmware P2P-device MAC (second MAC + PHY context + session protection, like Linux iwlmvm). This is the listen method that works reliably on the AX210. |
| `-itlawdlpromisc` | off | Sets `IN_PROMISC` on the P2P-device MAC while an AWDL peer is active, so the firmware delivers and ACKs data frames addressed to `awdl0`. Needed for data. |
| `-itlawdltx` | off | Stage 3: transmits AWDL action frames (PSF and MI) during the listen dwells. |
| `-itlawdldata` | off | Stage 4: AWDL data frames to and from the `awdl0` BSD interface (what AirDrop's TCP/TLS connection uses). |
| `-itlawdltxpump` | off | Instead of sending at fixed offsets (+1 ms, +4 ms) inside each dwell, polls the firmware every `itlawdlpumpus` microseconds and sends as soon as the dwell really starts. Fixes the unreliable TX that made AirDrop uploads fail. See [AWDL TX timing](research/awdl-tx-timing.md). |
| `itlawdlpumpus=<µs>` | 1000 | Poll interval of the TX pump. Accepted range 250 to 10000. |
| `itlawdllead=<µs>` | 6000 | How early the listen dwell is requested before the slot. Accepted range 1000 to 60000. We use 30000. |
| `itlawdldwellx=<TU>` | 0 | Extends every listen dwell by this many time units (1 TU = 1024 µs). Range 0 to 200. |
| `-itlawdlfollow` | off | When this Mac wins the AWDL master election but the peer keeps its own timeline, adopt the peer's timeline. Fixes AirDrop failures when the Mac is master. See [master election](research/airdrop-master-election-and-multicast.md). |
| `-itlawdlhometx` | off | Experimental: also uses slots on the station (home) channel for AWDL dwells. |
| `-itlawdlnopeerlisten` | off | Turns off listening on the tracked peer's channel in slots where our own channel sequence stays home. Diagnostic. |
| `itlawdllistench=<n>` | none | Diagnostic: sends every listen dwell to channel `n` instead of the AW slot channel. |
| `-itlawdlnoinject` | off | Safety switch for bring-up: received AWDL action frames are parsed and tracked but never handed to IO80211Family. Use it first on a new macOS build, so an ABI mismatch on the receive path cannot panic the machine. |
| `-itlawdlsetif` | off | Sequoia only: also registers `awdl0` with `IO80211Controller::setAwdlInterface()`. Not needed on 15.8.1. Kept for experiments. |

### Switches that are on by default (use them to go back to older behaviour)

| Boot argument | What it turns off |
|---|---|
| `-itlawdlnoawgate` | The AW gate: data waits for the peer's availability window instead of leaving as soon as the dwell runs. With this switch, frames leave as soon as the dwell runs (old behaviour). Run time: `awdlctl awgate on/off`. |
| `-itlawdlnostats` | Reporting our AWDL data traffic to the IO80211Family peer manager, which it uses to choose the AWDL schedule (Idle, Low Power, Data states). With this switch the family does not see our traffic. Run time: `awdlctl datastats on/off`. |
| `-itlawdlnopeercache` | Asking the family to cache the unicast data peer (`cachePeer`), which the family needs before it lets data flow. |
| `-itlawdlnoifenable` | Marking `awdl0` as enabled for the family (`setInterfaceEnable`), one of the conditions of `cachePeer`. |
| `-itlawdlnoup` | Bringing `awdl0` up automatically 1 s after it is attached. |
| `-itlawdlpush` | The pull output model: by default the stack queues and paces `awdl0` packets and the driver takes them when it has room, which Apple's QUIC (AirDrop) needs for correct timing. `-itlawdlpush` sends packets as soon as the stack hands them over (old model; QUIC uploads stall). |
| `-itlawdlfixedrate` | The adaptive rate ladder for unicast AWDL data (from the cached peer's HT/VHT capabilities). With it, data stays at 6 Mbps. |
| `-itlawdllegacyrate` | HT/VHT rates for AWDL data: the ladder stops at 54 Mbps OFDM. |

### Other AWDL tunables

| Boot argument | Default | What it does |
|---|---|---|
| `-itlawdlidledwell` | off | Shorter listen dwells while AWDL is idle. Halves the gateway latency, but with the firmware's late dwell starts it misses most of what an idle peer sends and discovery can take minutes. Run time: `awdlctl idledwell on/off`. |
| `itlawdltrustrssi=<n>` | 60 | Peers heard at or above −`n` dBm (range 30 to 95) are trusted for TX gating ("bubble"); weaker peers are not. |

## AirportItlwm: AWDL coex backend (experimental, opt-in)

A second way to run AWDL, ported from [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm)'s "coex listener" engine. Instead of short P2P listen dwells, the firmware keeps a listener context on its own channel (149 at 80 MHz, else 6 at 20 MHz) next to the Wi-Fi connection the whole time. How it works: [how-awdl-works.md](how-awdl-works.md#the-coex-backend).

It is off unless you add `-itlawdlcoex`. With it, the P2P listen path is switched off for the whole boot: `-itlawdlp2p`, `-itlawdllisten`, `-itlawdltx`, `-itlawdltxpump`, `-itlawdlpromisc` and the other dwell switches above then do nothing, and no `SESSION_PROTECTION_CMD` or `HOT_SPOT_CMD` is sent. The two backends use the same firmware MAC, PHY context and station numbers, so they never run together.

The port lands in phases. What each phase does is listed in [test-results-tahoe.md](test-results-tahoe.md#coex-backend-to-test-on-hardware).

| Boot argument | Default | What it does |
|---|---|---|
| `-itlawdlcoex` | off | Selects the coex backend. Needs `-itlawdl` and a build that passes the AWDL build guard, and the AW timeline (`-itlawdlaw`), which stores the frame templates macOS hands over. While en0 is associated, a listener context runs on its own channel (149 at 80 MHz when the firmware allows all of 149 to 161, else 6 at 20 MHz, else en0's channel), answers the firmware's country notice, hands every AWDL action and data frame it hears to `awdl0`, sends macOS's sync frames (MI and PSF, every 45 ms idle, 100 ms while data moves) and sends `awdl0`'s data one frame at a time, with a rate ladder up to VHT 80 MHz two streams and a send gate that follows the iPhone's schedule. Block Ack is off unless you add the switches below. The choice is shown as `AWDLBackend` (`coex`, `p2p`, `scan` or `none`). |
| `-itlawdlcoexshared` | off | With `-itlawdlcoex`: never give the listener its own channel; it shares en0's channel and binding (ForbiddenItlwm without its multi-channel switch). For A/B tests only. Refused when en0 is on a DFS channel, because the listener could never send there. |
| `-itlawdlsameband` | off | With `-itlawdlcoex`: while en0 is on 2.4 GHz, keep the listener on channel 6 instead of 149, so the radio does not switch bands (ForbiddenItlwm's switch of the same name). |
| `-itlawdlcoexnogate` | off | With `-itlawdlcoex`: turns the send gate off, so data for the iPhone also leaves while its schedule says it is on another channel or dozing. For A/B tests. |
| `-itlawdlcoexcalwait` | off | With `-itlawdlcoex`: the iPhone's schedule is used only after it has been calibrated from about 300 of its frames, instead of assuming offset 0 from its first sync frame. For A/B tests. |
| `itlawdlcoexpull=<n>` | 192 | With `-itlawdlcoex`: how many `awdl0` packets the driver takes from the network stack before it stops pulling (32 to 240). The P2P path keeps 32. |
| `-itlawdlcoexba` | off | With `-itlawdlcoex`: answers the iPhone's Block Ack request (ADDBA), so the iPhone can send to the Mac in A-MPDUs. The iPhone gets a firmware station of its own (station 3) and a firmware receive session; the answer goes out from the listener's queue. Idle sessions (60 s without data on their TID) are ended with our own DELBA; the station is removed after 60 s without hearing the iPhone or 10 minutes without data. While a session is open the listener does not receive promiscuously (the firmware does not acknowledge the session's frames otherwise), so group frames such as mDNS are not received meanwhile. Off by default until it has been tested on hardware. |
| `-itlawdlcoexbatx` | off | With `-itlawdlcoex`: Mac to iPhone in A-MPDUs at VHT 80 MHz two streams. Turns `-itlawdlcoexba` on too. Once the iPhone has acknowledged our receive answer (or a bulk stream of payload frames goes to it), its station gets a firmware aggregation queue on TID 0 with the firmware's rate control (VHT MCS 4 to 9, one or two streams, 80 MHz when the listener is at 80 MHz; HT MCS 4 to 7 at 20 MHz otherwise) and the driver sends an ADDBA request (at most every 200 ms, not while the iPhone's schedule says it is away, 40 per round then 20 s of rest). After the iPhone agrees, its payload frames go out as QoS data on that queue and the firmware aggregates them; short exchanges stay on the listener's single-frame queue. A DELBA from the iPhone sends everything back to the single-frame queue and asking starts again. |
| `-itlawdlcoexpeerpause` | off | With `-itlawdlcoexbatx`: while aggregate frames move and the iPhone's schedule says it is on another channel, the firmware stops sending to the iPhone's station (`ADD_STA` with `DISABLE_TX`) and starts again when it is back, at most 450 ms later. A failed resume is retried every 50 ms; after 5 failures the iPhone's station is removed. |
| `-itlawdlcoexnoorder` | off | With `-itlawdlcoexbatx`: turns the ordering rule off. By default every unicast frame for the iPhone waits behind its paused, full or still draining aggregation queue, so a short QUIC packet never overtakes held payload on the single-frame queue. For A/B tests. |
| `-itlawdlcoexsamechprobe` | off | With `-itlawdlcoexbatx`: when the listener is on en0's own channel, the 250 ms probe window (promiscuous receive opened once per ADDBA dialog while our request goes unanswered and a receive session is open) is allowed there too. Normally the listener is never promiscuous on en0's channel. For A/B tests. |
| `itlawdlcoexchan=<n>` | 0 | With `-itlawdlcoex`: the listener's channel preference. `0` keeps the default order: 149 at 80 MHz when the firmware allows all of 149 to 161, else 6 at 20 MHz. `149`: the same, but 44 at 80 MHz (all of 36 to 48 allowed) is tried before 6. `44`: 44 at 80 MHz first, then 149, then 6. `6`: 6 first, then 149, then 44. Any other value counts as 0. Meant for places where 149 to 161 stay passive or are not allowed at 80 MHz (the listener then falls back to 6 at 20 MHz, which is slow); the iPhone puts its own slots on 44 there. A channel is only used when the firmware's map allows starting a transmission on it (an AP heard beaconing on it also counts) and it is not a radar channel. |
| `-itlawdlawsync` | off | With `-itlawdlcoex`: while no data moves, a sync frame also goes out at the start of each of the iPhone's AWs on the listener's channel (within 2 ms of it, at most one per 8 ms), besides the usual one every 45 ms. An idle iPhone is reliably awake only then, so it finds the Mac sooner (ForbiddenItlwm's switch of the same name). Needs the iPhone's schedule (`ITLAWDLCoexScheduleKnown`). |

The counters are on the controller: `ioreg -lw0 -r -c AirportItlwm -d1 | grep -E 'AWDLBackend|ITLAWDLCoex'`.

## AirportItlwm: other switches (inherited from upstream)

| Boot argument | What it does |
|---|---|
| `-novht` | Disables VHT (802.11ac). |
| `-noht40` | Disables 40 MHz channels. |
| `itlwm_cc=XX` | Overrides the country code. |
| `itlwm_he=1` | Enables 802.11ax (HE) where upstream supports it. |
| `-itlhecap` | Reports the HE (802.11ax) capability to macOS (`getHE_CAPABILITY`). Experimental: only reported when this switch is set. |
| `-itlfwstats` | Every 5 s, reads firmware statistics and reports channel load to macOS, like a native driver. Experimental. |

## Run-time switches (`awdlctl`)

When `-itlawdllisten` is set, an administrator can change some of the switches above without rebooting, which is how A/B comparisons were made:

```sh
make -C tools/awdlctl
sudo tools/awdlctl/awdlctl pump on        # same as -itlawdltxpump
sudo tools/awdlctl/awdlctl lead 30000     # same as itlawdllead=30000
sudo tools/awdlctl/awdlctl follow on      # same as -itlawdlfollow
sudo tools/awdlctl/awdlctl hometx off
sudo tools/awdlctl/awdlctl dwellx 0
sudo tools/awdlctl/awdlctl pumpus 1000
sudo tools/awdlctl/awdlctl awgate on       # off = same as -itlawdlnoawgate
sudo tools/awdlctl/awdlctl datastats on    # off = same as -itlawdlnostats
sudo tools/awdlctl/awdlctl idledwell off   # on = same as -itlawdlidledwell
sudo tools/awdlctl/awdlctl dwell 420       # longest single listen dwell, TU (16 to 1000)
tools/awdlctl/awdlctl show                # counters, no sudo needed
```

The driver also accepts the IORegistry properties `AWDLListenEveryNth` (1 to 64, default 1) and `AWDLListenAlign` (0 or 1). `awdlctl` does not expose them yet. The default longest dwell (`AWDLListenDwellTU`) is 420 TU: one dwell then covers the six-slot run of the AWDL Data sequence.

## BlueToolFixup

| Boot argument | What it does |
|---|---|
| `-btlfxadvsets` | Patches `bluetoothd` so it uses BLE advertising sets with a third-party controller. On macOS 15.8.1 (24H32) and 26.7.2 (25G332) the patch table is only applied when the `bluetoothd` UUID matches exactly, and then all six patches or none; on other builds nothing is patched and the log says why. On 26.4.1 it uses crazyhulk's per-page patterns. If `bluetoothd` started before Lilu was ready, BlueToolFixup restarts it once so the patches can apply. |
| `-btlfxnolatefix` | Turns off that restart of an already running `bluetoothd`. |
| `-btlfxadvsetsany` | Applies the patterns without the UUID check. Only for testing a new macOS build; a partial match is logged. |
| `-btlfxa2dpcheck` | Upstream crazyhulk switch for the A2DP fast-connect gate. See [BLE advsets changes](research/ble-advsets-sequoia-changes.md) for a false positive on 15.8.1. |
| `-btlfxdbg` | More logging (use with the Debug build). |

Check the result with:

```sh
log show --last boot --predicate 'eventMessage CONTAINS "[advsets]"'
```
