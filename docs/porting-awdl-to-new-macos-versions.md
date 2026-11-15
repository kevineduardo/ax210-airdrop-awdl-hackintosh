# Porting AWDL to a new macOS version

This is the process we used to bring crazyhulk's Tahoe AWDL code to Sequoia. Follow it when a new macOS release (or a new minor build) breaks AWDL, or to try older releases.

Everything that reads Apple software happens **on your own Mac, on your own licensed copy of macOS**. Do not commit kernel collections, extracted kexts, `bluetoothd`, disassembly or raw symbol dumps. Commit only the declarations a driver needs (method names, slot order, struct sizes) and a short note of how you checked them.

## 0. Know what can break

The AWDL logic is portable. These parts depend on the IO80211Family ABI of each macOS build:

| What | Where | How it breaks |
|---|---|---|
| `IO80211VirtualInterface` and `IO80211AWDLProtocol` vtable layout | `driver/AirportItlwm/include/Airport/macOS15/`, `macOS26/` | A slot added or removed shifts every later method: the family calls the wrong function and the machine panics |
| `IO80211Buffer` / `IO80211BufferPool` access | `AirportItlwmAWDLInterface.{hpp,cpp}` (`awdlPoolGetBuffer`, `awdlBuf*`) | Tahoe uses vtable calls; on Sequoia `IO80211Buffer` has no virtual methods and is called by symbol |
| RX metadata passed to `bpfTapInput` (0x48 bytes) | `AWDLRxMeta` | Wrong fields → frames dropped or misparsed by the peer manager |
| AW event IDs and payload offsets (`sync_state`, `channel_sequence`, OOB request, sync frame template, sync params) | `AirportItlwmAWDLInterface.cpp`, `apple80211_ioctl.h` | Wrong timeline, no PSF, or a struct-size check fails |
| Capability bits that make `wifip2pd` use AWDL | `AirportItlwmV2.cpp` (`AIRPORT_WCL` block) | `awdl0` is never created |
| Imported symbols | whole kext | The kext does not load ("symbol not found") |
| BLE advertising-sets patterns | `driver/BlueToolFixup/BrcmPatchRAM/BlueToolFixup.cpp` | No BLE advertising: the iPhone never sees the Mac |

The full analysis for 15 vs 26 is in [research/awdl-sequoia-port-analysis.md](research/awdl-sequoia-port-analysis.md).

## 1. Check the imports

Build the kext (or download it from CI), then compare its imports with what your kernel exports:

```sh
tools/kc/kcsyms.py exports > /tmp/kc-syms.txt          # reads /System/Library/KernelCollections/BootKernelExtensions.kc
tools/check-kext.sh AirportItlwm.kext /tmp/kc-syms.txt
```

Any `FAIL imports not in the kernel collection` line is a symbol the new build no longer exports (or does not export yet). For example, the Sequoia build checked against a 15.0.1 kernel collection misses 14 symbols in `IO80211Controller`/`IO80211SkywalkInterface`: that is why the Sequoia target needs 15.2 or later.

On Apple-silicon Macs, `BootKernelExtensions.kc` still contains an x86_64 kernel collection that `kcsyms.py` can read, which is handy for a quick check, but it is only meaningful if it is the same macOS build as the target machine.

## 2. Compare the vtables

```sh
tools/kc/kcsyms.py vtable IO80211VirtualInterface > /tmp/vi.txt
tools/kc/kcsyms.py vtable IO80211AWDLProtocol     > /tmp/awdl.txt
tools/kc/kcsyms.py vtable IO80211SkywalkInterface > /tmp/sky.txt
tools/kc/kcsyms.py vtable IO80211Buffer           # on 15.8.1 this ends at 0x110: no virtual methods of its own
tools/kc/kcsyms.py vtable IO80211BufferPool       # getBuffer must be at 0x118
```

Each line is `slot offset, target address, symbol`. Compare the order with the header for that macOS version. Rules of thumb:

- The ancestors' slots (OSObject → IOService → … → IO80211SkywalkInterface) must match exactly; a difference there means the parent headers are wrong too.
- New methods of a class come right after its parent's last slot, in declaration order. `_RESERVED…` slots count.
- `___cxa_pure_virtual` slots are methods the subclass (us) must implement. `IO80211AWDLProtocol` is all pure virtuals: 63 on both 15.8.1 and 26.
- A slot that exists on one version and not the other (`flushFlowQueues`, `p2pMulticastTx(unsigned, bool)` and `signalMulticastPackets` exist on Tahoe only) must be removed from that version's header, not left as a placeholder.

