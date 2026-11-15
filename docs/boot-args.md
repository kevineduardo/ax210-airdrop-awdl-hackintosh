# Boot arguments

All AWDL code is compiled into the Sequoia and Tahoe builds of `AirportItlwm.kext`, but **nothing runs unless you add `-itlawdl`**. Without it the kext behaves like the upstream AirportItlwm: normal Wi-Fi only.

The AWDL work was brought up in stages, and each stage still has its own switch. That makes it possible to bisect a problem on new hardware or a new macOS build without rebuilding the kext.

## Recommended set (what we test with)

```
-itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -btlfxadvsets
```

While testing, also add `keepsyms=1 debug=0x100` so a kernel panic leaves a readable backtrace.

`-btlfxadvsets` belongs to BlueToolFixup, not to AirportItlwm. It turns on BLE advertising sets, which AirDrop needs so that iPhones can discover the Mac. See [BlueToolFixup](#bluetoolfixup) below.

## AirportItlwm: AWDL switches

| Boot argument | Default | What it does |
|---|---|---|
| `-itlawdl` | off | Master switch. Announces the AWDL capability to macOS and lets IO80211Family create `awdl0`. Every other switch below needs it. |
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

## AirportItlwm: other switches (inherited from upstream)

| Boot argument | What it does |
|---|---|
| `-novht` | Disables VHT (802.11ac). |
| `-noht40` | Disables 40 MHz channels. |
| `itlwm_cc=XX` | Overrides the country code. |
| `itlwm_he=1` | Enables 802.11ax (HE) where upstream supports it. |

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
tools/awdlctl/awdlctl show                # counters, no sudo needed
```

The driver also accepts the IORegistry properties `AWDLListenDwellTU` (16 to 1000, default 160), `AWDLListenEveryNth` (1 to 64, default 1) and `AWDLListenAlign` (0 or 1). `awdlctl` does not expose them yet.

## BlueToolFixup

| Boot argument | What it does |
|---|---|
| `-btlfxadvsets` | Patches `bluetoothd` so it uses BLE advertising sets with a third-party controller. On macOS 15.8.1 (24H32) the patch table is only applied when the `bluetoothd` UUID matches exactly; on other builds nothing is patched and the log says why. On Tahoe 26.4.1 it uses crazyhulk's per-page patterns. |
| `-btlfxadvsetsany` | Applies the patterns without the UUID check. Only for testing a new macOS build; a partial match is logged. |
| `-btlfxa2dpcheck` | Upstream crazyhulk switch for the A2DP fast-connect gate. See [BLE advsets changes](research/ble-advsets-sequoia-changes.md) for a false positive on 15.8.1. |
| `-btlfxdbg` | More logging (use with the Debug build). |

Check the result with:

```sh
log show --last boot --predicate 'eventMessage CONTAINS "[advsets]"'
```
