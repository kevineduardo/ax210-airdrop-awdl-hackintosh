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
#include <AWDLCoex/AWDLProbePolicy.hpp>
#include <AWDLCoex/AWDLBlockAck.hpp>
#include <AWDLCoex/AWDLSameChannelProbe.hpp>
#include <AWDLCoex/AWDLServiceMerge.hpp>

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
// Phase 5 (ours): itlawdlcoexchan=N, the listener's channel preference. 0, the default, keeps ForbiddenItlwm's
// order (149 at 80 MHz, then 6 at 20 MHz). 149: the same, with 44 at 80 MHz tried before 6. 44: 44 at 80 MHz
// first, then 149, then 6. 6: 6 first, then 149, then 44. Where 149 to 161 are passive (no AP heard there) or
// not allowed at 80 MHz, 36 to 48 often are usable, and the iPhone itself puts its own slots on 44.
static unsigned awdlCoexChanPref;
// The first channel of the 80 MHz block a wide primary sits in (36 for 36 to 48, 149 for 149 to 161), else 0.
static inline unsigned awdlCoexWideBlock(unsigned primary)
{
    return primary >= 36 && primary <= 48 && primary % 4 == 0 ? 36 :
        primary >= 149 && primary <= 161 && (primary - 149) % 4 == 0 ? 149 : 0;
}

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
// Phase 3: receive Block Ack (-itlawdlcoexba). The iPhone's ADDBA request gets a firmware station for the
// peer (station 3, bound to the listener's MAC context) and a firmware session, then an answer from the
// listener's queue. While a session is open, promiscuous receive is off: the firmware doesn't acknowledge a
// session's frames with it on. Off by default during the rollout (ForbiddenItlwm runs it with its data mode).
constexpr uint8_t kAwdlBaStationId = 3;
static bool awdlCoexBaRx;
static bool awdlCoexBaPeerAdded, awdlCoexBaFilterOff;
static uint8_t awdlCoexBaPeer[6];
static ItlAWDL::PendingAddbaRequests awdlCoexBaRequestsPending;
static uint8_t awdlCoexBaTidToken[8];
static uint8_t awdlCoexBaTids;
static int awdlCoexBaFailedGen = -1, awdlCoexBaGen = -1;
static uint32_t awdlCoexBaRequests, awdlCoexBaSessions, awdlCoexBaAnswers, awdlCoexBaAnswerErrors,
    awdlCoexBaOtherPeer, awdlCoexBaDelbas, awdlCoexBaStatus, awdlCoexBaFilterChanges;
