# AirDrop and AWDL for Intel AX210 Wi-Fi on Hackintosh (macOS Sequoia and Tahoe)

[![CI](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/actions/workflows/ci.yml/badge.svg)](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/kevineduardo/ax210-airdrop-awdl-hackintosh?include_prereleases)](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/releases)
[![License: GPL-2.0 (kexts) / MIT (tools, docs)](https://img.shields.io/badge/license-GPL--2.0%20%7C%20MIT-blue)](#license)

Native Wi-Fi, **AWDL** (Apple Wireless Direct Link) and **AirDrop** for the **Intel Wi-Fi 6E AX210** on a hackintosh, through macOS's own Wi-Fi stack. No HeliPort, no USB dongle, no Broadcom card swap.

This is a fork of [AirportItlwm](https://github.com/OpenIntelWireless/itlwm) (OpenIntelWireless, via laobamac and crazyhulk) that brings crazyhulk's experimental Tahoe AWDL code to **macOS 15 Sequoia** and fixes what kept AirDrop transfers from finishing. It ships with a [BlueToolFixup](https://github.com/acidanthera/BrcmPatchRAM) build that turns on BLE advertising sets, so iPhones can find the Mac in the AirDrop sheet.

> **Status: experimental beta.** AirDrop works in both directions on the test machine (Sequoia 15.8.1, AX210). Read [what is tested](#what-works) before you install it, and keep a known-good EFI on a USB stick.

## What works

Tested on an AMD Ryzen 5 5600G desktop with an Intel AX210 (PCIe, Bluetooth over USB), SMBIOS MacPro7,1, OpenCore 1.0.8, **macOS 15.8.1 Sequoia (24H32)**.

| Feature | Status |
|---|---|
| Wi-Fi through the native macOS stack (5 GHz, 802.11ac 80 MHz), with AWDL on | Works |
| `awdl0` up, AWDL peers, Bonjour `_airdrop._tcp` on `awdl0` | Works |
| AirDrop iPhone → Mac (photos, up to ~1 MB), Mac as AWDL master or slave | Works (4 of 4 in the last two test boots, with `-itlawdltxpump` and `-itlawdlfollow`) |
| AirDrop Mac → iPhone (small file) | Works |
| Mac reappears in the iPhone's AirDrop list after a transfer | Works (6 to 9 s, including reopening the share sheet) |
| Bluetooth controller and BLE advertising sets (AirDrop/Handoff discovery) | Works |
| Large files (> 500 MB), many transfers in a row, Mac ↔ Mac | Not tested yet |
| Handoff, Universal Clipboard, AirPlay to Mac, Sidecar, Universal Control | Not tested yet |
| Sleep/wake, 24 h stability, AirPods and other BT devices | Not tested yet |

### macOS versions

| macOS | Wi-Fi | AWDL / AirDrop | Notes |
|---|---|---|---|
| 15 **Sequoia** | Yes | **Yes, tested on 15.8.1** | AWDL layouts checked against the 15.8.1 kernel. Other 15.x builds may have different IO80211Family vtables (15.0.1 does): start with `-itlawdlnoinject` and see [porting to a new macOS build](docs/porting-awdl-to-new-macos-versions.md). |
| 26 **Tahoe** | Yes | Builds; AWDL code is crazyhulk's original plus our fixes | Our Tahoe build is **not tested on hardware yet**. Reports welcome. The BLE advsets table targets 26.4.1. |
| 14 **Sonoma** and older | Use upstream [itlwm releases](https://github.com/OpenIntelWireless/itlwm/releases) | **No** | Sonoma uses the older IO80211 family, where nobody has implemented AWDL for Intel cards yet. See [docs/porting-awdl-to-new-macos-versions.md](docs/porting-awdl-to-new-macos-versions.md#sonoma-and-older) if you want to try. |

### Cards

The AWDL code lives in the `iwx` HAL, which drives the Intel Wi-Fi 6/6E family: **AX200, AX201, AX210, AX211, AX411**, the Killer AX1650 and some other 22000-series cards. Only the **AX210** has been tested. Cards driven by the older HALs (`iwm`, `iwn`, for example AC 8265 or AC 7260) get Wi-Fi only.

## Quick start

1. Download from [Releases](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/releases):
   - `AirportItlwm-AWDL-Sequoia-Release.zip` (or `-Tahoe-`), and
   - `BlueToolFixup-advsets-Release.zip`.
2. Add both kexts to OpenCore, together with Lilu, [IntelBluetoothFirmware and IntelBTPatcher](https://github.com/OpenIntelWireless/IntelBluetoothFirmware). Remove `itlwm.kext` and any other AirportItlwm.
3. Add the boot arguments:
   ```
   -itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -btlfxadvsets
   ```
4. Reboot, join your Wi-Fi network, open AirDrop on an iPhone and send a photo to the Mac.

The full guide, including the OpenCore entries and how to check that everything loaded, is in [docs/installation.md](docs/installation.md). Every boot argument is explained in [docs/boot-args.md](docs/boot-args.md).

## Documentation

| | |
|---|---|
| [Installation](docs/installation.md) | OpenCore config, kext order, verification, rollback |
| [Boot arguments](docs/boot-args.md) | Every AWDL and BlueToolFixup switch |
| [Troubleshooting](docs/troubleshooting.md) | Diagnostics scripts, common failures, what to put in a bug report |
| [How AWDL works in this driver](docs/how-awdl-works.md) | Architecture: from IO80211Family to the Intel firmware |
| [Porting AWDL to a new macOS version](docs/porting-awdl-to-new-macos-versions.md) | What depends on the macOS ABI and how to verify it on your own Mac |
| [Porting to other Wi-Fi chips (Realtek, Atheros…)](docs/porting-to-other-wifi-chips.md) | Step-by-step plan for doing the same on different hardware |
| [macOS driver development notes](docs/macos-driver-development.md) | Tools, techniques and the legal boundaries we follow |
| [Test results](docs/test-results.md) | The test log that took AirDrop from "ASK accepted, upload failed" to working |
| [Engineering notes](docs/research/) | Port analysis, TX timing, panic analysis, master election, BLE advsets |
| [Changes from upstream](CHANGES-FROM-UPSTREAM.md) | Exactly what this fork changed |

## Tools in this repo

| Tool | What it is for |
|---|---|
| [`tools/awdlctl`](tools/awdlctl/awdlctl.c) | Read AWDL counters and change TX switches at run time (A/B tests without a reboot) |
| [`scripts/monitor-awdl.sh`](scripts/monitor-awdl.sh) | Records counters, logs and an optional pcap during AirDrop tries and writes a per-attempt timeline |
| [`scripts/collect-diagnostics.sh`](scripts/collect-diagnostics.sh) | One-shot snapshot for bug reports |
| [`tools/kc/kcsyms.py`](tools/kc/kcsyms.py) | Lists symbols and decodes C++ vtables from **your own** kernel collection, to check driver headers against the running macOS |
| [`tools/check-kext.sh`](tools/check-kext.sh) | Post-build checks: struct-layout consistency across compile units, imports present in the kernel |
| [`tools/advsets/verify-advsets.pl`](tools/advsets/verify-advsets.pl) | Checks the BlueToolFixup patch patterns against your own `bluetoothd` |

## Building

Kexts are built by [CI](.github/workflows/ci.yml) on GitHub's macOS runners. To build locally you need Xcode 16 and two third-party SDKs that are not stored in this repo:

```sh
git clone https://github.com/acidanthera/MacKernelSDK driver/AirportItlwm/MacKernelSDK
git -C driver/AirportItlwm/MacKernelSDK checkout 3f750085caa17ec3a7880f11c11bf4f48cd6a164
cd driver/AirportItlwm
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Sequoia -configuration Release \
  ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO SYMROOT=$PWD/build/products build
../../tools/check-kext.sh build/products/Release/Sequoia/AirportItlwm.kext
```

BlueToolFixup also needs `Lilu.kext` (1.7.2 DEBUG) in `driver/BlueToolFixup/`. See [CONTRIBUTING.md](CONTRIBUTING.md#building).

Always do clean builds after touching `ieee80211_var.h`; [this is why](docs/research/panic-stale-struct-layout.md).

## Contributing

Contributions are welcome: test reports on other machines, Tahoe testing, other Intel cards, new macOS builds, other Wi-Fi chips. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. In short:

- Every change needs tests and must pass `tests/run.sh` and CI.
- If you used an AI tool, say which one and which model in the pull request.
- Driver changes stay GPL-2.0; tools and docs are MIT.
- No Apple binaries, extracted kexts, kernel collections, disassembly listings or copied Apple headers.

## Credits

This project stands on other people's work. What we used from each:

| Project | License | What we used |
|---|---|---|
| [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm) by zxystd and contributors | GPL-2.0 | The whole Intel Wi-Fi driver: the `iwx`/`iwm`/`iwn` HALs, the OpenBSD net80211 port and the AirportItlwm glue to IO80211Family |
| [laobamac/itlwm](https://github.com/laobamac/itlwm) | GPL-2.0 | Sequoia/Tahoe (IO80211 "WCL") support and the reconstructed Skywalk headers that crazyhulk's work builds on |
| [crazyhulk/itlwm](https://github.com/crazyhulk/itlwm) @ ff8db43 | GPL-2.0 | **The experimental AWDL implementation for Tahoe** (`awdl0`, AW timeline, P2P listen, AWDL TX/RX, data path). Our fork starts from this commit |
| [Unopcoooo/itlwm](https://github.com/Unopcoooo/itlwm) (`awdl/sequoia-ac9560-preview`) | GPL-2.0 | Reconstructed macOS 15 layouts of `IO80211VirtualInterface` and `IO80211AWDLProtocol`, and findings about `wifip2pd` on 24H32. Our Sequoia headers are derived from them |
| [acidanthera/BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM) | GPL-2.0 | BlueToolFixup, which patches `bluetoothd` for non-Apple Bluetooth controllers |
| [crazyhulk/BlueToolFixup-FixA2DP-BLE](https://github.com/crazyhulk/BlueToolFixup-FixA2DP-BLE) @ 0fb661b | GPL-2.0 | The `-btlfxadvsets` BLE advertising-sets patches for Tahoe 26.4.1, which our 15.8.1 table extends |
| [acidanthera/Lilu](https://github.com/acidanthera/Lilu) | BSD-3-Clause | Kernel patching framework BlueToolFixup runs on (build-time dependency, not stored here) |
| [acidanthera/MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | see repo | Kernel headers for building the kexts (build-time dependency, not stored here) |
| [OpenIntelWireless/IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) | GPL-3.0 | Bluetooth firmware loader and IntelBTPatcher, used at run time (not included) |
| OpenBSD `iwx(4)` / `net80211`, Linux `iwlwifi` / `iwlmvm` | ISC / BSD / GPL-2.0 | The drivers itlwm was ported from; iwlmvm's P2P-device approach is what our P2P listen copies |
| Intel `iwlwifi` firmware | [redistributable](driver/AirportItlwm/iwlwifi-firmware-license) | Firmware bundled in the kext by upstream |

**AI disclosure.** This port was developed by [Kevin Eduardo](https://github.com/kevineduardo) with AI coding assistance from Anthropic's Claude models (Claude Opus 5.5 for most of the work, Claude Fable 5.2 for the panic analysis and the AirDrop failure analysis). Every change was built, checked with the tools in this repo and tested on real hardware before it was kept.

## License

- `driver/AirportItlwm` and `driver/BlueToolFixup`: **GPL-2.0**, like the projects they come from. Our changes to them are GPL-2.0 too; changed files carry a notice at the top and are listed in [CHANGES-FROM-UPSTREAM.md](CHANGES-FROM-UPSTREAM.md).
- Everything else (docs, tools, scripts, tests, CI): **MIT**, see [LICENSE](LICENSE).

## Legal

This project is not affiliated with or endorsed by Apple, Intel or any upstream project. AirDrop, AWDL, macOS and Mac are trademarks of Apple Inc.; Intel is a trademark of Intel Corporation.

The repository contains **no Apple code or binaries**. The interface headers under `driver/AirportItlwm/include/Airport` declare only the class layouts and method names a third-party driver needs to interoperate with IO80211Family. Anything that would require Apple's software (checking layouts, finding byte patterns in `bluetoothd`) is described as a step you run on your own licensed copy of macOS, and its raw output is never committed.

Apple's macOS license allows installing macOS only on Apple-branded computers. Whether you run macOS on other hardware is your decision and your responsibility.

---

<sub>Keywords: hackintosh AirDrop Intel Wi-Fi, AX210 AirDrop, AirportItlwm AWDL, itlwm AirDrop, AWDL Intel, AirDrop on hackintosh Sequoia, AirDrop on hackintosh Tahoe, Intel Wi-Fi 6E macOS, AX200 AX201 AX211 macOS, OpenCore Wi-Fi AirDrop, Continuity hackintosh Intel, native Wi-Fi hackintosh without Broadcom.</sub>
