# Methods and tools

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

The tools and recipes below were used on 26.7.2 (25G332); the KC tools also work on 15.8.1 (24H32). All of them read your own installed macOS; none of their output is shipped in this repo (see [macos-driver-development.md](../macos-driver-development.md#legal-boundaries-we-follow)).

## Repo tools

| Tool | What it does |
|---|---|
| `tools/build/build-clt.sh` | Builds AirportItlwm with the Command Line Tools only (no Xcode). Reads sources, defines and paths from the `.xcodeproj` with `tools/build/pbxinfo.py`; fails if any import does not resolve |
| `tools/check-kext.sh` | Before installing: imports against the system KC ("imports: none missing"), unique layout tag, struct sizes identical across compile units |
| `tools/kc/vtcheck.py` | Compares, slot by slot, the vtables of the kext's classes with those of their base classes in the KC |
| `tools/kc/kcdis.py <kc> <address> <size>` | Disassembles a range of the KC and symbolizes the calls |
| `tools/kc/kccalls.py <kc> <nm file> <regex> [--strings]` | For each KC function whose name matches the regex (for example the `AppleBCMWLAN` ones): direct calls, virtual slots called and, with `--strings`, the strings it references |
| `tools/kc/kcsyms.py` | Lists kexts and symbols in the KC and decodes vtables (see [porting-awdl-to-new-macos-versions.md](../porting-awdl-to-new-macos-versions.md)) |
| `tools/awdlctl/awdlctl` | Run-time switches of the AWDL driver (`awgate`, `idledwell`, `datastats`, …); changing them needs `sudo`. See [boot-args.md](../boot-args.md#run-time-switches-awdlctl) |
| `tools/airdrop/airdrop-send.swift` | Opens the AirDrop share picker for a file (start a transfer without the mouse) |
| `tools/airdrop/airdrop-measure.sh` | Summarizes AirDrop transfers from the log: connection type, QUIC errors, totals, body duration |
| `tools/airdrop/awdlquic.swift` | QUIC client/server over AWDL between two Macs (12 MB each way) |
| `tools/advsets/verify-advsets.pl` | Checks that the BlueToolFixup advsets patterns match exactly once in a `bluetoothd` binary |
| `scripts/monitor-awdl.sh -n <name>` | Full capture while you test: log, ioreg, samples, pcap of `awdl0` |

A screen-click helper (to pick the recipient in the picker) and a reboot-loop script were also used; they are not published.

The tools use the KC of the running system (`/System/Library/KernelCollections/BootKernelExtensions.kc`); the kernel is at `/System/Library/Kernels/kernel`.

## Recipes

### From the log to the function

1. Find the kernel log message that shows the wrong value.
2. Search for the string in the KC; the function that references it is the starting point.
3. Disassemble with `kcdis.py`, follow where the field comes from (offset in the object) and look for who writes it.
4. If the writer is a virtual method, see who calls it: if nobody in the family does, the driver has to.

### DTrace in the kernel

Needs SIP with DTrace allowed (see [opencore-patches.md](opencore-patches.md) §3) and a reboot. Put SIP back afterwards.

```
# which strings a process makes the kernel compare
fbt::strcmp:entry /execname == "pfctl"/ { printf("%s,%s", stringof(arg0), stringof(arg1)); }
# confirm the bytes of a patch in the running kernel
tracemem(<function address + offset>, 16)
```

### Unified log

- In zsh, `log` is a builtin: always use `/usr/bin/log`.
- `--style compact` and narrow predicates (`process`, `subsystem`, `eventMessage CONTAINS`).
- To wait for an event: `log stream` with a predicate, in a background process that exits on the first match and then runs the measurement.
- Private data (`<private>`) can be turned on temporarily with `Enable-Private-Data` in `/Library/Preferences/Logging/`; turn it off afterwards.

### Compare with a real Mac

Run the same read-only commands on both and compare line by line: sysctls, `ifconfig -v`, MD5 of configuration files, kernel log in the same situation. Same files with a different state point at the in-memory kernel; same state with a different result points at the driver.

### Diagnostic counters in ioreg

Every path that can reject frames has a counter and keeps the last rejected frame (length + first bytes). That is what tells a right hypothesis from a wrong one in minutes.

## Mistakes not to repeat

| Mistake | Consequence | Rule |
|---|---|---|
| Installing a build without checking imports | OpenCore refuses the kext, boot without Wi-Fi | `check-kext.sh` before installing; test first on a spare EFI |
| Reinstalling and rebooting a build that had already failed | A wasted reboot | When a result is unexpected, investigate before reinstalling |
| Mounting the sealed snapshot of the running system | System binaries fail until the next reboot | Don't mount it; read the binaries some other way |
| `plutil -insert Kernel.Patch.` without an index | Corrupted `config.plist` | Give the index; back up and `plutil -lint` |
| Measuring with the iPhone locked | A "failure" that doesn't exist | Confirm the peer's state before concluding |
| Measuring discovery with `sharingd` stuck | A "regression" blamed on the wrong change | Check the baseline (BLE advertising, detections) before comparing |
| Drawing conclusions from one sample | Wrong hypotheses (MTU 1484, HT Control, CE marks) | Repeat; keep it as a hypothesis until there is a second measurement |
| Fixing one function at a time | Several reboots for a single subsystem | Read the subsystem's whole contract first; one reboot per coherent set of changes |
| A verifier that reads too early | Needless `bluetoothd` restarts | A verifier has to tell "not applied" from "not loaded yet" |

## Test routine

1. Build, `check-kext.sh`, `vtcheck.py`.
2. Install on a test EFI only; keep the main EFI on the last good build so you can boot from it if the test fails.
3. Unmount the EFI partitions before rebooting.
4. After booting: UUID of the loaded kext, `sysctl net.filter_state`, counters at zero where they should be.
5. Measure, write down the result with time and build, and only then copy the build to the main EFI.
