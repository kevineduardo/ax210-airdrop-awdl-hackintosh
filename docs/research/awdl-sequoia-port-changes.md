# AWDL Sequoia port: changes made

*Engineering notes written during the port (November 2026). Line numbers refer to the tree at the time of writing; the modified files later gained a two-line license notice at the top, so numbers may be off by two.*

Date: 2026-11-10.

- Working tree: `driver/AirportItlwm`, a copy of crazyhulk@ff8db43, with MacKernelSDK at 3f75008.
- The upstream reference checkouts were left untouched.
- Full diff: generated with `diff -ruN -x MacKernelSDK -x .DS_Store <crazyhulk/itlwm@ff8db43 checkout> driver/AirportItlwm`; 809 lines across 13 files.

**Status: nothing has been compiled.** `xcodebuild` is now present on the machine, but refuses to run because the license has not been accepted (`sudo xcodebuild -license`). The license was not accepted and no build was attempted. Everything that depends on the compiler is listed in section 4.

> Status update: the code was later built and tested. It builds in all four Sequoia/Tahoe × Debug/Release configurations, and AirDrop iPhone→Mac and Mac→iPhone worked on Sequoia 15.8.1.

All of AWDL stays off without `-itlawdl`. The staged boot-args are the same as on Tahoe: `-itlawdlaw`, `-itlawdllisten`, `-itlawdlp2p`, `-itlawdltx`, `-itlawdldata`, `-itlawdlpromisc`, `-itlawdlnopeerlisten` and `itlawdllistench=`. There are two new boot-args: `-itlawdlnoinject` (both targets) and `-itlawdlsetif` (Sequoia only).

---

## 1. Summary by file

| File | +/− | What changed |
|---|---|---|
| `AirportItlwm/AirportItlwmAWDLConfig.h` (new) | +26 | Defines `AIRPORT_AWDL` whenever `AIRPORT_WCL` is defined, i.e. on Sequoia and Tahoe. Also defines `AIRPORT_AWDL_ABI_SEQUOIA` (`__IO80211_TARGET == __MAC_15_2`) or `AIRPORT_AWDL_ABI_TAHOE`. Depends only on preprocessor macros |
| `AirportItlwm/AirportItlwmV2.hpp` | +3/−1 | Includes the config. Guard `:266` becomes `#if defined(AIRPORT_AWDL)`. New field `bool fAWDLSetIf` |
| `AirportItlwm/AirportItlwmV2.cpp` | +7/−6 | The 6 guards (`:145`, `:486`, `:502`, `:516`, `:613`, `:644`) become `AIRPORT_AWDL`. `init()` initializes `fAWDLSetIf = false` |
| `AirportItlwm/AirportItlwmSkywalkInterface.hpp` | +2/−1 | Includes the config. Guard `:305` (real `setVIRTUAL_IF_CREATE`) becomes `AIRPORT_AWDL` |
| `AirportItlwm/AirportItlwmAWDLInterface.hpp` | +32/−8 | See details after the table |
| `AirportItlwm/AirportItlwmAWDLInterface.cpp` | +124/−8 | See details after the table |
| `include/Airport/IO80211VirtualInterfaceWCL.h` (new) | +21 | Version shim: `macOS15/` or `macOS26/IO80211VirtualInterface.h` |
| `include/Airport/IO80211AWDLProtocol.h` (new) | +22 | Version shim: includes the VI shim, then `macOS15/` or `macOS26/IO80211AWDLProtocol.h` |
| `include/Airport/macOS15/IO80211VirtualInterface.h` (new) | +141 | See details after the table |
| `include/Airport/macOS15/IO80211AWDLProtocol.h` (new) | +131 | See details after the table |
| `include/Airport/macOS15/IO80211ControllerV2.h` | +4 | `void setAwdlInterface(IO80211VirtualInterface*)`, non-virtual, with an asm label. Adds no slot and does not change the layout |
| `include/Airport/apple80211_ioctl.h` | +8 | `apple80211_awdl_sync_params` gains `uint32_t non_election_broadcast_period` **only** under `AIRPORT_WCL && __MAC_15_2`, plus `static_assert(sizeof == 24)`. Tahoe and older targets stay at 20 bytes |
| `itlwm.xcodeproj/project.pbxproj` | +2 | New `PBXBuildFile 5EA015A1D0C4E2B7F9A30B11` (same fileRef `3C6104E34D78EB99C79D1C20`), added to the Sequoia Sources phase `AFFBDFD057C2402C908ECB89`. `plutil -lint` reports OK |

