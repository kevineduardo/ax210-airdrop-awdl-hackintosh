# Porting crazyhulk's AWDL (AirDrop) from Tahoe to Sequoia: analysis

*Engineering notes written during the port (November 2026). Line numbers refer to the tree at the time of writing; the modified files later gained a two-line license notice at the top, so numbers may be off by two.*

Date: 2026-11-10. Read-only analysis: nothing was compiled and none of the reference checkouts were modified.

> Status update: the port described here was later implemented and built in all four Sequoia/Tahoe × Debug/Release configurations. AirDrop iPhone→Mac and Mac→iPhone worked on Sequoia 15.8.1. See [awdl-sequoia-port-changes.md](awdl-sequoia-port-changes.md).

Sources:

| Abbreviation | Location | Origin |
|---|---|---|
| **CH** | a local checkout of crazyhulk/itlwm@ff8db43 | crazyhulk/itlwm @ ff8db43 |
| **LB** | a local checkout of laobamac/itlwm@7204c69 | laobamac/itlwm main @ 7204c69 (parent of ff8db43) |
| **UN** | a local checkout of Unopcoooo/itlwm, branch `awdl/sequoia-ac9560-preview` | Unopcoooo/itlwm, branch `awdl/sequoia-ac9560-preview` |
| **KC** | `/System/Library/KernelCollections/BootKernelExtensions.kc` | the test machine (an AMD Ryzen 5600G desktop, macOS 15.8.1 (24H32)), Darwin 24.6.0 |

The diff was generated with `diff -ruN itlwm-laobamac itlwm-crazyhulk`. It is 4546 lines long, with **4158 lines added and 10 removed**, spread across 17 files.

Important: the KC on the test machine is exactly the same build (15.8.1 / 24H32) that Unopcoooo generated the Sequoia headers from (UN `docs/awdl-status.md:38`). The symbol checks below therefore apply to the real target.

---

## 1. Breaking down the diff

### (a) Parts that do not depend on the macOS version

| File | +/− | Content |
|---|---|---|
| `itlwm/hal_iwx/ItlIwx.cpp` | +936 / −7 | AWDL listening in three ways: P2P device (`iwx_p2p_*`), auxiliary ROC (`HOT_SPOT_CMD`) and scan v14. Also AWDL TX (`iwx_awdl_tx_frame`), RX counters, and ROC and session notifications |
| `itlwm/hal_iwx/ItlIwx.hpp` | +16 / −1 | Prototypes; `iwx_tvqm_enable_txq(..., int sta_id = IWX_STATION_ID)` |
| `itlwm/hal_iwx/if_iwxreg.h` | +27 | `IWX_HOT_SPOT_CMD 0x53`, `iwx_hs20_roc_req/res`, `IWX_AX210_GP2_REG 0xd02c68` |
| `itlwm/hal_iwx/if_iwxvar.h` | +38 | `sc_awdl_*`, `sc_aux_roc_*` and `sc_p2p_*` state |
| `itl80211/openbsd/net80211/ieee80211_input.c` | +46 | Hook under `#ifdef AIRPORT_WCL` (CH lines 318–357): diverts vendor-127 action frames and data to awdl0, and filters management frames from foreign BSSs |
| `itl80211/openbsd/net80211/ieee80211_var.h` | +108 | Fields and callbacks `ic_vendor_action_handler`, `ic_awdl_listen`, `ic_awdl_tx`, `ic_awdl_data_handler`, `ic_awdl_addr*`, plus counters |
| **Subtotal hal_iwx + net80211** | **+1171 / −8** | |
| `AirportItlwm/AirportItlwmEthernetInterface.{cpp,hpp}` | +10 / −1 | Virtual method `getInterfaceAddress()` (awdl0 publishes its own MAC) |
| `AirportItlwm/AirportItlwmSkywalkInterface.cpp` | +30 | `getHT_CAPABILITY` (CH:1014), used by `IO80211PeerManager::getHtCapabilityIE`. **The symbol exists in the Sequoia KC**, so it works the same way |
| `AirportItlwm/AirportItlwmV2.cpp` (capability bits) | ~12 of +76 | `capabilities[4] |= 1` and `capabilities[3] |= 0x20`, under `#ifdef AIRPORT_WCL` (CH:1060–1071). UN confirms that wifip2pd 24H32 tests index 0x20, i.e. byte 4, bit 0 (UN `AirportItlwmV2.cpp:3615-3623`) |
| `itlwm.xcodeproj/project.pbxproj` | +6 | File added **only** to Sources phase `F8D94CA52B9ABFE20081A3C4`, shared by Sonoma14.4 and Tahoe |

### (b) Parts that depend on the Tahoe ABI

| File | +/− | Nature |
|---|---|---|
| `include/Airport/macOS26/IO80211AWDLProtocol.h` | +132 | 63 pure slots starting at 0x0fc8; `static_assert(sizeof==0x130)` |
| `include/Airport/macOS26/IO80211VirtualInterface.h` | +143 | Tahoe layout (0x130), pure `setMacAddress` at 0x0f40 |
| `AirportItlwm/AirportItlwmAWDLInterface.hpp` | +324 | Includes `macOS26/*` directly (lines 16–17); `extern "C"` `getMgmtFramePool`/`bpfTapInput` (lines 28–32) |
| `AirportItlwm/AirportItlwmAWDLInterface.cpp` | +2248 | See the note below |
| `AirportItlwm/AirportItlwmV2.{cpp,hpp}` (guarded parts) | ~64 + 13 | Fields `fAWDLIf`/`fAWDLBsdIf`, handlers, `createInterface()` and `configureInterface()` for awdl0, `destroyAWDLInterface()` |
| `AirportItlwm/AirportItlwmSkywalkInterface.hpp` | +5 / −1 | Real `setVIRTUAL_IF_CREATE` only on Tahoe (lines 304–308) |
| **Subtotal (b)** | **≈ +2930** | |