static uint8_t awdlCoexBaAnswer[ItlAWDL::kAddbaResponseLength];
static size_t awdlCoexBaAnswerLength;
static uint8_t awdlCoexBaAnswerTid;
static uint64_t awdlCoexBaAnswerRetryUS;
static uint16_t awdlCoexBaSequence;
static ItlAWDL::BaFilterState awdlCoexBaFilter;
// The width the last peer station was added with (80 or 20; 0 before the first).
static uint32_t awdlCoexBaPeerWidth;
// Our management frames (ADDBA answers and requests, DELBA) are tagged per ring slot so they never count
// toward the data rate, and so each one pairs with its own completion.
enum { kCoexSlotBaAnswer = 3, kCoexSlotBaAsk, kCoexSlotDelba };
static uint8_t awdlCoexSendingMgmt, awdlCoexSendingMgmtToken;
// The iPhone acknowledged our ADDBA answer.
static bool awdlCoexBaPeerAccepted;
// Bumped on every peer station add: completions and queue owners from an older peer are recognised.
static uint32_t awdlCoexBaPeerEpoch;
static uint32_t awdlCoexMgmtOK, awdlCoexMgmtFailed;
// DELBA from the iPhone for its own session, or our idle teardown: those TIDs are removed from the firmware
// on the next pass.
static uint8_t awdlCoexBaTidTeardown;
static uint64_t awdlCoexBaPeerLastUS;
// Block Ack action frames to our AWDL address, by whether they carry AWDL's BSSID.
static uint32_t awdlCoexBaActionAwdlBssid, awdlCoexBaActionOtherBssid, awdlCoexRxWhileFilterOff;
static uint64_t awdlCoexBaLastRxPayloadUS;
static uint32_t awdlCoexBaRxIdleTeardowns, awdlCoexBaDelbasSent;
static uint8_t awdlCoexBaRxIdleTids;
// Idle receive teardown: the session is removed only after the iPhone acknowledges our DELBA, or after three
// failed tries or a 3 s deadline. Keyed by firmware generation, peer epoch and TID.
static uint32_t awdlCoexDelbaTries, awdlCoexDelbaForced, awdlCoexDelbaConfirmed, awdlCoexDelbaFailedTx;
static int awdlCoexDelbaWaitTid = -1, awdlCoexDelbaWaitGen = -1;
static uint32_t awdlCoexDelbaWaitEpoch;
static uint16_t awdlCoexDelbaAttemptId;
static uint8_t awdlCoexDelbaResult; // 0 none or pending, 1 acknowledged, 2 failed.
static uint64_t awdlCoexDelbaDeadlineUS, awdlCoexDelbaNextTryUS;
// Last QoS data from the BA peer per TID: a receive session is idle only when its own TID is.
static uint64_t awdlCoexBaRxTidLastUS[8];
static uint32_t awdlCoexBaNonAwdlBssidAccepted, awdlCoexBaNonAwdlBssidApplied, awdlCoexDelbaRxTidRemoved;
static uint32_t awdlCoexRxVht80TwoStream;
// Phase 4: send Block Ack (-itlawdlcoexbatx, which turns -itlawdlcoexba on too). The iPhone's station gets
// a firmware aggregation queue on TID 0 with firmware rate control (VHT80, two streams, MCS 4 to 9 at 80 MHz),
// and our ADDBA request. Once the iPhone agrees, its payload frames go out as QoS data on that queue and the
// firmware aggregates them; short exchanges stay on the listener's single-frame queue.
enum { kCoexBaTxOff, kCoexBaTxAsking, kCoexBaTxAgreed, kCoexBaTxFailed, kCoexBaTxEnabling };
constexpr uint8_t kAwdlBaTxTid = 0;
// 40 asks a round, then 20 s of rest: a phone that is mostly away may miss a whole round.
constexpr uint32_t kAwdlBaTxAsksMax = 40;
constexpr uint64_t kAwdlBaTxRestUS = 20000000;
// Asking stops once nothing has gone to or come from the peer for this long and no bulk send runs.
constexpr uint64_t kAwdlCoexBaIdleAskUS = 30000000;
constexpr uint32_t kAwdlCoexBaTxQueuedMax = 128;
static bool awdlCoexBaTxEnabled, awdlCoexBaPeerPauseEnabled;
static int awdlCoexBaTxQueue = -1, awdlCoexBaTxResetting = -1;
static int awdlCoexBaTxState = kCoexBaTxOff;
static uint16_t awdlCoexBaTxSeq;
static uint8_t awdlCoexBaTxToken = 0x5a;
static uint32_t awdlCoexBaTxAsks, awdlCoexBaTxAskRounds, awdlCoexBaTxAgreements, awdlCoexBaTxRefusals,
    awdlCoexBaTxFrames, awdlCoexBaTxSubmitErrors, awdlCoexBaTxNotifs, awdlCoexBaTxAcked, awdlCoexBaTxSentAir,
    awdlCoexBaTxQueueResult, awdlCoexBaTxDelbas, awdlCoexBaTxPeerAdds;