### Details: `AirportItlwmAWDLInterface.hpp`

- Includes the config before the guard, so it works when included on its own.
- The `macOS26/…` includes become the shims `<Airport/IO80211VirtualInterfaceWCL.h>` and `<Airport/IO80211AWDLProtocol.h>`.
- Under `AIRPORT_AWDL_ABI_SEQUOIA` there are `extern "C"` declarations by mangled name for `IO80211Buffer::returnBuffer()`, `setLength(unsigned long)`, `getCapacity() const` and `getBytesNoCopy()`.
- New counter `fRxAFNoInject`.

### Details: `AirportItlwmAWDLInterface.cpp`

- Guard becomes `AIRPORT_AWDL`.
- `awInjectAllowed()` reads `-itlawdlnoinject`. `awSetIfAllowed()` reads `-itlawdlsetif` and exists only on Sequoia.
- Helpers `awdlPoolGetBuffer` / `awdlBufReturn` / `awdlBufCapacity` / `awdlBufBytes` / `awdlBufSetLength`:
  - **Tahoe:** the same `vcall`s with the same offsets as before.
  - **Sequoia:** the pool is still called through `vcall` at +0x118 (verified); the buffer is called by symbol.
- `inputActionFrame`, with `-itlawdlnoinject`: still feeds `trackPeer`/`syncFromFrame`, but does not touch IO80211Family. Publishes `AWDLRxNoInject`.
- Extra check `getBytesNoCopy() == nullptr` → returns the buffer and counts it as a drop. This is the only behavior difference on Tahoe, and only in a case that previously panicked.
- `createAWDLInterface`: on Sequoia, with `-itlawdlsetif`, calls `setAwdlInterface(awdl)` after `start`.
- `destroyAWDLInterface`: calls `setAwdlInterface(nullptr)` if the former was called.

### Details: `macOS15/IO80211VirtualInterface.h`

- Base: Unopcoooo's reconstruction (UN `include/Airport/macOS15/IO80211VirtualInterface.h`).
- `setLinkState(IO80211LinkState, unsigned)` now returns **bool**. crazyhulk ignores the return value.
- Extra forward declarations, the same as in the Tahoe header.
- Header comment with provenance and the verification performed.

### Details: `macOS15/IO80211AWDLProtocol.h`

- Base: Unopcoooo's reconstruction.
- Forward declaration corrected to `apple80211_awdl_extension_state_machine_parameter`, singular.
- `static_assert(sizeof(IO80211AWDLProtocol) == 0x120)`.

### Guards: correction to the count

Grepping for `__IO80211_TARGET != __MAC_15_2` gives **14 lines**. Only **10 are AWDL guards**, and those were replaced. The other 4 are not AWDL-related and were left alone:

- `AirportItlwmSkywalkInterface.hpp:36/94/105`: InfraProtocol differences between 15 and 26.
- `WCL/WCL.hpp:132`.

[awdl-sequoia-port-analysis.md](awdl-sequoia-port-analysis.md) says "11"; the correct number is 10.

### Tahoe: no change in intent

- The `vcall` offsets and `AWDLRxMeta` are the same.
- The creation sequence is the same.
- The `macOS26/` headers were not touched; the shim includes exactly the same files.
- `apple80211_awdl_sync_params` stays at 20 bytes.
- What changes on Tahoe: the new field `fRxAFNoInject` (8 bytes at the end of the object), the optional `-itlawdlnoinject` boot-arg (off by default) and the null-pointer check described above.

---

## 2. Verification against the 24H32 KC (done without otool)

Source: `/System/Library/KernelCollections/BootKernelExtensions.kc` (66 125 824 bytes, 15.8.1 24H32).

The kernel collection was parsed with small ad-hoc scripts (a Python equivalent is `tools/kc/kcsyms.py`). They did the following:

- Read the fileset entries and each kext's `LC_SYMTAB` tables.
- Decode the vtables. Kernel cache pointers with chained fixups hold the target in the low 30 bits, relative to base `0xffffff8000100000`; the decoding was validated by the consistency of the resulting names, such as `D1`/`D0`/`OSObject::release`.
- Extract function bodies and search for x86 patterns.

### 2.1 Offsets and symbols verified