**Note on `AirportItlwmAWDLInterface.cpp`:** of its 2248 lines, only about 60 actually touch IO80211Family ABI. The rest is portable protocol logic: the AW timeline, peer-aligned listening, PSF/MI construction, the Ethernet↔802.11 datapath and the diagnostic properties. The ABI-dependent points are:

1. `enum` with vtable offsets for `IO80211BufferPool`/`IO80211Buffer`: `0x118/0x138/0x140/0x158` (CH `AirportItlwmAWDLInterface.cpp:224-231`).
2. `AWDLRxMeta`, 0x48 bytes passed to `bpfTapInput` (`:239-252`).
3. AW message IDs `41/42/43` and structures of 0x24 and 0x28 bytes (`:381-401`).
4. Payload layouts copied by raw offset:
   - `sync_state`: +8, +16 and +20 (`:1935+`);
   - `sync_frame_template`: +4 and +8 (`:2054+`);
   - OOB request: +12, +16, +40 and +48 (`:2083+`);
   - `channel_sequence`: +5, +7 and +12, with a stride of 12 and 400 bytes total (`:367+`).
5. Skywalk creation and registration sequence (`:1529-1596`) and `setLinkState(..., 1)` (`:1605`).

---

## 2. The guard that excludes Sequoia, and the target definition

### Guard

The condition is always `#if defined(AIRPORT_WCL) && __IO80211_TARGET != __MAC_15_2`. It appears in 11 places in the AWDL code:

| File:line | Effect on Sequoia |
|---|---|
| `AirportItlwm/AirportItlwmAWDLInterface.hpp:13` | Whole header empty |
| `AirportItlwm/AirportItlwmAWDLInterface.cpp:12` (closes at `:2248`) | Whole `.cpp` empty |
| `AirportItlwm/AirportItlwmV2.hpp:265` | No `fAWDLIf`, `fAWDLBsdIf`, `fCreatingAWDL`, `fAWDLMac`, `createAWDLInterface`, `setAWDLLinkGated`, `destroyAWDLInterface` |
| `AirportItlwm/AirportItlwmV2.cpp:145` | `init()` |
| `AirportItlwm/AirportItlwmV2.cpp:486` | `awdlActionFrameHandler` becomes `return 0` |
| `AirportItlwm/AirportItlwmV2.cpp:502` | `awdlDataFrameHandler` becomes `return 0` |
| `AirportItlwm/AirportItlwmV2.cpp:516` | `stop()`: no `destroyAWDLInterface()` |
| `AirportItlwm/AirportItlwmV2.cpp:613` | `configureInterface()`: no awdl0 path |
| `AirportItlwm/AirportItlwmV2.cpp:644` | `createInterface()`: no awdl0 path |
| `AirportItlwm/AirportItlwmSkywalkInterface.hpp:304` | `setVIRTUAL_IF_CREATE` falls back to the `kIOReturnUnsupported` stub |

Note: the `__IO80211_TARGET != __MAC_15_2` guards in `AirportItlwmSkywalkInterface.hpp:35/93/104` **are not AWDL guards**. They cover InfraProtocol differences between 15 and 26 and must not be touched.

**Current side effect on Sequoia:** with `-itlawdl`, the code under `#ifdef AIRPORT_WCL` (CH `AirportItlwmV2.cpp:340-350` and `:1060-1071`) installs the handlers and advertises the AWDL capability. However, `VIRTUAL_IF_CREATE` answers *Unsupported*, and the firmware filter is left open (`IWX_MAC_FILTER_IN_CONTROL_AND_MGMT`, `ItlIwx.cpp:8932`) with nothing consuming the frames.

### How the `AirportItlwm-Sequoia` target is defined (CH `itlwm.xcodeproj/project.pbxproj`)

- Target `C2631DEBF2864F9B8272D88C` (pbxproj:2448–2466). Its own Sources phase: `AFFBDFD057C2402C908ECB89` (pbxproj:3359). It **does not contain** `AirportItlwmAWDLInterface.cpp`.
- Configurations Debug `2B4AD1BBF0014C848E6A7F18` (pbxproj:~4229) and Release `E71DD1C8720A4A5E8D09A1CD` (pbxproj:~4297):
  - `GCC_PREPROCESSOR_DEFINITIONS = AIRPORT_WCL, AIRPORT_VTD, AIRPORT, "__IO80211_TARGET=__MAC_15_2", __PRIVATE_SPI__, IO80211FAMILY_V2`
  - `INFOPLIST_FILE = AirportItlwm/WCL/Sequoia-Info.plist`, `MACOSX_DEPLOYMENT_TARGET = 15.2`, `CONFIGURATION_BUILD_DIR = $(SYMROOT)/$(CONFIGURATION)/Sequoia`
  - `SYSTEM_HEADER_SEARCH_PATHS = "itl80211/openbsd itl80211 include"`, `GCC_PREFIX_HEADER = itlwm/PrivateSPI.pch`
