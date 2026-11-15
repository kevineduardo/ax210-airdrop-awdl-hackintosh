# Tools

All tools run on your own Mac. The kernel-collection tools read `/System/Library/KernelCollections/BootKernelExtensions.kc` in place; keep their output local (see [docs/macos-driver-development.md](../docs/macos-driver-development.md#legal-boundaries-we-follow)). Python tools need only the standard library.

## Driver diagnostics

| Tool | Use |
|---|---|
| `awdlctl/` | AWDL counters and run-time switches. `make -C tools/awdlctl`, then `tools/awdlctl/awdlctl show` or `sudo tools/awdlctl/awdlctl pump on`. Every switch is listed in [docs/boot-args.md](../docs/boot-args.md#run-time-switches-awdlctl). |
| `check-kext.sh` | Post-build checks on `AirportItlwm.kext`: one struct layout in every compile unit, and every import present in the running kernel (`NO_KC=1` to skip that part, or pass a symbol list). |

## Kernel collection (`kc/`)

| Tool | Use |
|---|---|
| `kcsyms.py` | List the kexts and symbols in a kernel collection, decode C++ vtables. `kcsyms.py vtable IO80211VirtualInterface`, `kcsyms.py exports > kc-syms.txt` |
| `vtcheck.py` | Compare each class of a built kext with its base class in the kernel collection; exits 1 when a slot does not line up. |
| `kcdis.py` | Disassemble an address range of the kernel collection with symbol names on calls and jumps. |
| `kccalls.py` | For every function matching a regex: direct calls, virtual-call offsets and, with `--strings`, the C strings it loads. |

```sh
python3 -I tools/kc/vtcheck.py AirportItlwm.kext/Contents/MacOS/AirportItlwm \
    /System/Library/KernelCollections/BootKernelExtensions.kc \
    AirportItlwmSkywalkInterface:IO80211InfraProtocol AirportItlwmAWDLInterface:IO80211AWDLProtocol
python3 -I tools/kc/vtcheck.py --dump <kernel-collection> Class [slot-offset-hex ...]
python3 -I tools/kc/kcdis.py <kernel-collection> <vmaddr-hex> <length-hex> [out-prefix]
python3 -I tools/kc/kccalls.py <kernel-collection> - '<mangled-name-regex>' [--strings]
```

`kccalls.py` takes `-` to read the symbols from the collection itself, or a saved `nm <kc>` listing.

## Building without Xcode (`build/`)

| Tool | Use |
|---|---|
| `build-airportitlwm-clt.sh` | Build `AirportItlwm.kext` with the Command Line Tools only, then run `check-kext.sh`. `tools/build/build-airportitlwm-clt.sh [Tahoe\|Sequoia] [Release\|Debug]`. Environment: `JOBS`, `SDK_OVERRIDE`, `GIT_COMMIT`, `CHECK_IMPORTS=0\|1`. Output in `driver/AirportItlwm/build/clt/<Conf>/<Platform>/`. |
| `build-bluetoolfixup-clt.sh` | Same for `BlueToolFixup.kext`; needs `driver/BlueToolFixup/Lilu.kext` and `MacKernelSDK`. Output in `driver/BlueToolFixup/build/clt/`. |
| `pbxinfo.py` | Show what `xcodebuild` would use for a target: sources, build settings and `-D` flags. `pbxinfo.py <project.xcodeproj> <target> <configuration> [--sources \| --setting NAME \| --defines]` |

## AirDrop measurement (`airdrop/`)

| Tool | Use |
|---|---|
| `airdrop-measure.sh` | Summarise recent AirDrop transfers from the system log: flows per interface, QUIC connections and totals, HTTP/3 durations. `tools/airdrop/airdrop-measure.sh [30m]` |
| `awdlquic.swift` | QUIC throughput test between two Macs over `awdl0` (or any interface). Build with `swiftc -O -o awdlquic tools/airdrop/awdlquic.swift`; run `./awdlquic server <identity.p12> <password>` on one Mac and `./awdlquic client [ifname]` on the other. |
| `airdrop-send.swift` | Open the AirDrop share sheet for one file, for repeatable send tests. `swiftc -O -o airdrop-send tools/airdrop/airdrop-send.swift && ./airdrop-send <file>` |

A throwaway identity for `awdlquic`:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 2 -subj /CN=awdlquic
openssl pkcs12 -export -inkey key.pem -in cert.pem -out id.p12 -passout pass:test
```

## Other

| Tool | Use |
|---|---|
| `advsets/verify-advsets.pl` | Check the BlueToolFixup patterns against your own `/usr/sbin/bluetoothd`. |
| `../scripts/monitor-awdl.sh` | Record counters, logs and an optional pcap during AirDrop attempts; writes a per-attempt timeline. |
| `../scripts/collect-diagnostics.sh` | One-shot snapshot for bug reports. |
| `../scripts/boot-cycles.sh` | Unattended reboot loop that records whether the BLE advsets patches applied on each boot. `sudo scripts/boot-cycles.sh --start [N] [--wait SECONDS]`, `--status`, `--stop`. Log in `/var/log/ax210-boot-cycles.log`. |