static uint64_t awdlCoexBaTxLastAskUS, awdlCoexBaTxRestUntilUS;
static uint32_t awdlCoexBaTxIdleHolds;
static ItlAWDL::BaTxBulkState awdlCoexBaTxBulk;
// The pump is converting a frame for the aggregation queue (no fixed rate: the firmware's rate control picks).
static bool awdlCoexBaSendingAgg;
static bool awdlCoexBaQueueAllocPending;
static uint32_t awdlCoexBaTxAggEnableStatus;
// The unicast destination of our data and how many payload frames went to it, for picking the TX peer.
static uint8_t awdlCoexTxDest[6];
static uint32_t awdlCoexTxDestFrames;
static uint32_t awdlCoexBaAskOK, awdlCoexBaAskFailed, awdlCoexBaAsksSkippedAway, awdlCoexAskSkipsSinceLast;
static uint32_t awdlCoexBaTxResponses, awdlCoexBaTxResponseFrames, awdlCoexBaTxAggResponses;
static uint32_t awdlCoexBaTxPeakQueued, awdlCoexBaTxSinglesWithBacklog, awdlCoexBaTxPausedFullDeferrals;
static uint32_t awdlCoexBaTxRespOK, awdlCoexBaTxRespFailed, awdlCoexBaTxRespLastFailStatus;
static uint32_t awdlCoexBaTxVht80TwoStreamNotifs, awdlCoexBaTxPeakReportedFrames, awdlCoexBaTxLastRate;
static uint64_t awdlCoexBaTxLastAggUS, awdlCoexBaTxLastNotifUS;
static uint32_t awdlCoexBaEarlyAggFrames;
// The answer's window (0 until agreed). Under 64 it caps the frames outstanding on the aggregation queue.
static uint16_t awdlCoexBaTxWindow;
static uint32_t awdlCoexBaTxSmallWindows, awdlCoexBaTxPolicyRefusals;
static uint32_t awdlCoexBaTxQueuedCap()
{
    return awdlCoexBaTxWindow && awdlCoexBaTxWindow < 64 ? awdlCoexBaTxWindow : kAwdlCoexBaTxQueuedMax;
}
// The aggregation queue's owner: listener token, generation and peer epoch, checked again after the
// firmware answers the queue request, so a stale answer installs nothing.
struct CoexBaQueueOwner { int generation; uint32_t listener, peer; };
static bool awdlCoexBaQueueCurrent(int generation, uint32_t listener, uint32_t peer)
{
    return awdlCoexAdded && awdlCoexBaPeerAdded &&
        awdlCoexGeneration == generation && awdlCoexToken == listener && awdlCoexBaPeerEpoch == peer;
}
// Rate control for the peer's station (iwx_rs_init): set only while that command is built.
static bool awdlBaTlc, awdlBaTx80Active;
static uint32_t awdlBaTxMinMcs = 4;
// Ordering rule (on unless -itlawdlcoexnoorder): everything unicast to the Block Ack peer takes one path, in
// order, while its aggregation queue is paused, full or still holds frames, so a short QUIC packet never
// overtakes held payload on the single-frame queue (QUIC would take that as loss).
static bool awdlCoexOrder = true;
static uint32_t awdlCoexOrderViaAgg, awdlCoexOrderSingleBehindAgg, awdlCoexOrderStalled;
// Peer pause (-itlawdlcoexpeerpause): while aggregate frames move and the iPhone's schedule says it is away,
// its station's transmit is disabled in the firmware (ADD_STA DISABLE_TX), then enabled again.
static ItlAWDL::PeerPauseState awdlCoexPeerPause;
static ItlAWDL::PeerBulkState awdlCoexPeerBulk;
static uint64_t awdlCoexResumeRetryUS;
static bool awdlCoexResumePending;
static uint32_t awdlCoexResumeFailStreak, awdlCoexResumeRetirements, awdlCoexPauseResumeFailures;
static uint32_t awdlCoexGateQueuedPauses;
// Probe window: with a receive session open and our ask unanswered, promiscuous receive is opened once for
// 250 ms per dialog, to see whether the answer arrives then. -itlawdlcoexsamechprobe also opens it while the
// listener is on the station's own channel (normally never promiscuous there).
static uint64_t awdlCoexBaProbeUntilUS;
static bool awdlCoexBaProbeTried;
static ItlAWDL::SameChannelProbe awdlCoexSameChannelProbe;

// Per-enqueue identity for listener management frames, kept with the slot (see awdlCoexNoteSubmitted).
static uint16_t awdlCoexMgmtEnqueueId, awdlCoexLastMgmtEnqueueId;
// Block Ack event log (types as in ForbiddenItlwm: A ask, a ask done, R answer, B BA action to us, Q request,
// q our answer done, d our DELBA done, D peer DELBA, L our DELBA sent, F forced teardown, S send state,
// X peer retired, P probe window, u reclaimed without status). Recorded always, published with
// -itlawdlcoexbatrace (phase 5).
struct AwdlCoexBaEvent { uint32_t ms; char type; uint16_t a, b; uint8_t c; uint16_t d, id; };
static AwdlCoexBaEvent awdlCoexBaEvents[256], awdlCoexBaEventsFirst[64];
static uint32_t awdlCoexBaEventCount;
// Ours, for phase 5's decision on a host reorder buffer: the order the BA peer's QoS data arrives in. The
// firmware acknowledges the session's frames; the host hands them to awdl0 in arrival order, as ForbiddenItlwm
// does. A fresh sequence number behind the highest one seen on its TID counts as out of order.
static ItlAWDL::RxSequenceTracker awdlCoexRxSeq;
static uint16_t awdlCoexRxSeqHigh[8];
static uint8_t awdlCoexRxSeqHighKnown;
static uint32_t awdlCoexRxSeqOutOfOrder, awdlCoexRxSeqMaxBehind;

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