- Tahoe (`7D5753727745775F75788913` and `D92AB006FB4B2B6A6032F9C7`) is the same, except for `__IO80211_TARGET=__MAC_26_0`, `WCL/Info.plist` and deployment target 10.15.
- **Header directory selection is not in the pbxproj.** It is done by wrappers in `include/Airport/*.h`, all of the form `#if __IO80211_TARGET == __MAC_15_2` → `macOS15/…`, `#else` → `macOS26/…`. Example: `include/Airport/IO80211SkywalkInterface.h:10-14`. There is **no wrapper yet** for `IO80211VirtualInterface.h` and `IO80211AWDLProtocol.h`, because CH includes `macOS26/` directly.
- The schemes exist in `itlwm.xcodeproj/xcshareddata/xcschemes/AirportItlwm-Sequoia.xcscheme` and `AirportItlwm-Tahoe.xcscheme`.

---

## 3. Vtable mapping

### 3.1 Checking the claimed counts ("Tahoe ~95 vs Sequoia ~81")

The claim is **inverted and miscounted**. The real counts are:

| | Tahoe (CH `macOS26/IO80211VirtualInterface.h`) | Sequoia (UN `macOS15/IO80211VirtualInterface.h`) |
|---|---|---|
| Lines containing `virtual` (raw grep) | **81** | **95** (93 declarations, plus "Nonvirtual" on line 118 and `_virtualExpansion` on line 121) |
| Ancestor overrides | 53 (lines 38–90, including 5 `_abi_*`) | 53 (lines 24–76; same names, `enable` appears at the end) |
| New non-reserved slots | 28 (27 + pure `setMacAddress`) | 24 |
| Reserved (`_RESERVED…0..15`) | 16 (macro `OSMetaClassDeclareReservedUnused`, not caught by the grep) | 16 (declared explicitly) |
| **Total new slots** | **44** (0x0e68–0x0fc0) | **40** (0x0e60–0x0f98) |
| **Total declared** | **97** | **93** |
| `sizeof` | 0x130 (`IO80211SkywalkInterface` = 0x128) | 0x120 (`IO80211SkywalkInterface` = 0x118) |
| Start of `IO80211AWDLProtocol` | 0x0fc8 | 0x0fa0 (constant delta of −0x28 = 5 slots) |

The 5-slot difference breaks down as follows. There is 1 slot fewer at the end of Sequoia's `IO80211SkywalkInterface` (new slots start at 0xe60 instead of 0xe68). There are 4 slots fewer in `IO80211VirtualInterface`, listed in the next table.

### 3.2 New IO80211VirtualInterface slots

Legend for the CH column: **O** = crazyhulk overrides it; **C** = crazyhulk calls it.

| # | Method | Tahoe | Sequoia | Signature / notes | CH |
|---|---|---|---|---|---|
| 1 | `init(IO80211Controller*, ether_addr*, unsigned, const char*)` | 0xe68 | 0xe60 | Same; symbol `__ZN23IO80211VirtualInterface4initEP17IO80211ControllerP10ether_addrjPKc` **present in the KC** | **O + C** (`IO80211AWDLProtocol::init`, CH:1694) |
| 2 | `setLinkState(IO80211LinkState, unsigned)` | 0xe70 | 0xe68 | Tahoe declares `IOReturn`; UN declares `void`. According to UN, the real one in 24H32 returns **bool** (UN `AirportItlwmV2.cpp:1083-1086`). CH ignores the return value, so this is not a problem | **C** (virtual, CH:1605) |
| 3 | `forwardPacket(IO80211NetworkPacket*)` | 0xe78 | 0xe70 | Return IOReturn (T) / void (S, not verified) | – |
| 4 | `handleIoctl(__ifnet*, unsigned long, void*)` | 0xe80 | 0xe78 | same | – |
| 5 | `addPeerToCache(unsigned char*)` | 0xe88 | 0xe80 | same | – |
| 6 | `deletePeerFromCache(unsigned char*)` | 0xe90 | 0xe88 | same | – |
| 7 | `handleDebugCmd(apple80211_debug_command*)` | 0xe98 | 0xe90 | same | – |
| 8 | `getPeerSlotDataStats(ether_addr*, bool, peerSlotDataStats*)` | 0xea0 | 0xe98 | same | – |
| 9 | `getPeerDataStats(ether_addr*, bool, packetCounters_t*)` | 0xea8 | 0xea0 | same | – |
| 10 | `clearPeerDataStats(ether_addr*, bool)` | 0xeb0 | 0xea8 | same | – |
| 11 | `setAwdlCurrentChannelSequenceIndex(unsigned)` | 0xeb8 | 0xeb0 | same | – |
| 12 | `isOutputFlowControlled()` | 0xec0 | 0xeb8 | bool (T) / void (S) | – |
| 13 | `setOutputFlowControlled()` | 0xec8 | 0xec0 | | – |
| 14 | `clearOutputFlowControlled()` | 0xed0 | 0xec8 | | – |
| 15 | `flushFlowQueues(ether_addr*)` | 0xed8 | **does not exist** | Symbol absent from the 24H32 KC (verified) | – |
| 16 | `configureAQMOutput()` | 0xee0 | 0xed0 | | – |
| 17 | `attachToBpf()` | 0xee8 | 0xed8 | | – |
| 18 | `configureIfnet()` | 0xef0 | 0xee0 | | – |
| 19 | `reset()` | 0xef8 | 0xee8 | | – |
| 20 | `dupAndTransmitMcastPacket(pkt*, u8*, u8*)` | 0xf00 | 0xef0 | | – |
| 21 | `freeMulticastPacket(pkt*)` | 0xf08 | 0xef8 | | – |
| 22 | `triggerAWDLMulticastTx(unsigned, u8)` | 0xf10 | 0xf00 | | – |
| 23 | `p2pMulticastTx(IO80211NetworkPacket**, unsigned)` | 0xf18 | 0xf08 | | – |
| 24 | `p2pMulticastTx(unsigned, bool)` | 0xf20 | **does not exist** | Symbol absent from the KC | – |
| 25 | `signalMulticastPackets(unsigned, unsigned)` | 0xf28 | **does not exist** | Symbol absent from the KC | – |
| 26 | `sendPendingPacketsToStack(pkt**, unsigned)` | 0xf30 | 0xf10 | | – |
| 27 | `freePendingPackets(pkt**, unsigned)` | 0xf38 | 0xf18 | | – |
| 28 | `setMacAddress(ether_addr&) = 0` | 0xf40 (in VI) | **moved** to `IO80211SkywalkInterface` (pure; CH `macOS15/IO80211SkywalkInterface.h:153`) | CH's `override` (`AirportItlwmAWDLInterface.hpp:84`) compiles on both; the slot differs but the compiler resolves it | **O** |
| 29–44 | `_RESERVEDIO80211VirtualInterface0..15` | 0xf48–0xfc0 | 0xf20–0xf98 | | – |

