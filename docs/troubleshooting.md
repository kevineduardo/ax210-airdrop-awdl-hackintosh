# Troubleshooting

## First: collect data

```sh
make -C tools/awdlctl                           # once
scripts/collect-diagnostics.sh                  # one-shot snapshot → diagnostics-<date>.txt
sudo scripts/monitor-awdl.sh -n try1            # while you try AirDrop; Ctrl-C to stop → awdl-monitor-<date>-try1/summary.md
```

`monitor-awdl.sh` writes a timeline per AirDrop attempt (DISCOVER, ASK, accept, UPLOAD, import), the counter changes during each one, and who was AWDL master. With `sudo` it also records a pcap of `awdl0`.

Both outputs contain MAC addresses, network names and device names. Review them before attaching them to a public issue.

## Symptoms

### The Mac does not appear in the iPhone's AirDrop list

1. **BLE advertising.** `log show --last boot --predicate 'eventMessage CONTAINS "[advsets]"'` must show `all 6 patches applied` (15.8.1) or the per-page patch lines (Tahoe). If it says the UUID does not match, your macOS build is not in the table: see [porting-awdl-to-new-macos-versions.md](porting-awdl-to-new-macos-versions.md#6-bluetoolfixup-for-the-new-build).
2. **Bluetooth on and working**: `system_profiler SPBluetoothDataType` shows the controller; IntelBluetoothFirmware and IntelBTPatcher are loaded.
3. **`awdl0` exists**: `ifconfig awdl0`. If not, check that `-itlawdl` is in `boot-args` (`nvram boot-args`) and that the loaded AirportItlwm is this fork's build (`system_profiler SPAirPortDataType | grep Firmware` shows `-awdl-`).
4. **AirDrop setting** on the Mac and on the iPhone: "Contacts Only" needs both devices signed in to iCloud with each other in Contacts; try "Everyone for 10 minutes" to rule this out.

### The Mac appears, the transfer starts, then "Failed" or stays at "Waiting"

This is the AWDL TX/timeline problem the TX pump and `-itlawdlfollow` fix. Check that both are on:

```sh
nvram boot-args | tr ' ' '\n' | grep -E 'itlawdl(txpump|follow|lead)'
tools/awdlctl/awdlctl show | grep -E 'AWDLTxPump|AWDLSyncMaster|AWDLPeerSync|AWDLDataLatencyMs'
```

- `AWDLTxFwBusy` growing fast while you transfer: the pump is off or `itlawdllead` is too small; try `sudo tools/awdlctl/awdlctl pump on` and `lead 30000`.
- `AWDLSyncMaster` says the Mac is master and `AWDLPeerSync` shows a non-zero slot offset: `follow` is off.
- Access point on a DFS channel (52–144): the AWDL channel sequence then often avoids your home channel. Try the AP on 36–48 or 149–165 as a test.

### The Mac takes a long time to reappear after a transfer

`AWDLDataMcast` should have a non-zero `rx` count. If it stays 0, you are on a build without the multicast fix.

### Kernel panic at boot

1. Boot without `-itlawdl` (OpenCore picker → edit boot-args, or your backup EFI). If it still panics, the problem is not AWDL.
2. If it only panics with AWDL on, boot with `-itlawdl -itlawdlnoinject` only. If that is stable, the panic is on the receive path: almost certainly an ABI difference on your macOS build. Open an issue with the panic report (`/Library/Logs/DiagnosticReports/*.panic`), your exact macOS build (`sw_vers`) and the kext build (`kmutil showloaded --list-only | grep Airport`).
3. If you built the kext yourself, run `tools/check-kext.sh` on it. A layout mismatch means stale objects: do a clean build.

### Wi-Fi got slower with AWDL on

Listening to AWDL takes the radio away from the home channel for short dwells. That is also true on real Macs, but Broadcom's firmware schedules it better. Remove `-itlawdlhometx` if you added it, keep `AWDLListenEveryNth` at 1, and report numbers (`iperf3` with and without `-itlawdl`).

### Bluetooth stops working with `-btlfxadvsets`

Remove `-btlfxadvsets`, reboot, and open an issue with `log show --last boot --predicate 'eventMessage CONTAINS "btlfx"'`.

## Opening an issue

Use the issue templates. Include: macOS build, card model, CPU/board, OpenCore version, kext versions, `boot-args`, what you did, what happened, and the redacted `summary.md` from `monitor-awdl.sh` if AirDrop is involved.