// Phase 2: the listener's own station and queue, sync frames, single-frame data, the rate ladder and
// the send gate.
constexpr uint64_t kAwdlCoexTxStuckUS = 1000000;
static bool awdlCoexSending, awdlCoexSendingData;
static uint64_t awdlCoexInFlightUS;
static uint32_t awdlCoexStaResult, awdlCoexQueueResult;
static uint32_t awdlCoexTxBuilt, awdlCoexTxNotReady, awdlCoexTxSubmitted, awdlCoexTxSubmitErrors;
static uint32_t awdlCoexTxBuildRejected;
// Why the channel is refused right now, as bits: 1 not in the fixed-schedule list, 2 passive (net80211's
// flag for a channel the firmware's profile doesn't mark active, without a beacon hint), 4 DFS, 8 no ad hoc,
// 16 not active, 32 not the channel our frames name, 64 a move is pending. 0 while it is eligible.
static uint32_t awdlCoexRefusedReason, awdlCoexRefusedChannelFlags;
static char awdlCoexRefusalSnapshot[128];
static uint32_t awdlCoexTxOK, awdlCoexTxFailed, awdlCoexTxStuck, awdlCoexTxRefusedChannel;
static uint32_t awdlCoexSetupAttempts, awdlCoexSetupAborts, awdlCoexBadCompletions, awdlCoexLateCompletions;
static int awdlCoexLastSlot = -1, awdlCoexLastBoundary = -1, awdlCoexPendingGeneration = -1;
static uint32_t awdlCoexLastSsn = 0xffffffffU;
static char awdlCoexFaultSnapshot[160];
// Where the last queue allocation stopped, and the number the firmware gave. Set by setup just before it
// asks for the queue: TVQM checks them after the firmware's answer, before it adopts a host ring, so a
// stale answer can't install one.
static const char *awdlCoexQueueStage = "none";
static int awdlCoexQueueAssigned = -1;
static uint32_t awdlCoexQueueWritePointer = 0xffffffffU;
static uint32_t awdlCoexSetupToken;
static int awdlCoexSetupGen = -1;
static bool awdlCoexSetupCurrent(int generation)
{
    return awdlCoexSetupToken == awdlCoexToken && awdlCoexAdded && !awdlStopping && awdlCoexSetupGen == generation;
}
// A sync frame can wait behind retried data while the radio is on the station's channel. A stall counts
// only if no completion at all came for a second, or the sync frame waited 5 s; it pauses sending for
// 0.5 s, and only three stalls in a row with no completion between them halt TX.
static uint64_t awdlCoexLastCompletionUS, awdlCoexTxPauseUntilUS;
static uint32_t awdlCoexStuckStreak, awdlCoexTxPauses, awdlCoexWedgedRebuilds;
static uint32_t awdlCoexDataTx, awdlCoexDataTxErrors, awdlCoexDataOK, awdlCoexDataFailed, awdlCoexDataPumps;
static uint64_t awdlCoexLastSyncUS;
static uint32_t awdlCoexIdleSyncFrames;
// Phase 5: -itlawdlawsync (ForbiddenItlwm's switch). While no data moves, a sync frame also goes at the start
// of the iPhone's AWs in its entries on our channel, where an idle iPhone is awake, so it finds us sooner. At
// most one per kAwdlAwSyncMinGapUS, within kAwdlAwSyncWindowUS of the AW start; the task wakes for the next AW.
static bool awdlAwSync;
static uint32_t awdlCoexAwSyncFrames;
// Phase 5: -itlawdlsvcmerge (ForbiddenItlwm's switch). The iPhone spreads its service records (TLV 2) over
// several frames with the same service update indicator; each frame from it heard on our channel gets the
// records it lacks added from the ones cached for that indicator, so macOS sees them all sooner.
static bool awdlServiceMerge;
static ItlAWDL::PeerServiceCache awdlServiceCache;
static uint32_t awdlCoexServiceCacheToken;
static uint32_t awdlServiceMergedFrames, awdlServiceMergedRecords, awdlServiceMergeRefused;
static uint8_t awdlServiceMergeBuf[ItlAWDL::kServiceMergeFrameMax];
constexpr uint64_t kAwdlAwSyncWindowUS = 2000, kAwdlAwSyncMinGapUS = 8000;
// TX response attribution: the response's status is for the descriptor its header index names; the SSN
// only says how far to reclaim. Slots reclaimed without their own status are counted, not credited.
static uint32_t awdlCoexStatusIdxHits, awdlCoexStatusIdxMisses, awdlCoexMultiReclaims, awdlCoexReclaimUnknown,
    awdlCoexSyncReclaimedUnknown;