Non-virtual functions and helpers that CH uses by symbol, all **present in the 24H32 KC** (verified with grep):

- `__ZN23IO80211VirtualInterface16getMgmtFramePoolEv`
- `__ZN23IO80211VirtualInterface11bpfTapInputEP13IO80211BufferjPvm`
- `__ZN23IO80211VirtualInterface11postMessageEjPvmb`

### 3.3 Ancestor overrides (53 on both; names match one to one)

Offsets differ from `setInterfaceEnable` onwards. This comes from the `IO80211SkywalkInterface` base and **changes nothing for the port**: the compiler resolves offsets using each target's base header, which is already validated for the laobamac STA on Sequoia.

Differences in the offset comments (Tahoe → Sequoia):

- `setInterfaceEnable` 0x930→0x928
- `getFeatureFlags` 0x9b8→0x9b0
- `bpfTap` 0x9e0→0x9d8
- `setMulticastAddresses` 0xa88→0xa80
- `createPeerManager` 0xb08→0xb00
- **`postMessage` 0xb18→0xb10 (C)**
- `logTx*`/`inputPacket`/`pending`/`packetSpace` −8
- `getWmeTxCounters` 0xc38→0xc30, `setWoWEnabled`/`wowEnabled`/`printDataPath` −8
- `_abi_0c68..0c80`, `flushPacketQueues`, `cachePeer`, `shouldLog`, `vlogDebug*`, `getP2PSkywalkPeerMgr`, `findPeer` −8

Same on both: `free`, `configureReport`, `updateReport`, `start`, `stop`, `terminate`, `attach`, `detach`, `newUserClient`, `stringFromReturn`, `enable`, `prepareBSDInterface`, `processBSDCommand`, `linkState`, `setScanningState`, `updateLinkParameters`, `updateInterfaceCoexRiskPct`, `setInterfaceNF`, `setDebugFlags`, `debugFlags`, `setInterfaceChipCounters`, `setInterfaceMIBdot11`, `setFrameStats`, `setInfraSpecificFrameStats`, `getEventPipeSize`, `createEventPipe`.

CH uses these: **O** `free` (CH:1804); **C** `attach`/`start`/`stop`/`detach` (CH:1544-1626) and `postMessage` (CH:515/598/630). It also uses IORegistryEntry's `setProperties` (O, CH:735/770), as well as `initRegistrationInfo`, `mExpansionData`/`mExpansionData2` and `deferBSDAttach(true)`. These last ones exist in both `IOSkywalk*` headers (`macOS15/IOSkywalkEthernetInterface.h:60-67`, `macOS15/IOSkywalkNetworkInterface.h:74-94`) and are the same ones the LB STA already uses on Sequoia (CH `AirportItlwmV2.cpp:434-470`).

### 3.4 IO80211AWDLProtocol

The **63 methods are identical in name, signature and order**. I checked with a `diff` of the `= 0;` lines between CH `macOS26/IO80211AWDLProtocol.h:64-126` and UN `macOS15/IO80211AWDLProtocol.h:60-122`: they are equal. The only difference is the base offset, 0x0fc8+8·i on Tahoe and 0x0fa0+8·i on Sequoia. In both cases `sizeof` adds no ivars (Tahoe 0x130; Sequoia 0x120, per the `static_assert` in UN `ItlAwdlInterface.hpp:185`).

CH overrides all 63, since they are pure, and actually implements about 25. The rest are `AWDL_SET_STUB`/`AWDL_GET_STUB` stubs. CH's `.hpp` has 72 `override`s: 63 AWDL, plus `init`/`free`/`setMacAddress`/`setProperties`, plus 5 from `AirportItlwmAWDLEthernetInterface`.

The symbol `__ZN19IO80211AWDLProtocol10gMetaClassE` is **present in the 24H32 KC** (verified).

Small defects in the UN header:

- The forward declaration `apple80211_awdl_extension_state_machine_parameters`, in the plural (line 16), does not match the type actually used, which is singular. It causes no error because the complete type comes from `apple80211_ioctl.h`.
- The header itself lacks a `static_assert(sizeof(IO80211AWDLProtocol)==0x120)`.

### 3.5 Contract with IO80211AWDLPeerManager (the "non-vtable" part)