| Item | 24H32 result |
|---|---|
| `__ZTV23IO80211VirtualInterface`: ancestors | Exactly **53** `IO80211VirtualInterface::*` entries up to 0xe58, and they are the same 53 the header declares (compared with `comm`: 0 extra, 0 missing) |
| New VI slots | 0xe60 `init`, 0xe68 `setLinkState`, 0xe70 `forwardPacket`, 0xe78 `handleIoctl`, 0xe80 `addPeerToCache`, 0xe88 `deletePeerFromCache`, 0xe90 `handleDebugCmd`, 0xe98 `getPeerSlotDataStats`, 0xea0 `getPeerDataStats`, 0xea8 `clearPeerDataStats`, 0xeb0 `setAwdlCurrentChannelSequenceIndex`, 0xeb8 `isOutputFlowControlled`, 0xec0 `setOutputFlowControlled`, 0xec8 `clearOutputFlowControlled`, 0xed0 `configureAQMOutput`, 0xed8 `attachToBpf`, 0xee0 `configureIfnet`, 0xee8 `reset`, 0xef0 `dupAndTransmitMcastPacket`, 0xef8 `freeMulticastPacket`, 0xf00 `triggerAWDLMulticastTx`, 0xf08 `p2pMulticastTx(pkt**,u)`, 0xf10 `sendPendingPacketsToStack`, 0xf18 `freePendingPackets`, 0xf20–0xf98 `_RESERVED0..15`. **Identical to the header** |
| `IO80211SkywalkInterface`: last slots | 0xe50 `getLastTxTimeStamp`, 0xe58 `getLastRxTimeStamp`. Matches the existing Sequoia header |
| `__ZTV19IO80211AWDLProtocol` | **63** `___cxa_pure_virtual` from 0xfa0 to 0x1190; 0x1198 = 0 (end) |
| `__ZTV17IO80211BufferPool` | +0x118 `getBuffer()`, +0x120 `returnBuffer(IO80211Buffer*)`, +0x128 `flushPool`, +0x130 `resetPool`, +0x138 `printStats`. **`kPoolGetBuffer = 0x118` is valid on Sequoia** |
| `__ZTV13IO80211Buffer` | **Has no virtual methods of its own.** The vtable ends at +0x110 (`OSObject::_RESERVED15`) and +0x118 is 0. Tahoe's `kBufReturn` 0x118, `kBufSetLength` 0x138, `kBufGetCapacity` 0x140 and `kBufGetBytes` 0x158 **would panic on Sequoia**. That is why on Sequoia these calls go by symbol |
| Export: buffers | `__ZN13IO80211Buffer12returnBufferEv`, `…9setLengthEm`, `…14getBytesNoCopyEv`, `__ZNK13IO80211Buffer11getCapacityEv` and `__ZN17IO80211BufferPool9getBufferEv` are `N_SECT\|N_EXT` (0x0f) in IO80211Family. AppleBCMWLANCoreMac **imports by name** `returnBuffer`, `setLength`, `getBytesNoCopy()`, `bpfTapInput(IO80211Buffer*…)`, `getMgmtFramePool`, `VI::init` and `VI::setLinkState` |
| Export: AWDLProtocol | `IO80211AWDLProtocol::gMetaClass`, `C2` and `D2` are also imported by Broadcom, so `OSDefineMetaClassAndStructors` links |
| `__asm__` labels | All 107 IO80211 labels in the new or modified headers exist as exports in IO80211Family |
| RX meta for `bpfTapInput` | In `AppleBCMWLANProximityInterface::handleActionFrame_rx`, +0x679 has `mov r8d,0x48`, +0x685 `mov edx,0xc` and +0x68a a `call` to `0xffffff80022a50ec`, which is `IO80211VirtualInterface::bpfTapInput(IO80211Buffer*,…)`. The buffer is at `rbp-0x78`. The channel is written via `lea rsi,[rbp-0x6c]` (+0x0c). `rate`/`rssi`/`tsf` are written with `bswap` at +0x28, +0x2c and +0x30. **Size, DLT and fields +0x0c/+0x28/+0x2c/+0x30 match Tahoe** |
| OOB request (Broadcom `setAWDL_OOB_REQUEST` / `_AUTO_REQUEST`) | Reads a `u16` at +0x28 (40) and a 64-bit pointer at +0x30 (48). The auto version also reads +0x0c, +0x10 and a `u16` at +0x14. Matches CH's `recordOOB` |
| `setAWDL_SYNC_FRAME_TEMPLATE` | Reads +0x04 (len) and +0x08 (64-bit pointer). Matches |
| `setAWDL_SYNC_PARAMS` | Reads +0x04, +0x08, +0x0c, +0x10 and **+0x14**. Hence the 24-byte size |
| `IO80211Controller::setAwdlInterface` | 20-byte body: `mov rax,[rdi+0x120]; mov [rax+0x188],rsi; ret`. A trivial setter, and `nullptr` is safe. **Broadcom does not import it** |
| Already confirmed earlier (UN and the port analysis) | AW events 41/43 with 0x24/0x28 bytes; `sync_state` +8/+16/+20; `channel_sequence` +5/+7/+12×12 in 400 bytes; role 6; capability index 0x20 |

