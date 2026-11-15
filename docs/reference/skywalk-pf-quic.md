# User-space networking: pf → netagent → channel-flow → QUIC

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

Everything in this note was found and checked on 26.7.2 (25G332).

On a real Mac, Network.framework connections run in user space on Skywalk (`channel-flow`). If the flowswitch transport agent is off, everything falls back to kernel sockets (`socket-flow`). That works, but QUIC degrades. On the test hackintosh it was off because of an OpenCore quirk. It has nothing to do with the Wi-Fi driver, and it affects any network card.

## Symptoms

- `sharingd` logs `quic_time_delta start time X is after end time Y` hundreds or thousands of times per transfer (~2170 in a 12 MB AirDrop transfer).
- `netstat -qq -I awdl0`: `L4S pkts` rising and `paced pkts: 0`.
- Every connection shows up as `socket-flow`; none as `channel-flow`.

## Ten-second check

```sh
sysctl net.filter_state net.link.generic.system.enable_netagent
ifconfig -v en0 | grep FlowSwitch
```

| | Healthy (real Mac) | Broken |
|---|---|---|
| `net.filter_state` | empty | `pf` |
| `enable_netagent` | 1 | 0 |
| FlowSwitch agent | `flags:0x4483` | `flags:0x4403` (missing `NEXUS_PROVIDER`, 0x80) |
| interface `eflags` | with `ECN_ENABLE,CHANNEL_DRV` | without |

## The chain

1. **pf.** When rules are loaded, the kernel checks whether they are "compatible" (`pf_process_compatibilities` / `pf_check_compatible_rules`). Anchors with known names are accepted: `_pf`, `com.apple`, `com.apple.internet-sharing`, `com.apple/250.ApplicationFirewall`, `com.apple/200.AirDrop`. Any other anchor or rule marks pf as incompatible.
2. **`net.filter_state`.** The incompatible state shows up as `pf` in this sysctl.
3. **netagent.** `dlil_filter_event` turns off the flowswitch transport agent when `net_filter_event_state` has bits other than `PF_PRIVATE_PROXY` → `enable_netagent = 0`, agent `0x4403`.
4. **Network.framework.** Without the agent, no connection gets a nexus flow → `socket-flow`.
5. **QUIC.** With ECN in the L4S/Prague state, each packet's send time comes from the pacer (`quic_pacer_get_packet_tx_time`, used in `quic_conn_handle_sent_packet`). Over sockets the pacer has no effect, the predicted time runs ahead of the real one, and `quic_rtt_process_new_sample` → `quic_time_delta` logs the error. The gap grew from 0 to 8.3 s over one transfer.

The stock `/etc/pf.conf` only has `scrub-anchor "com.apple/*"` and `anchor "com.apple/*"`. That is compatible: on a real Mac with the same file (same MD5) the state stays empty.

## The cause in this case

OpenCore's `Kernel > Quirks > PowerTimeoutKernelPanic` replaces the literal `"com.apple\0"` with `"not.apple\0"` in memory. On kernel 25.6 that literal is shared with pf's anchor check, so `com.apple` was no longer recognized and the stock `pf.conf` counted as custom. Details and fix in [opencore-patches.md](opencore-patches.md).

Other things that give the same symptom, legitimately: your own pf rules, third-party firewalls that load anchors, Internet Sharing with rules outside the known names.

## How it was found (a reusable method)

1. **Narrow it down to the difference with a real Mac.** Same commands on both: sysctls, `ifconfig -v`, MD5 of `/etc/pf.conf` and `/etc/pf.anchors/com.apple`. Same files, different state → the difference is in the in-memory kernel.
2. **Isolate with reversible experiments.** `launchctl disable system/com.apple.pfctl` + reboot → healthy state. Loading a single `anchor "com.apple"` → back to `pf`. `pfctl -F all` → cleared. (Anchors loaded with `-a` stay empty until reboot.)
3. **Watch the comparison happen.** DTrace, with SIP allowing it (`csr-active-config` with the DTrace bit, temporarily):
   ```
   fbt::strcmp:entry /execname == "pfctl"/ { printf("%s,%s", stringof(arg0), stringof(arg1)); }
   ```
   Output: `strcmp(com.apple,_pf)` and then `strcmp(com.apple,not.apple)`. The second string does not exist in the kernel on disk.
4. **Find who writes it.** `strings` on `OpenCore.efi` finds `not.apple`; the code is in `OcAppleKernelLib/CommonPatches.c`.

## Measuring after the fix

`tools/airdrop/airdrop-measure.sh [window]` summarizes from the log: system state, `sharingd` connections by type and interface, the number of `quic_time_delta` errors, totals for each QUIC connection, and the duration of HTTP/3 transactions.

Values after the fix: 784 `channel-flow en0` and 0 `socket-flow` in 6 minutes; 0 `quic_time_delta` errors; `networkQuality` ~194 to 207 / ~106 Mbps; on `awdl0`, `paced pkts` and `pacemaker needed` rising.

`tools/airdrop/awdlquic.swift` is a QUIC client/server pair (Network.framework, `includePeerToPeer`, Bonjour `_hxquic._udp`) for testing `awdl0` between two Macs without depending on AirDrop.

## Notes

- The macOS application firewall holds incoming connections to unsigned binaries: a test server on a Mac with the firewall on receives nothing until the user decides. Run the server on the side without the firewall.
- `quic_time_delta` errors don't imply loss: the connection with 2173 errors had 9018 packets sent, 9011 acknowledged and 0 retransmitted.
- Clean QUIC doesn't imply high throughput: the limit on `awdl0` comes from the radio and the driver (see [awdl-protocol-notes.md](awdl-protocol-notes.md) §3 and §5).