| Element used by CH | Tahoe (CH) | Sequoia 24H32 | Status |
|---|---|---|---|
| AW messages, `postMessage` 41/43 | 0x24 / 0x28 bytes | UN `itlwm/awdl/EngineState.hpp:14-15` and `AirportItlwmV2.cpp:510-523`: 36 and 40 bytes | **Same (confirmed)** |
| `apple80211_awdl_sync_state` +4/+8/+16/+20 | CH:1935+ | UN `apple80211_ioctl.h:1047-1076` (24H32 static_asserts) | **Same** |
| `apple80211_awdl_sync_params` | The shared header has **20 bytes** (CH `include/Airport/apple80211_ioctl.h:1044-1050`) | 24 bytes, with +20 `non_election_broadcast_period` (UN static_asserts) | Offsets +4..+16 are the same. CH does `memcpy(sizeof)` with 20 bytes, so the getter does not fill +20. Minor, but the field should be added on the Sequoia branch |
| `sync_frame_template` +4 len, +8 ptr | CH:2054+ | UN `apple80211_ioctl.h:927-932` | **Same** |
| `channel_sequence` +5 count, +6 encoding, +7 steps, entries at +12 in steps of 12, 400 bytes | CH:365-370 | UN `itlwm/awdl/TemplateRuntime.hpp:73-79` | **Same** |
| OOB request (+12, +16, +40 len, +48 ptr) | CH:2083+ | Not verified by UN | **To verify** |
| `AWDLRxMeta` 0x48 for `bpfTapInput` | CH:239-252 | UN does not use `bpfTapInput` | **To verify** (disassemble `AppleBCMWLANProximityInterface::handleActionFrame_rx`, which exists in the KC) |
| Slots of `IO80211BufferPool::getBuffer` / `IO80211Buffer::{returnBuffer,setLength,getCapacity,getBytesNoCopy}` | Fixed offsets 0x118…0x158 | Symbols **present** in the 24H32 KC: `__ZN17IO80211BufferPool9getBufferEv`, `__ZN13IO80211Buffer12returnBufferEv`, `__ZN13IO80211Buffer9setLengthEm`, `__ZNK13IO80211Buffer11getCapacityEv`, `__ZN13IO80211Buffer14getBytesNoCopyEv` | **Offsets not verified**. Replace with calls by symbol |
| AWDL role = 6 | CH hpp:36 | UN `AirportItlwmV2.cpp:67` `static_assert(APPLE80211_VIF_AWDL == 6)` | **Same** |
| `apple80211_capability_data` | 21 bytes | 19 bytes (`include/Airport/apple80211_ioctl.h:437-447`) | Bytes 3 and 4 are within bounds; OK |

Peer manager functions that CH cites as the basis of its reverse engineering, all **present in 24H32**:

- `IO80211AWDLPeerManager::{actionFrameHasCorrectOUI, configureValidAWDLChannels, reportDataPathEvents, setAwdlChannelSequence, monitorTimeline, buildEnhDataRateTlv, getHtCapabilityIE}`
- `IO80211AWDLPeer::{calculatePeerTimeDiff, shouldRemovePeerFromNDP}`
- `AppleBCMWLANCore::createProxInterface(apple80211_virt_if_create_data*)`
- `AppleBCMWLANProximityInterface::{handleActionFrame_rx, handleAvailabilityWindowStart, buildAWDLChanSequence, bringupLink}`

---

## 4. How Unopcoooo creates AWDL on Sequoia, and what that implies

### Where the AWDL stack lives on Sequoia

The stack is in **IO80211Family**, just as on Tahoe. The 24H32 KC contains:

- `IO80211AWDLProtocol` (metaclass);
- `IO80211AWDLPeerManager` and `IO80211AWDLPeer`, with all the methods above;
- `IO80211Controller::{createVirtualInterface, attachVirtualInterface, setAwdlInterface}`;
- on the Broadcom driver side, `AppleBCMWLANCore::createProxInterface` and `AppleBCMWLANProximityInterface`.

UN's own code checks at runtime that the peer manager exists and that its class is `"IO80211AWDLPeerManager"` (UN `AirportItlwmV2.cpp:856-872, 918-920`).

**Conclusion: the Tahoe architecture (the driver provides a subclass of `IO80211AWDLProtocol`; IO80211Family owns the peer manager, election and NDP) applies to Sequoia without fundamental changes.** The differences are in ABI (section 3), not in design.

### What UN did

1. **Creation** (UN `AirportItlwmSkywalkInterface.cpp:380-467`): `setVIRTUAL_IF_CREATE` filters for role 6, picks the MAC and calls `IO80211Controller::attachVirtualInterface(&inf, &mac, 6, false)`. This calls the factory `AirportItlwm::createVirtualInterface` (UN `AirportItlwmV2.cpp:2354-2385`), which does `new ItlAwdlInterface` and `IO80211VirtualInterface::init(this, mac, 6, "awdl")`. Then come `IO80211VirtualInterface::start(instance)` and `probeRegisterResources(...)`, which creates a native `IOSkywalkPacketBufferPool` and native Tx/Rx queues and calls `registerEthernetInterface` (UN `AirportItlwmV2.cpp:1338-1400`). Finally `setAwdlInterface(awdl)`, which **CH does not do**, and `deferBSDAttach(false)`.
2. **BSD:** it lets the native `IOSkywalkLegacyEthernet` create the `IOSkywalkLegacyEthernetInterface` awdl0. UN found that in 24H32 this class **does not call `ifnet_attach`**, and had to force `IONetworkInterface::attachToDataLinkLayer` by symbol, then hook in through an `iflt` filter (UN `AirportItlwmV2.cpp:45-66, 762-830, 2154-2200`).
3. **Protocol:** it has its own AWDL engine inside the driver, in `itlwm/awdl/` (124 headers, about 11.8k lines), with a peer table, election, sync and ROC. RX is observed in `hal_iwm` (`itlwm/hal_iwm/mac80211.cpp:970+`). **It does not inject action frames into `IO80211AWDLPeerManager` via `bpfTapInput`.** It only publishes AW events (`postMessage` 41/43) and registers peers with the kernel through `ifnet_notice_node_presence_v2` (UN `AirportItlwmV2.cpp:~1060-1080`).
4. **Stated limitations:**
   - exclusive radio, i.e. no normal Wi-Fi during the session;
   - receive only, Mac → Hackintosh;
   - session opened by local tools that are not published;
   - AC9560 only, which uses the `iwm` driver, not `iwx`.

   See UN `docs/awdl-status.md:22-33, 73-82` and `README.md:9-20`.

