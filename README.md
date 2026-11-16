# AirDrop and AWDL for Intel AX210 Wi-Fi on Hackintosh (macOS Sequoia and Tahoe)

[![CI](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/actions/workflows/ci.yml/badge.svg)](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/kevineduardo/ax210-airdrop-awdl-hackintosh?include_prereleases)](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/releases)
[![License: GPL-2.0 (kexts) / MIT (tools, docs)](https://img.shields.io/badge/license-GPL--2.0%20%7C%20MIT-blue)](#license)

Native Wi-Fi, AWDL (Apple Wireless Direct Link) and AirDrop for the Intel Wi-Fi 6E AX210 on a hackintosh, through macOS's own Wi-Fi stack. It works without HeliPort, a USB dongle or a Broadcom card swap.

This is a fork of [AirportItlwm](https://github.com/OpenIntelWireless/itlwm) (OpenIntelWireless, via laobamac and crazyhulk). crazyhulk wrote the first experimental AWDL code for Tahoe; this fork ports it to macOS 15 Sequoia, fixes what kept AirDrop transfers from finishing, and on macOS 26 Tahoe adds the pieces Apple's Wi-Fi stack expects from a real driver (peer cache, data statistics, QUIC pacing). It ships with a [BlueToolFixup](https://github.com/acidanthera/BrcmPatchRAM) build that turns on BLE advertising sets, so iPhones can find the Mac in the AirDrop sheet.

> **Status: experimental beta.** AirDrop works in both directions on the test machine, first on Sequoia 15.8.1 and then on Tahoe 26.7.2. Read [what is tested](#what-works) before you install it, and keep a known-good EFI on a USB stick.

> **Just want fast AirDrop?** Try [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) first. It is a separate AirportItlwm fork with its own AWDL port, and today it is ahead of this one: about 20 MB/s in both directions, more macOS builds (Tahoe 26.7.1 to 26.7.3, Sequoia 15.8.x), more Intel cards, Handoff and iMessage tested, and many more testers. This repo is the place for the research notes, the porting guides and the tools, and both projects exchange fixes. On Tahoe, read our [`PowerTimeoutKernelPanic` note](docs/reference/skywalk-pf-quic.md) whichever driver you use.

## What works

One test machine: AMD Ryzen 5 5600G desktop, Intel AX210 (PCIe, Bluetooth over USB), SMBIOS MacPro7,1, OpenCore 1.0.8. It ran Sequoia 15.8.1 first and Tahoe 26.7.2 later.

### Tahoe 26.7.2 (25G332), current release

| Feature | Status | Measured |
|---|---|---|
| Wi-Fi through the native macOS stack | Works | `networkQuality` about 200 Mbps down / 106 Mbps up |
| Wi-Fi with AWDL on | Works | gateway ping 17 to 20 ms with an iPhone nearby (a real Mac: 19 ms) |
| AirDrop iPhone → Mac | Works | 12 MB in 3.1 to 3.7 s; on the same network the file goes over Wi-Fi, not AWDL |
| AirDrop Mac → iPhone | Works, slow | 12 MB in about 12 s (1 MB/s) or 38 to 49 s, depending on the AWDL schedule |
| AWDL as master and as slave | Works | complete transfers in both roles |
| QUIC over `awdl0` | Works | 0 timing errors per transfer; needs the OpenCore change in [installation](docs/installation.md#4-tahoe-only-two-opencore-settings) |
| AirDrop discovery | Works, with failures | a few seconds when it works; the Mac sometimes drops out of the iPhone's list (workaround: `killall sharingd`) |
| BLE advertising sets | Works | all six patches applied in six boots in a row |
| Large files, sleep/wake, AirPods and other BT devices, Handoff and other Apple ID features | Not tested yet | |

Details and open problems: [docs/test-results-tahoe.md](docs/test-results-tahoe.md).

### Sequoia 15.8.1 (24H32)

| Feature | Status |
|---|---|
| Wi-Fi through the native macOS stack (5 GHz, 802.11ac 80 MHz), with AWDL on | Works |
| AirDrop iPhone → Mac (photos up to about 1 MB), Mac as AWDL master or slave | Works (4 of 4 in the last two test boots) |
| AirDrop Mac → iPhone (small file) | Works |
| Mac reappears in the iPhone's AirDrop list after a transfer | Works (6 to 9 s) |
| BLE advertising sets | Works |

These results are for the [v1.0.0-beta.1](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/releases/tag/v1.0.0-beta.1) Sequoia kext. Later releases also carry the Tahoe fixes into the Sequoia build (with the Tahoe-only kernel calls compiled out); that combination has not been tested on Sequoia hardware yet. Details: [docs/test-results.md](docs/test-results.md).

### macOS versions

| macOS | Wi-Fi | AWDL / AirDrop | Notes |
|---|---|---|---|
| 26 Tahoe | Yes | Yes, tested on 26.7.2 | Needs `PowerTimeoutKernelPanic` off in OpenCore. Layouts checked against the 25G332 kernel; BLE advsets tables for 26.7.2 and 26.4.1 |
| 15 Sequoia | Yes | Yes, tested on 15.8.1 (beta.1 kext) | Needs 15.2 or later. Other 15.x builds may have different IO80211Family vtables: start with `-itlawdlnoinject` and see [porting to a new macOS build](docs/porting-awdl-to-new-macos-versions.md) |
| 14 Sonoma and older | Use upstream [itlwm releases](https://github.com/OpenIntelWireless/itlwm/releases) | No | AirportItlwm's Sonoma builds use an older IO80211 interface and the AWDL code is only built for Sequoia and Tahoe. See [what a Sonoma port would take](docs/porting-awdl-to-new-macos-versions.md#sonoma-and-older) |

### Cards

The AWDL code lives in the `iwx` HAL, which drives the Intel Wi-Fi 6/6E family: AX200, AX201, AX210, AX211, AX411, the Killer AX1650 and some other 22000-series cards. Only the AX210 has been tested. Cards driven by the older HALs (`iwm`, `iwn`, for example AC 8265 or AC 7260) get Wi-Fi only.

## Quick start

1. Download from [Releases](https://github.com/kevineduardo/ax210-airdrop-awdl-hackintosh/releases):
   - `AirportItlwm-AWDL-Sequoia-Release.zip` (or `-Tahoe-`), and
   - `BlueToolFixup-advsets-Release.zip`.
2. Add both kexts to OpenCore, together with Lilu, [IntelBluetoothFirmware and IntelBTPatcher](https://github.com/OpenIntelWireless/IntelBluetoothFirmware). Remove `itlwm.kext` and any other AirportItlwm.
3. Add the boot arguments:
   ```
   -itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -btlfxadvsets
   ```
   On Tahoe also add `-itlawdlhometx -ibtcompatbeta`, and set `Kernel > Quirks > PowerTimeoutKernelPanic` to `false` in OpenCore.
4. Reboot, join your Wi-Fi network, open AirDrop on an iPhone and send a photo to the Mac.

Run `scripts/check-compat.sh` before and after installing: it checks your macOS build, card, kexts, boot arguments and network stack without changing anything.

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
| [Test results: Sequoia](docs/test-results.md) | The test log that took AirDrop from "ASK accepted, upload failed" to working |
| [Test results: Tahoe](docs/test-results-tahoe.md) | Tahoe 26.7.2: QUIC, peer cache, data rates, open problems |
| [Reference notes](docs/reference/) | IO80211 driver contract, AWDL protocol notes, OpenCore patches, Skywalk/pf/QUIC, best practices |
| [Engineering notes](docs/research/) | Port analysis, TX timing, panic analysis, master election, BLE advsets |
| [Changes from upstream](CHANGES-FROM-UPSTREAM.md) | Exactly what this fork changed |

## Tools in this repo

| Tool | What it is for |
|---|---|
| [`tools/awdlctl`](tools/awdlctl/awdlctl.c) | Read AWDL counters and change TX switches at run time (A/B tests without a reboot) |
| [`scripts/monitor-awdl.sh`](scripts/monitor-awdl.sh) | Records counters, logs and an optional pcap during AirDrop tries and writes a per-attempt timeline |
| [`scripts/check-compat.sh`](scripts/check-compat.sh) | Read-only check of macOS build, card, kexts, boot arguments and network stack, with a verdict |
| [`scripts/collect-diagnostics.sh`](scripts/collect-diagnostics.sh) | One-shot snapshot for bug reports |
| [`tools/kc/kcsyms.py`](tools/kc/kcsyms.py) | Lists symbols and decodes C++ vtables from **your own** kernel collection, to check driver headers against the running macOS |
| [`tools/check-kext.sh`](tools/check-kext.sh) | Post-build checks: struct-layout consistency across compile units, imports present in the kernel |
| [`tools/advsets/verify-advsets.pl`](tools/advsets/verify-advsets.pl) | Checks the BlueToolFixup patch patterns against your own `bluetoothd` |
| [`tools/kc/vtcheck.py`](tools/kc/vtcheck.py), [`kcdis.py`](tools/kc/kcdis.py), [`kccalls.py`](tools/kc/kccalls.py) | Compare a built kext's vtables with the kernel's, disassemble kernel functions, list calls and strings |
| [`tools/build/`](tools/build/) | Build the kexts with the Command Line Tools only, and inspect the Xcode project |
| [`tools/airdrop/`](tools/airdrop/) | AirDrop transfer summaries from the log, QUIC throughput test over `awdl0` |

Commands for every tool: [tools/README.md](tools/README.md).

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

Contributions are welcome: test reports on other machines, Sequoia testing of the current build, other Intel cards, new macOS builds, other Wi-Fi chips. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. In short:

- Every change needs tests and must pass `tests/run.sh` and CI.
- If you used an AI tool, say which one and which model in the pull request.
- Driver changes stay GPL-2.0; tools and docs are MIT.
- No Apple binaries, extracted kexts, kernel collections, disassembly listings or copied Apple headers.

## Credits

This project is built on other people's work. What we used from each:

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
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) | GPL-2.0 | The idea and the vtable anchors of the AWDL build guard (AirDrop only starts on verified macOS builds), the user compatibility checker that `scripts/check-compat.sh` is modeled on, and the AWDL coex listener engine behind the opt-in `-itlawdlcoex` backend (listener context beside Wi-Fi, country answers, sync and data TX, send gate) with its header-only helpers and host tests (ported, GPL-2.0) |
| Intel `iwlwifi` firmware | [redistributable](driver/AirportItlwm/iwlwifi-firmware-license) | Firmware bundled in the kext by upstream |

**AI disclosure.** This port was developed by [Kevin Eduardo](https://github.com/kevineduardo) with AI coding assistance from Anthropic's Claude models (Claude Opus 5.5 for most of the work, Claude Fable 5.2 for the panic analysis and the AirDrop failure analysis). Every change was built, checked with the tools in this repo and tested on real hardware before it was kept.

## License

- `driver/AirportItlwm` and `driver/BlueToolFixup`: **GPL-2.0**, like the projects they come from. Our changes to them are GPL-2.0 too; changed files carry a notice at the top and are listed in [CHANGES-FROM-UPSTREAM.md](CHANGES-FROM-UPSTREAM.md).
- Everything else (docs, tools, scripts, tests, CI): **MIT**, see [LICENSE](LICENSE).
- Exception: `tests/unit/awdlcoex/` holds host tests ported from ForbiddenItlwm. They test GPL-2.0 driver headers and stay **GPL-2.0** (each file says so in its SPDX line). The rest of `tests/` is MIT.

## Legal

This project is not affiliated with or endorsed by Apple, Intel or any upstream project. AirDrop, AWDL, macOS and Mac are trademarks of Apple Inc.; Intel is a trademark of Intel Corporation.

The repository contains **no Apple code or binaries**. The interface headers under `driver/AirportItlwm/include/Airport` declare only the class layouts and method names a third-party driver needs to interoperate with IO80211Family. Anything that would require Apple's software (checking layouts, finding byte patterns in `bluetoothd`) is described as a step you run on your own licensed copy of macOS, and its raw output is never committed.

Apple's macOS license allows installing macOS only on Apple-branded computers. Whether you run macOS on other hardware is your decision and your responsibility.

---

<sub>Keywords: hackintosh AirDrop Intel Wi-Fi, AX210 AirDrop, AirportItlwm AWDL, itlwm AirDrop, AWDL Intel, AirDrop on hackintosh Sequoia, AirDrop on hackintosh Tahoe, Intel Wi-Fi 6E macOS, AX200 AX201 AX211 macOS, OpenCore Wi-Fi AirDrop, Continuity hackintosh Intel, native Wi-Fi hackintosh without Broadcom.</sub>
