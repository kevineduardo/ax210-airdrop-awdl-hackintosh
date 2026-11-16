// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL coex backend, -itlawdlcoex).
// See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
// Ported from ForbiddenItlwm (retropineaple), commit a5f1eb9, itlwm/hal_iwx/ItlIwx.cpp, GPL-2.0.
//
//  ItlIwxAWDLCoex.hpp
//
//  State of the AWDL "coex listener" engine: a second MAC context (a LISTENER, MAC id 1) on its own
//  PHY context and binding, beside the associated station, that receives and sends AWDL frames
//  without leaving the station's channel. The engine and its names are ForbiddenItlwm's; see
//  docs/how-awdl-works.md ("The coex backend").
//
//  Only ItlIwx.cpp includes this file (near its top, inside #ifdef AIRPORT_WCL). The globals are
//  file-scope statics, as in ForbiddenItlwm, so the hooks in iwx_tx, iwx_rx_tx_cmd, iwx_rx_frame
//  and friends see them under their original names. The function bodies are in ItlIwxAWDLCoex.cpp,
//  which ItlIwx.cpp includes once at its end.
//
//  Nothing here runs unless the front end set ic_awdl_coex_on before the HAL attached
//  (-itlawdl + -itlawdlcoex + the AWDL build guard). With it set, the P2P-device listen path is
//  never installed (see iwx_attach and iwx_p2p_listen_enabled): both use MAC id 1, PHY context 1
//  and firmware station 3, so the two backends must never run in the same boot.
//

#ifndef ItlIwxAWDLCoex_hpp
#define ItlIwxAWDLCoex_hpp

#include <AWDLCoex/AWDLFrameParser.hpp>
#include <AWDLCoex/AWDLFrameBuilder.hpp>
#include <AWDLCoex/AWDLDataFrame.hpp>
#include <AWDLCoex/AWDLPeerSchedule.hpp>
#include <AWDLCoex/AWDLRegulatory.hpp>
#include <AWDLCoex/AWDLChannels.hpp>
#include <AWDLCoex/AWDLObservation.hpp>

// The engine is selected for this boot (ic_awdl_coex_on at attach). Read by the P2P path's guards.
static bool awdlCoexSelected;
// Set at detach: AWDL timers and tasks stop re-arming.
static bool awdlStopping;
// The firmware generation whose capabilities were last published.
static int awdlCoexFwPublishedGen = -1;
// Where the counters go (the AirportItlwm controller; ForbiddenItlwm used the registry root), and
// the 802.11 state the engine writes ic_awdl_coex_* into.
static IOEthernetController *awdlCoexCtl;
static struct ieee80211com *awdlCoexIc;

// The engine's parts, derived from awdlCoexSelected in awdlCoexAttach. In ForbiddenItlwm these are
// -itlawdlcoexprobe (awdlCoexProbe), -itlawdlcoexrx (awdlCoexRx), -itlawdlcoextx (awdlCoexTx),
// -itlawdlcoexmulti (awdlCoexMulti) and -itlawdlcoexdata (awdlCoexData); here -itlawdlcoex turns on
// what each phase of the port has: phase 1 the listener, the country answers and receive (with data
// receive, awdlCoexDataRxOn, which ForbiddenItlwm ties to -itlawdlcoexdata).
static bool awdlCoexProbe, awdlCoexRx, awdlCoexTx, awdlCoexMulti, awdlCoexData, awdlCoexDataRxOn;
// -itlawdlcoexshared (A/B): never an own PHY context; the listener shares the station's binding.
static bool awdlCoexShared;
// -itlawdlsameband (#16 in ForbiddenItlwm): while the station is on 2.4 GHz, keep the listener on
// channel 6, in the same band, instead of time-sharing the radio across bands.
static bool awdlCoexSameBand;

// The listener: MAC context 1 (a LISTENER, TSF B), PHY context 1 when it has its own channel.
constexpr uint32_t kAwdlCoexMacId = 1;
constexpr int kAwdlCoexPhyId = 1;
constexpr uint32_t kAwdlCoexTxPeriodMS = 113;
static const uint8_t kAwdlCoexBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