### Comparison with CH's design

| | CH (Tahoe) | UN (Sequoia) |
|---|---|---|
| Instantiation | Direct: `withController` → `attach` → `initRegistrationInfo` → `start` | `attachVirtualInterface` + `createVirtualInterface` factory |
| `setAwdlInterface` | No | Yes |
| awdl0 BSD | Own `IOEthernetInterface` (`AirportItlwmAWDLEthernetInterface`), same as the LB STA | Native `IOSkywalkLegacyEthernet` bridge (problematic) |
| Datapath | mbuf → `inputAWDLData` / `outputAWDL` | Native Skywalk queues |
| Peer manager | Fed with AFs via `getMgmtFramePool` + `bpfTapInput` | Bypassed (own engine) |
| Radio | Concurrent with the STA (P2P device / ROC alongside the association) | Exclusive |
| Hardware | AX210 / `hal_iwx` | AC9560 / `hal_iwm` |

**Assessment:** the Tahoe design **can** be mapped to Sequoia. UN took a different path because of its own choices (iwm, exclusive radio, probe-based approach), not because Sequoia required it. UN's evidence actually supports CH's path: the peer manager contract (AW events, layouts) is the same, and the `IOSkywalkLegacyEthernet` bridge UN used is broken in 24H32, which CH avoids by using its own `IOEthernetInterface`.

**From UN's work, only these are reused:** the two `macOS15/` headers, the `setAwdlInterface` declaration, the 24H32 layout checks and the capability bit confirmation. The `itlwm/awdl` engine and `hal_iwm` are **not usable** for the AX210.

---

## 5. What in the hal_iwx changes assumes Tahoe

Nothing in `hal_iwx`/`net80211` depends on macOS. There is no reference to `__IO80211_TARGET`, and the only `#ifdef` is `AIRPORT_WCL`, which Sequoia defines (`ItlIwx.cpp:9173`; `ieee80211_input.c:318`). There are, however, assumptions worth knowing:

1. **Requires an active association.** `iwx_awdl_listen` returns `EBUSY` if `ic_state != IEEE80211_S_RUN` (`ItlIwx.cpp:9171`), and `iwx_awdl_tx`/`iwx_awdl_tx_frame` use `ic->ic_bss` (`:9218-9318`). AWDL only works with Wi-Fi connected to an AP. This is the opposite of UN.
2. **Depends on firmware command versions**, not on the OS:
   - `PHY_CONTEXT_CMD` v3 (`iwx_p2p_phy_cmd`);
   - `SCAN_REQ_UMAC` v14 (`iwx_awdl_listen_v14`, `:9782`);
   - `ADD_STA` v12+ (`iwx_aux_roc_add_sta`);
   - `HOTSPOT_SUPPORT` capability (auxiliary ROC, `:9680`);
   - `IWX_AX210_GP2_REG 0xd02c68` (`:9697`).

   The AX210 firmware in this tree is `iwlwifi-ty-a0-gf-a0-68.ucode`, the same for both targets.
3. **Fixed firmware IDs:** `IWX_P2P_MAC_ID 1`, `PHY_IDX 1`, `STA_ID 3` (`:9050-9052`). These can collide if the STA ever uses MLO or a second MAC. Not OS-dependent.
4. **Effects on the STA when `ic_vendor_action_handler` is set** (only with `-itlawdl`):
   - the STA MAC starts accepting `IN_CONTROL_AND_MGMT` (`:8932`);
   - RX stops checking for duplicates on frames destined for awdl0 (`:5853+`);
   - `ieee80211_inputm` drops management frames from foreign BSSs (`ieee80211_input.c:339-372`).
5. **`iwx_tvqm_enable_txq`** gained the `sta_id` parameter, defaulting to `IWX_STATION_ID`, and the `AIRPORT_VTD` path with `runAction`. It is the same on Sequoia, because that target also defines `AIRPORT_VTD`.

   On AMD without VT-d: `ItlDma` falls back to "direct" mode when there is no mapper (`itl80211/ItlDma.cpp:38-49`). AWDL TX uses the existing `bus_dmamap_packet`, so it **adds no VT-d dependency**.
6. The boot-args (`-itlawdlp2p`, `-itlawdlpromisc`, and in AirportItlwm `-itlawdl`, `-itlawdlaw`, `-itlawdllisten`, `-itlawdltx`, `-itlawdldata`, `-itlawdlnopeerlisten`, `itlawdllistench=`) are version-independent.

---

## 6. Concrete port plan (one tree, two targets)

### Step 0: preparation

```sh
cp -R <crazyhulk/itlwm@ff8db43 checkout> itlwm-port
cd itlwm-port && git init && git add -A && git commit -m "base crazyhulk ff8db43"
git clone https://github.com/acidanthera/MacKernelSDK MacKernelSDK
git -C MacKernelSDK checkout 3f750085caa17ec3a7880f11c11bf4f48cd6a164
```

Xcode is **not installed** on the test machine (`python3`/`nm` triggered `xcode-select`). Full Xcode must be installed and `sudo xcode-select -s /Applications/Xcode.app` run before building.

> Status update: Xcode was installed later, and the port was built in all four Sequoia/Tahoe × Debug/Release configurations.