Do not assume all minor releases of one major version share a layout: the `IO80211VirtualInterface` vtable of 15.0.1 differs from 15.8.1 (for example `getPeerSlotDataStats` is at 0xe60 on 15.0.1, where 15.8.1 has `init`).

## 3. Write or adjust the headers

- Put a new layout in its own directory (`include/Airport/macOS15/`, `macOS26/`) and select it in the shims (`include/Airport/IO80211VirtualInterfaceWCL.h`, `IO80211AWDLProtocol.h`) by `__IO80211_TARGET`.
- Declare every slot in order. Add `static_assert(sizeof(Class) == 0x…)` for the instance size you measured.
- At the top of the header, write which build you verified against and how (one or two lines, no raw dumps).
- If a method changed its calling convention (for example `setLinkState` returns `bool` on 24H32), note it.

## 4. Check the data layouts that are not in headers

Some payloads are copied by raw offset (`sync_state`, `channel_sequence`, OOB requests, `bpfTapInput` metadata). They were verified by reading how Apple's own Broadcom driver (`AppleBCMWLANCoreMac`, `AppleBCMWLANProximityInterface`) fills them, in a disassembler, on the local kernel collection. Tools that work for this: [ipsw](https://github.com/blacktop/ipsw) (`ipsw kernel extract` to get one kext out of a kernel collection, `ipsw macho disass`), Ghidra, Hopper or IDA. Write down only the conclusion (offset, width, meaning), as in [research/awdl-sequoia-port-changes.md](research/awdl-sequoia-port-changes.md).

## 5. Bring it up in stages

Never test a new ABI with everything on. Use the boot-args in this order, one reboot each, and keep a known-good EFI on a USB stick:

1. `-itlawdl -itlawdlnoinject`: `awdl0` is created, nothing is handed to IO80211Family. Wi-Fi must stay stable.
2. add `-itlawdlaw`: AW timeline and AW events (`log stream --predicate 'eventMessage CONTAINS[c] "awdl"'`).
3. add `-itlawdllisten -itlawdlp2p`: `AWDLRxNoInject` rises in `ioreg`.
4. remove `-itlawdlnoinject`: frames reach the peer manager; `awdl0` should list peers (`AWDLStateDump … peers:N` in the log).
5. add `-itlawdltx`, then `-itlawdldata -itlawdlpromisc`, then `-itlawdltxpump itlawdllead=30000 -itlawdlfollow`.

Always boot with `keepsyms=1 debug=0x100`, so a panic report names the function. Symbolicate with the dSYM from CI (`atos -o AirportItlwm.kext.dSYM/... -l <load address> <pc>`).

## 6. BlueToolFixup for the new build

1. Find the new `bluetoothd` UUID and check whether the existing patterns still match exactly once:
   ```sh
   perl tools/advsets/verify-advsets.pl /usr/sbin/bluetoothd driver/BlueToolFixup/BrcmPatchRAM/BlueToolFixup.cpp
   ```
2. If they do, add a table entry in `kAdvSetsTables` with the new UUID and the Darwin version range.
3. If they don't, find the same code in the new binary (look for the log strings named in [research/ble-advsets-analysis.md](research/ble-advsets-analysis.md)), write new patterns with masks for relocated bytes, and add them as a new `kAdvSetsPatchesXX` table. `tests/unit/test_advsets_tables.py` checks that every table is consistent; update the `@seq` list in `verify-advsets.pl` if you change the 15.8.1 patterns.
4. Test with `-btlfxadvsets` and look for `[advsets] all 6 patches applied` in the log.

## 7. Before you open a pull request

- `tests/run.sh` passes, CI builds every target, and `tools/check-kext.sh` shows consistent struct sizes.
- The PR says which macOS build(s) you tested, on which card, which boot-args, and what worked (attach a redacted `scripts/monitor-awdl.sh` summary).
- No Apple binaries or raw dumps are in the diff.

## Sonoma and older

The AWDL code is only compiled into the "WCL" targets of `itlwm.xcodeproj` (`AIRPORT_WCL`: Sequoia 15.2+ and Tahoe). The Sonoma and older AirportItlwm targets talk to macOS through a different, older IO80211 interface, so none of the `awdl0` glue (`AirportItlwmAWDLInterface`, the `macOS15`/`macOS26` headers) applies there.

A Sonoma port could reuse the HAL part (`ic_awdl_*` callbacks, P2P listen, TX pump) and the timeline logic, but it needs new glue for the AWDL interface of that IO80211Family, verified with the same method as above (steps 1 to 5). Nobody has done it yet. If you want to try, open a discussion first so we can share notes.