// Per ring slot: 1 for a group data frame, 2 for unicast, so completions say whether the peer acknowledged us;
// 3 to 5 for our Block Ack management frames (kCoexSlotBaAnswer and after).
static uint8_t awdlCoexSlotKind[IWX_MIN_256_BA_QUEUE_SIZE_GEN3];
static uint32_t awdlCoexSlotBaPeerEpoch[nitems(awdlCoexSlotKind)];
static uint8_t awdlCoexSlotToken[nitems(awdlCoexSlotKind)]; // Dialog token (ADDBA) or TID (DELBA) per slot.
static uint16_t awdlCoexSlotEnqueueId[nitems(awdlCoexSlotKind)];
static uint32_t awdlCoexUcastOK, awdlCoexUcastFailed, awdlCoexGroupOK, awdlCoexGroupFailed;
// Unicast data rate for our frames: 0 legacy 24 Mbit/s, then HT MCS 4, 5, 7 (20 MHz, one stream). Steps
// down after a window with over 20% failures, up after one with under 5%. Steps 4 and 5 are VHT 80 MHz,
// two streams, MCS 4 and 7, only while the listener is at 80 MHz.
static unsigned awdlCoexRateStep;
static uint32_t awdlCoexRateWindowOK, awdlCoexRateWindowFailed, awdlCoexRateUps, awdlCoexRateDowns;
static const uint8_t kAwdlCoexRateMcs[6] = {0, 4, 5, 7, 4, 7};
static uint32_t awdlCoexVhtFrames;
// The peer whose schedule is tracked (its TLV 4 and TLV 18), for the send gate.
static ItlAWDL::PeerSchedule awdlCoexPeerSchedule;
static uint8_t awdlCoexSchedPeer[6];
static uint32_t awdlCoexSchedToken;
static uint32_t awdlCoexPeerSyncFrames, awdlCoexPeerSequenceRejected;
static bool awdlCoexScheduleCalibrated;
static bool awdlCoexScheduleCurrent()
{
    return awdlCoexAdded && awdlCoexSchedToken == awdlCoexToken &&
        awdlCoexPeerSchedule.ourChannel == awdlCoexChannel &&
        (!awdlCoexBaPeerAdded || ItlAWDL::sameAddress(awdlCoexSchedPeer, awdlCoexBaPeer));
}
// Send gate (on unless -itlawdlcoexnogate). Frames for the peer leave the host only while its schedule has
// it on our channel, and while it is idle only in the first kAwdlGateAwWindowUS of an AW.
static bool awdlCoexGate = true;
static uint64_t awdlCoexGateSinceUS, awdlCoexGateLimitUS;
static uint8_t awdlCoexGateKind;
static uint32_t awdlCoexGateHolds, awdlCoexGateAwayTicks, awdlCoexGateDozeTicks, awdlCoexGateForced;
// Last data frame from the schedule peer: a peer that sent data in the last 100 ms is awake.
static uint64_t awdlCoexSchedPeerRxUS;
constexpr uint64_t kAwdlGateAwWindowUS = 4000, kAwdlGateAwakeUS = 100000;
// -itlawdlcoexcalwait off: the schedule assumes offset 0 until refuted (PeerSchedule::assumeZero), so the
// gate works from the first sync frame after each anchor.
static bool awdlCoexAssumeZero = true;

// The engine's task: country answers, channel moves, address changes, sync frames, data, publication.
static CTimeout *awdlCoexTxTimer;
static struct task awdlCoexTxTaskEntry;
static uint64_t awdlCoexLastRoutinePublishUS;

static uint64_t awdlUptimeUS();
static void awdlStopTimers();
static void awdlCoexPublish();
static void awdlCoexAddrSyncWanted();
static void awdlCoexNoteSubmitted(struct iwx_softc *, int qid, int idx, const struct ieee80211_frame *);
static bool awdlCoexOwnsRing(const struct iwx_tx_ring *);
static bool awdlCoexBaOwnsRing(const struct iwx_tx_ring *);
static bool awdlCoexRingHoldsRefs(const struct iwx_tx_ring *);
static void awdlCoexBaTxResponse(const struct iwx_tx_resp *, const struct iwx_tx_ring *);
static void awdlCoexBaTxNotif(const struct iwx_compressed_ba_notif *);
static bool awdlVht80TwoStream(uint32_t rate);
static mbuf_t awdlCoexFrameMbuf(const uint8_t *bytes, size_t length);

#endif /* ItlIwxAWDLCoex_hpp */