### Step 1: headers (`include/Airport/`)

1. Copy `IO80211VirtualInterface.h` and `IO80211AWDLProtocol.h` from UN into `include/Airport/macOS15/`.
   - Add to `macOS15/IO80211AWDLProtocol.h`: `static_assert(sizeof(IO80211AWDLProtocol) == 0x120, "Sequoia ABI size");`
   - In `macOS15/IO80211VirtualInterface.h:79`, change `void setLinkState(...)` to `bool setLinkState(...)`, following UN's finding. Leave the other slots as `void`, since CH does not call them.
2. Create the wrappers `include/Airport/IO80211VirtualInterface.h` and `include/Airport/IO80211AWDLProtocol.h`, following the pattern in `IO80211SkywalkInterface.h:10-14`:
   ```c
   #ifdef AIRPORT_WCL
   #if __IO80211_TARGET == __MAC_15_2
   #include "macOS15/IO80211AWDLProtocol.h"
   #else
   #include "macOS26/IO80211AWDLProtocol.h"
   #endif
   #endif
   ```
3. In `include/Airport/macOS15/IO80211ControllerV2.h`, after line 218, add (as in UN `macOS15/IO80211ControllerV2.h:219`):
   `void setAwdlInterface(IO80211VirtualInterface *) __asm__("__ZN17IO80211Controller16setAwdlInterfaceEP23IO80211VirtualInterface");`
   Do not add it to `macOS26`, because it is not verified there.
4. **Do not** import UN's other header changes (`getSelfMacAddr` → `uint64_t`, `registerEthernetInterface` → `IOReturn`, extra fields in `apple80211_ioctl.h`). They are not needed for CH's path and would change the STA. The exception is `apple80211_awdl_sync_params`: it is currently 20 bytes (`apple80211_ioctl.h:1044-1050`) and 24 in 24H32. Add `uint32_t non_election_broadcast_period;` only under `#if __IO80211_TARGET == __MAC_15_2`, as UN does in `apple80211_ioctl.h:1063-1066`.

### Step 2: a feature macro instead of "Tahoe-only"

Create `AirportItlwm/AirportItlwmAWDLConfig.h` (or put it at the top of `AirportItlwmV2.hpp`):

```c
#if defined(AIRPORT_WCL)          // Sequoia (__MAC_15_2) and Tahoe (__MAC_26_0)
#define AIRPORT_AWDL 1
#if __IO80211_TARGET == __MAC_15_2
#define AIRPORT_AWDL_ABI_SEQUOIA 1
#else
#define AIRPORT_AWDL_ABI_TAHOE 1
#endif
#endif
```

Replace **exactly** the 11 guards in the section 2 table with `#if defined(AIRPORT_AWDL)`. Do not touch `AirportItlwmSkywalkInterface.hpp:35/93/104`.

### Step 3: `AirportItlwmAWDLInterface.hpp`

- Lines 16–17: `#include <Airport/IO80211VirtualInterface.h>` and `#include <Airport/IO80211AWDLProtocol.h>`, i.e. the wrappers.
- Lines 28–32: the `extern "C"` declarations have the same mangled name on both versions. Keep them.
- Comments and `#endif` (`:579`): change "Tahoe" to "WCL".

### Step 4: `AirportItlwmAWDLInterface.cpp` (ABI shims)

1. Buffers (`:224-236` and uses at `:314-330`): under `#ifdef AIRPORT_AWDL_ABI_SEQUOIA`, replace `vcall(...)` with calls by symbol.
   ```c
   extern "C" AWDLBufferABI *IO80211BufferPool_getBuffer(AWDLBufferPoolABI *) __asm__("__ZN17IO80211BufferPool9getBufferEv");
   extern "C" void  IO80211Buffer_returnBuffer(AWDLBufferABI *) __asm__("__ZN13IO80211Buffer12returnBufferEv");
   extern "C" void  IO80211Buffer_setLength(AWDLBufferABI *, unsigned long) __asm__("__ZN13IO80211Buffer9setLengthEm");
   extern "C" unsigned long IO80211Buffer_getCapacity(const AWDLBufferABI *) __asm__("__ZNK13IO80211Buffer11getCapacityEv");
   extern "C" void *IO80211Buffer_getBytesNoCopy(AWDLBufferABI *) __asm__("__ZN13IO80211Buffer14getBytesNoCopyEv");
   ```
   This gives an `awdlBuf*()` wrapper with an `#if` per ABI. Keep `vcall` on Tahoe until it is verified that the same symbols exist there. After that, the two can be unified.
2. `AWDLRxMeta` (`:239-252`): keep. Add a safety boot-arg, for example `-itlawdlnoinject`, which makes `inputActionFrame` count frames and `return 1` without calling `bpfTapInput`. The first Sequoia boot then tests creation and listening without risking a panic in RX. Before enabling injection, verify the layout by disassembly (step 9).
3. AW events, `sync_state`, `sync_frame_template` and `channel_sequence`: leave unchanged, as they are confirmed.
4. `createAWDLInterface` (`:1529-1596`): keep the sequence. After `start`, on Sequoia, optionally call `setAwdlInterface(awdl)`, controlled by a `-itlawdlsetif` boot-arg, and call `setAwdlInterface(nullptr)` in `destroyAWDLInterface` (`:1614`). Whether the 24H32 family needs this is an open question (see risks).
5. `setAWDLLinkGated` (`:1605`): no changes. The return value of `setLinkState` is ignored.

### Step 5: `AirportItlwmV2.{hpp,cpp}` and `AirportItlwmSkywalkInterface.hpp`