### 2.2 Still to verify (needs Xcode: `otool`/`llvm-objdump`/`nm`, or `ipsw`)

1. **Transmitter address at meta+0x00.** Only the zeroing (`mov [rbp-0x78],edx`) was seen. `handleActionFrame_rx` still needs a full disassembly, for example with `llvm-objdump -d` on the kext extracted by `ipsw kernel extract`.
2. **Return width of `IO80211Buffer::getCapacity()`.** The code only uses the low 32 bits, which is safe either way.
3. **Vtable of our compiled `AirportItlwmAWDLInterface`.** Confirm with `nm`/`otool` that `__ZTV25AirportItlwmAWDLInterface` has 16 + 0x1198 bytes of inherited slots and that slot 0xe68 points to `IO80211VirtualInterface::setLinkState`.
4. **Undefined symbols in the binary.** `nm -u` on the Sequoia binary: all of them must exist in the export list of IO80211Family, IOSkywalkFamily or the kernel.
5. **Slot of `setVIRTUAL_IF_CREATE` in the Sequoia InfraProtocol.** The header was already used by laobamac's stub, but the slot was not verified against the KC.
6. **Behavior, not ABI.** Whether `IO80211VirtualInterface::start` creates the `IO80211AWDLPeerManager` without going through `attachVirtualInterface`. Broadcom imports `attachVirtualInterface`, and CH does not use it.
7. **Tahoe.** Nothing was verified against a Tahoe KC; crazyhulk's values are kept.

---

## 3. Build commands

Prerequisites: Xcode 16.4 with the license accepted. Run once:

```sh
sudo xcode-select -s /Applications/Xcode.app
sudo xcodebuild -license accept
```

Build both targets:

```sh
cd driver/AirportItlwm

# Sequoia (main target)
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Sequoia -configuration Debug \
  ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build
# Output: build/Debug/Sequoia/AirportItlwm.kext (CONFIGURATION_BUILD_DIR = $(SYMROOT)/$(CONFIGURATION)/Sequoia)

# Tahoe (regression: must still build)
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe -configuration Debug \
  ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build
```

For Release, use `-configuration Release`.

Checks after building:

```sh
K=build/Debug/Sequoia/AirportItlwm.kext/Contents/MacOS/AirportItlwm
nm -u $K | grep -E 'IO80211(Buffer|VirtualInterface|AWDLProtocol|Controller16setAwdl)'
nm -m $K | grep __ZTV25AirportItlwmAWDLInterface
otool -l $K | grep -A3 LC_SEGMENT_64
```

---

## 4. What could not be verified without compiling

- Whether the `macOS15/IO80211VirtualInterface.h` header (Unopcoooo) compiles as-is inside this tree. laobamac's Sequoia Skywalk header differs from Unopcoooo's only in `getSelfMacAddr` (`ether_addr*` here, `uint64_t` there), and the VI does not override it. Still, it has not been compiled.
- `-Woverloaded-virtual` or name-hiding warnings: `init`/`setLinkState` with signatures different from the base. This is the same pattern that already compiles on Tahoe.
- `apple80211_ioctl.h`: the new `static_assert` is only active under `AIRPORT_WCL` + 15.2. No `.c` files include these headers (checked with grep).
- The new `AirportItlwmAWDLConfig.h` was not added to the Xcode group. It is not required, because it is included by relative path.

> Status update: the code was later built in all four Sequoia/Tahoe × Debug/Release configurations and tested; AirDrop iPhone→Mac and Mac→iPhone worked on Sequoia 15.8.1.

## 5. Suggested test sequence (Sequoia, with Wi-Fi associated)

1. `-itlawdl -itlawdlnoinject`: awdl0 is created, the ioctls show up in the log and the STA stays stable.
2. `-itlawdlaw`: AW timeline and 41/43 events.
3. `-itlawdllisten -itlawdlp2p`: watch `AWDLRxNoInject` increase.
4. Remove `-itlawdlnoinject`: injection into the peer manager and discovery from a Mac or iPhone.
5. `-itlawdltx`, then `-itlawdldata -itlawdlpromisc`.
6. If the peer manager does not react to the ioctls, repeat with `-itlawdlsetif`.

Always use `keepsyms=1 debug=0x100` and have a rollback plan at hand.