// -itlawdlcoexmulti in ForbiddenItlwm: the listener gets the spare PHY context and its own binding on
// another channel, and the firmware, which manages time quotas itself (DYNAMIC_QUOTA), shares the radio
// between it and the station. Without it, or if that channel can't be used, the listener shares the
// station's.
static bool awdlCoexMultiActive;
static const char *awdlCoexMultiState = "off";
static uint32_t awdlCoexPhyResult, awdlCoexMultiBindResult;
// The listener's PHY context width. It applies only while that context's own command is being built:
// each PHY command records which context it builds before computing the width, so a station command
// built while ours waits can't pick it up.
static int awdlCoexPhyWidth = -1; // Only while our own PHY command is being built; -1 otherwise.
static int awdlPhyBuildingId = -1;
// The listener's own channel moves between 149 at 80 MHz and 6 at 20 MHz as the firmware's channel
// map allows. Its ACK and frame rates follow that channel's band, not the BSS's.
static struct ieee80211_channel *awdlCoexOwnChan;
static int awdlCoexOwnWidth = -1, awdlCoexWide80FailedGen = -1, awdlCoexCountryGen = -1;
static uint32_t awdlCoexMoves, awdlCoexMoveFailures, awdlCoexCountryNotices, awdlCoexCountryAnswers;
static uint32_t awdlCoexCountryLastStatus, awdlCoexCountryAnswerSeconds;
static uint16_t awdlCoexCountryNotice, awdlCoexCountryAnswered;
static uint8_t awdlCoexCountrySource, awdlCoexCountrySourceAnswered;
static bool awdlCoexCountryPending, awdlCoexCountryLearned;
// Bits 0-3: a beacon hint on 149, 153, 157, 161; bit 4: on 6 (ITLAWDLCoexBeaconHints).
static uint32_t awdlCoexBeaconHints;
static uint64_t awdlCoexLastMoveUS;
// A move waits for the listener's ring to drain; nothing new is submitted meanwhile.
static bool awdlCoexMovePending;
static uint64_t awdlCoexMovePendingUS, awdlCoexCountryRetryUS;
static uint32_t awdlCoexMoveDrainTimeouts, awdlCoexCountryFailures;
// A PHY command for the listener's context that failed without a readable answer leaves it uncertain;
// the own channel isn't used again until the firmware restarts.
static int awdlCoexPhyUncertainGen = -1;
// An add in progress (its generation), and a removal that arrived meanwhile: the add rolls back.
static int awdlCoexAddingGen = -1;
static uint32_t awdlCoexAddingOwner, awdlCoexAddingNext;
static bool awdlCoexRemoveRequested;
// The usable country the beacon hints were gathered under: hints are cleared only for a different
// usable country, not for a detour through the firmware's world code.
static uint16_t awdlCoexHintCountry;
static uint32_t awdlCoexCountryUnusableAnswers, awdlCoexHintClears;
// The channel our frames name (ForbiddenItlwm's awdlFixedChannel).
static unsigned awdlFixedChannel = 6;

static bool awdlCoexAdded;
// itlawdlcoexfilter in ForbiddenItlwm. 1: AWDL's BSSID with group frames, no promiscuous mode.
// 3 (used here): the same, plus promiscuous receive while the listener is on its own channel,
// different from the station's; a LISTENER context without it passes AWDL's action frames but no data.
static uint32_t awdlCoexFilter = 1;
// The address in the firmware's context: awdl0's (ic_awdl_addr) once known, before that a locally
// administered one. The firmware ACKs only its context's address.
static bool awdlCoexAddrKnown, awdlCoexAddrApplied;
static uint8_t awdlCoexAddr[6], awdlCoexAddrInFirmware[6];
static uint32_t awdlCoexAddrChanges, awdlCoexAddrFailures;
static uint64_t awdlCoexAddrRetryUS;
// Changes on every listener add or remove and every firmware restart. Work that waited on the firmware
// checks it (with the generation and the stop flag) before touching anything again.
static uint32_t awdlCoexToken;
// The firmware generation a removal is running in: a second add for the same generation waits.
static int awdlCoexRemovingGen = -1;
// A failed teardown command leaves the firmware's state unknown, so nothing is re-added until the
// firmware restarts (the generation changes).
static int awdlCoexBrokenGeneration = -1;
static uint32_t awdlCoexTeardownErrors;
static int awdlCoexGeneration = -1;
static uint32_t awdlCoexAdds, awdlCoexRemoves, awdlCoexMacResult, awdlCoexBindResult, awdlCoexBindStatus;
// Why the listener was added again: after a normal removal, or after a firmware restart dropped it.
static uint32_t awdlCoexAddsAfterRemove, awdlCoexAddsAfterRestart;
static bool awdlCoexLostToRestart;