Only the guard swap from step 2. The capability bits (`AirportItlwmV2.cpp:1060-1071`) are already under `AIRPORT_WCL`, and index 0x20 is confirmed in 24H32.

### Step 6: `itlwm.xcodeproj/project.pbxproj`

1. In `PBXBuildFile`, add a new entry with a different 24-hex-digit UUID, for example `5EA0AWDL…`:
   `NEWUUID /* AirportItlwmAWDLInterface.cpp in Sources */ = {isa = PBXBuildFile; fileRef = 3C6104E34D78EB99C79D1C20 /* AirportItlwmAWDLInterface.cpp */; };`
   Each PBXBuildFile belongs to a single phase, so `1EE81010096D5892BA04A0E1` cannot be reused.
2. Add that entry to the `files` list of phase `AFFBDFD057C2402C908ECB89 /* Sources */` (pbxproj:3359).
3. If `AirportItlwmAWDLConfig.h` is created, add it to the group (optional; it needs no phase).

### Step 7: build both targets

```sh
cd driver/AirportItlwm
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Sequoia -configuration Debug \
  ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe -configuration Debug \
  ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build     # regression: must still build
```

Checks after building:

- `nm -m …/Sequoia/AirportItlwm.kext/Contents/MacOS/AirportItlwm | grep -E "IO80211(VirtualInterface|AWDLProtocol|Buffer)"`. Every undefined symbol must exist in the KC.
- A `static_assert(sizeof(AirportItlwmAWDLInterface) > 0x120)` passes on its own.

### Step 8: offline verification against the KC (before any boot)

Use `ipsw kernel`/Ghidra/Hopper on `BootKernelExtensions.kc`:

1. Dump `__ZTV23IO80211VirtualInterface` and `__ZTV19IO80211AWDLProtocol`: confirm `init`@0xe60, `setLinkState`@0xe68 and the 63 AWDL slots starting at 0xfa0.
2. Disassemble `AppleBCMWLANProximityInterface::handleActionFrame_rx`: layout of the 0x48 bytes of meta and DLT 12.
3. Disassemble `AppleBCMWLANCore::createProxInterface`: order of `attach`/`start`/`setAwdlInterface`/registration in 24H32. CH copied the Tahoe order.
4. Disassemble `AppleBCMWLANProximityInterface::setAWDL_OOB_REQUEST`: offsets +40/+48.

### Step 9: staged tests (with `keepsyms=1 debug=0x100` and a rollback plan at hand)

1. `-itlawdl -itlawdlnoinject`: awdl0 should be created (`ifconfig awdl0`, `ioreg -l | grep AWDL`), the control ioctls should show up in the logs, and STA Wi-Fi should stay stable.
2. `+ -itlawdlaw`: AW timeline and 41/43 events.
3. `+ -itlawdllisten -itlawdlp2p`: P2P listening; watch `AWDLRxFrames`, `AWDLRxLastChannel`.
4. Remove `-itlawdlnoinject` (injection into the peer manager). Test discovery from a Mac or iPhone.
5. `+ -itlawdltx`, then `+ -itlawdldata -itlawdlpromisc`, and test the transfer.

Always test **with Wi-Fi associated**, because the `iwx` path requires it.

### Risks

1. **(Largest) Unverified layouts in the RX path:** the `IO80211Buffer` vtable offsets and the 0x48-byte `AWDLRxMeta`. A mistake here is a **kernel panic on the first AWDL action frame received**. Mitigation: calls by symbol, the `-itlawdlnoinject` boot-arg and verification against the KC.
2. **Creation sequence different from what the 24H32 family expects.** CH does `attach`/`start` directly. UN needed `attachVirtualInterface` + `setAwdlInterface` and checked that the peer manager was created. If on Sequoia `start` does not create the `IO80211AWDLPeerManager`, the AWDL ioctls never arrive. Mitigation: step 4.4 and inspecting `createProxInterface`.
3. **Symbol export:** the symbols exist in the KC, but it remains to confirm that `kmutil` accepts them for a third-party kext (`getMgmtFramePool`, `bpfTapInput`, `IO80211Buffer*`). UN links against several IO80211Family private symbols, which suggests it does.
4. **The Tahoe code is itself experimental.** It has boot-arg stages and was validated by reverse engineering Tahoe, with an unknown success rate. There is no evidence of a complete AirDrop on Sequoia via this path.
5. **Coexistence with the STA:** the open filter and `IN_PROMISC` on the P2P MAC may degrade normal Wi-Fi. AWDL requires an association.
6. **Host timing:** the AW timeline driven by `IOTimerEventSource` in µs, with ~6 ms of lead time for listening, may jitter on Ryzen without VT-d. There is no functional dependency on VT-d.
7. **Sequoia build variations:** the headers and layouts are from 24H32 (15.8.1). Another 15.x may shift slots. Mitigation: `start()` already checks `version_major == 24`. Add a check of the `IO80211AWDLProtocol` metaclass and refuse AWDL if `getMetaClass()->getClassSize()` of `IO80211VirtualInterface` is not 0x120.

### Open questions

- Does 24H32 need `IO80211Controller::setAwdlInterface` for `wifip2pd`/the peer manager to work? And does it exist on Tahoe?
- `apple80211_awdl_sync_params` on Tahoe: 20 or 24 bytes? On Sequoia it is 24.
- Layout of the OOB request (+40/+48) and of the RX meta in 24H32.
- Do the `IO80211Buffer*` symbol names exist unchanged on Tahoe? If so, unify the two ABIs without `vcall`.
- Does 24H32's `wifip2pd` also require capability bit 29 (byte 3, 0x20) for 5 GHz, as on Tahoe?
