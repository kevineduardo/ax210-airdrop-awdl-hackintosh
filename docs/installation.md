# Installation

This guide assumes you already boot macOS with OpenCore and know how to edit `config.plist`. If you don't, start with the [Dortania OpenCore Install Guide](https://dortania.github.io/OpenCore-Install-Guide/).

> **Before you start:** copy your working `EFI` folder to a USB stick and make sure you can boot from it. The AWDL code is experimental and a bad combination of kext and macOS build can panic at boot.

## Before you start: run `scripts/check-compat.sh`

```sh
scripts/check-compat.sh            # add --no-log to skip reading this boot's log
```

It changes nothing and does not need `sudo`. Before installing it tells you whether your macOS build and Wi-Fi card are the tested ones (Sequoia 15.8.1 or Tahoe 26.7.2, Intel AX210). After installing, run it again: it checks that the kexts are loaded, that `awdl0` is up, that the boot arguments match the tested set, that the BLE advertising-sets patches applied and, on Tahoe, that user-space networking is on. Each line says OK, WARN or FAIL. It exits with 1 when something FAILs. A FAIL for the kexts or `awdl0` is expected before you install.

## 1. What you need

| Item | Where to get it |
|---|---|
| `AirportItlwm.kext` for your macOS (Sequoia or Tahoe) | `AirportItlwm-AWDL-<macOS>-Release.zip` from [Releases](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/releases) |
| `BlueToolFixup.kext` with the advsets table | `BlueToolFixup-advsets-Release.zip` from the same release |
| `Lilu.kext` 1.7.x | [acidanthera/Lilu](https://github.com/acidanthera/Lilu/releases) |
| `IntelBluetoothFirmware.kext` and `IntelBTPatcher.kext` | [OpenIntelWireless/IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware/releases) |
| `awdlctl` (optional, for diagnostics) | `awdlctl-universal.zip` from the release, or `make -C tools/awdlctl` |

Check the downloads against `SHA256SUMS.txt` in the release:

```sh
shasum -a 256 -c SHA256SUMS.txt --ignore-missing
```

Use the Release builds for daily use. The Debug builds log much more and are meant for bug reports.

## 2. Remove what conflicts

- `itlwm.kext` (and HeliPort): AirportItlwm replaces both.
- Any other `AirportItlwm.kext`.
- Any other `BlueToolFixup.kext` (the one in this release replaces it; it is a normal BlueToolFixup plus the advsets tables).
- `BrcmPatchRAM*.kext` / `BrcmFirmwareData.kext`: only needed for Broadcom Bluetooth.

## 3. Add the kexts to OpenCore

Copy the kexts to `EFI/OC/Kexts/` and add them to `Kernel → Add` in this order:

| # | BundlePath | ExecutablePath | MinKernel | MaxKernel |
|---|---|---|---|---|
| 1 | `Lilu.kext` | `Contents/MacOS/Lilu` | | |
| 2 | `AirportItlwm.kext` | `Contents/MacOS/AirportItlwm` | Sequoia build: `24.2.0`<br>Tahoe build: `25.0.0` | Sequoia build: `24.99.99`<br>Tahoe build: leave empty |
| 3 | `IntelBluetoothFirmware.kext` | `Contents/MacOS/IntelBluetoothFirmware` | | |
| 4 | `IntelBTPatcher.kext` | `Contents/MacOS/IntelBTPatcher` | | |
| 5 | `BlueToolFixup.kext` | `Contents/MacOS/BlueToolFixup` | | |

Lilu must load before IntelBTPatcher and BlueToolFixup, which are Lilu plugins. If you keep both the Sequoia and the Tahoe AirportItlwm in the EFI (renamed folders, for example `AirportItlwm-Sequoia.kext` and `AirportItlwm-Tahoe.kext`), the `MinKernel`/`MaxKernel` values above make sure only the right one loads.

Other OpenCore settings (`SecureBootModel`, `DmgLoading`, and so on) are the same as for upstream AirportItlwm; this fork does not need anything extra. The test machine uses `SecureBootModel=Disabled`.

## 4. Tahoe only: two OpenCore settings

On macOS 26 (Darwin 25.x) AirDrop needs user-space networking (Skywalk) to work, and one OpenCore quirk silently turns it off:

1. Set `Kernel > Quirks > PowerTimeoutKernelPanic` to `false`. On kernel 25.6 the quirk falls back to rewriting the first `"com.apple\0"` string in the kernel, which the linker shares with pf's anchor check. pf then stops recognising Apple's anchors, user-space networking is disabled for the whole machine, and AirDrop's QUIC transfers stall. This happens with any network card.
2. Optionally add the replacement kernel patch for `IOService::ackTimerTick` (same intended effect, without touching the string). The table with Find, Mask, Replace and the kernel range is in [reference/opencore-patches.md](reference/opencore-patches.md).

Check after rebooting:

```sh
sysctl net.filter_state net.link.generic.system.enable_netagent   # expect: empty, and 1
```

If you see `pf` and `0`, read [reference/skywalk-pf-quic.md](reference/skywalk-pf-quic.md).

## 5. Boot arguments

Add to `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`:

Sequoia:

```
-itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -btlfxadvsets
```

Tahoe (adds `-itlawdlhometx`, and `-ibtcompatbeta` for IntelBluetoothFirmware):

```
-itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -itlawdlhometx -btlfxadvsets -ibtcompatbeta
```

For the first boots also add `keepsyms=1 debug=0x100`. If `boot-args` is listed in `NVRAM → Delete`, the new value replaces the old one at the next boot; otherwise reset NVRAM once.

What every switch does: [boot-args.md](boot-args.md).

On a macOS build nobody has tested yet (anything other than Sequoia 15.8.1 and Tahoe 26.7.2 so far), boot once with only `-itlawdl -itlawdlnoinject`, check that Wi-Fi works and that there is no panic, then add the rest. See [porting-awdl-to-new-macos-versions.md](porting-awdl-to-new-macos-versions.md).

## 6. Check that everything loaded

```sh
kmutil showloaded --list-only | grep -iE 'Airport|IntelBluetooth|BTPatcher|BlueTool|Lilu'
ifconfig awdl0                      # should exist; flags UP,RUNNING while AirDrop is open
log show --last boot --predicate 'eventMessage CONTAINS "[advsets]"'   # BLE advertising sets
system_profiler SPAirPortDataType | grep -E 'Firmware Version|Status'
```

`Firmware Version` should start with `itlwm: 2.4.0-awdl-<commit>` for builds from this repo.

Then, on an iPhone signed in to iCloud (or with AirDrop set to "Everyone for 10 minutes"), open the share sheet and send a photo to the Mac. The first discovery can take 5 to 10 seconds.

If something does not work, run `scripts/check-compat.sh` and `scripts/collect-diagnostics.sh` and see [troubleshooting.md](troubleshooting.md).

## 7. Rollback

If the Mac panics or does not boot:

1. Boot from the USB stick with your old EFI, or pick the macOS entry in OpenCore and press Space for options if you kept a second config.
2. Remove `-itlawdl` (and every other `-itlawdl*`) from `boot-args`. Without `-itlawdl` the kext behaves like upstream AirportItlwm.
3. If it still panics, put back the AirportItlwm you used before.

## SIP and security notes

Before changing `config.plist`, copy it somewhere safe and run `plutil -lint` afterwards. If you edit it with `plutil -insert`, always give the array index (`Kernel.Patch.25`); an empty index corrupts the file.

The test machine runs with SIP partly disabled for reasons that have nothing to do with Wi-Fi. Kexts injected by OpenCore do not need SIP disabled, so this setup should also work with SIP enabled, but that has not been tested yet. Reports are welcome.

`awdlctl` changes driver settings only when run by an administrator (`sudo`); the driver checks this itself.