// The listener's station and queue (phase 2); declared here because the listener's add and removal
// reset them.
static bool awdlCoexTxStaAdded, awdlCoexInFlight, awdlCoexTxHalted;
static int awdlCoexQueue = -1, awdlCoexPendingSlot = -1, awdlCoexOrphanQueue = -1;
static uint32_t awdlCoexTxSetupTried;
constexpr uint32_t kAwdlCoexTxSetupTries = 3;
// Block Ack with the peer (later phases of the port). Always off here.
static bool awdlCoexBaPeerAdded, awdlCoexBaFilterOff;
static uint8_t awdlCoexBaPeer[6];

// Receive.
static uint32_t awdlCoexForwarded, awdlCoexForwardRefused, awdlCoexForwardMalformed;
static uint32_t awdlCoexZeroPadTrimmed;   // listener frames whose trailing zero padding was cut
static uint32_t awdlCoexAwdlAction, awdlCoexAwdlData, awdlCoexOther, awdlCoexChannel, awdlCoexPublished;
static uint32_t awdlCoexActionOnChannel, awdlCoexActionOffChannel;
static uint32_t awdlCoexBaActionSeen[4];
// The peer's latest 16-slot channel sequence (TLV 18), for diagnosis only.
static uint8_t awdlCoexPeerSeq[16];
static uint32_t awdlCoexPeerSeqFrames, awdlCoexPeerSeqChanges;
// Data is handed to awdl0 only from a source heard sending valid AWDL action frames on our channel in
// the last 60 s. Addresses stay in memory only and are never published.
struct AwdlCoexPeer { uint8_t addr[6]; uint64_t heardUS; unsigned channel; uint32_t token; };
static AwdlCoexPeer awdlCoexPeers[8];
static uint32_t awdlCoexDataRxUnknownPeer, awdlCoexDataRxOffChannel;
// Why a data source was refused, by count only: 0 never heard, 1 heard over 60 s ago, 2 heard on another
// channel, 3 heard by an earlier listener (token).
static uint32_t awdlCoexRejectReasons[4];
static uint32_t awdlCoexDataRx, awdlCoexDataRxDelivered, awdlCoexDataRxRefused, awdlCoexDataRxNotIPv6;
static uint32_t awdlCoexDataRxRetry, awdlCoexRxModulation[8], awdlCoexRxWide80;
static uint32_t awdlCoexDataRxUcast, awdlCoexDataRxGroup, awdlCoexPromiscDropped;
static uint64_t awdlCoexLastDataUS;
static bool awdlCoexPromiscLive;      // Applied: the firmware accepted a MAC command that set it.
static bool awdlCoexPromiscUncertain; // A MAC command failed: the firmware may hold either filter.
static uint32_t awdlCoexPromiscAmbiguous, awdlCoexMacMaskSeen[4];
// The RX descriptor's MAC context mask, valid only for the frame being handed to iwx_rx_frame directly
// (not frames released later from a reorder buffer).
static bool awdlRxMacMaskValid;
static uint8_t awdlRxMacMask;

// The engine's task: country answers, channel moves, address changes, publication (and from phase 2
// sync frames and data).
static CTimeout *awdlCoexTxTimer;
static struct task awdlCoexTxTaskEntry;
static uint64_t awdlCoexLastRoutinePublishUS;

static uint64_t awdlUptimeUS();
static void awdlStopTimers();
static void awdlCoexPublish();
static void awdlCoexAddrSyncWanted();

#endif /* ItlIwxAWDLCoex_hpp */
