// Modified by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop port to
// macOS 15 Sequoia and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  AirportItlwmAWDLInterface.hpp
//  AirportItlwm
//
//  awdl0 for macOS 26 (Tahoe) and macOS 15 (Sequoia): the IO80211AWDLProtocol interface,
//  a host-side availability-window timeline, listen/transmit on Intel P2P Device dwells and
//  a data path. The contract was reverse-engineered from the macOS 26 IO80211Family and
//  AppleBCMWLAN kexts; the Sequoia differences were checked against the 15.8.1 (24H32)
//  kernel collection (see AIRPORT_AWDL_ABI_SEQUOIA below).
//

#ifndef AirportItlwmAWDLInterface_hpp
#define AirportItlwmAWDLInterface_hpp

#include "AirportItlwmAWDLConfig.h"

#if defined(AIRPORT_AWDL)

#include <Airport/Apple80211.h>
#include <Airport/IO80211VirtualInterfaceWCL.h>
#include <Airport/IO80211AWDLProtocol.h>
#include "AirportItlwmEthernetInterface.hpp"
#include <IOKit/IOTimerEventSource.h>

class AirportItlwm;
struct ieee80211_rxinfo;

// IO80211Family buffer objects, used only through their vtables / exported
// functions. Slot offsets are from the Tahoe kernel collection.
class AWDLBufferPoolABI;
class AWDLBufferABI;
// Same mangled names on Sequoia 24H32: both are exported by IO80211Family and
// imported by name by AppleBCMWLANCoreMac (verified in the 15.8.1 KC symtabs).
extern "C" AWDLBufferPoolABI *IO80211VirtualInterface_getMgmtFramePool(IO80211VirtualInterface *)
    __asm__("__ZN23IO80211VirtualInterface16getMgmtFramePoolEv");
extern "C" void IO80211VirtualInterface_bpfTapInput(IO80211VirtualInterface *, AWDLBufferABI *,
                                                    unsigned int, void *, unsigned long)
    __asm__("__ZN23IO80211VirtualInterface11bpfTapInputEP13IO80211BufferjPvm");

#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
// On 24H32 IO80211Buffer has no virtual methods of its own: __ZTV13IO80211Buffer ends
// after OSObject::_RESERVEDOSObject15 at +0x110, so Tahoe's buffer slots (0x118..0x158)
// would read past the vtable. Its methods are plain exported functions, called by name
// exactly like AppleBCMWLANCoreMac does (it imports returnBuffer, setLength and
// getBytesNoCopy by symbol). getCapacity is exported but not imported by Broadcom.
extern "C" void IO80211Buffer_returnBuffer(AWDLBufferABI *)
    __asm__("__ZN13IO80211Buffer12returnBufferEv");
extern "C" void IO80211Buffer_setLength(AWDLBufferABI *, unsigned long)
    __asm__("__ZN13IO80211Buffer9setLengthEm");
// Return width not encoded in the mangled name: only the low 32 bits are trusted.
extern "C" unsigned long IO80211Buffer_getCapacity(const AWDLBufferABI *)
    __asm__("__ZNK13IO80211Buffer11getCapacityEv");
extern "C" void *IO80211Buffer_getBytesNoCopy(AWDLBufferABI *)
    __asm__("__ZN13IO80211Buffer14getBytesNoCopyEv");
#endif

// Role values used by IO80211 on Tahoe and Sequoia (apple80211_virtual_if_role).
enum : unsigned int {
    kAirportItlwmRoleAWDL = 6,
};

// BSD side of awdl0. Same pairing as the station interface (legacy ifnet + Skywalk
// control interface), but it never touches the shared radio: interface flag changes
// are handled locally and output is dropped until the AWDL datapath exists.
class AirportItlwmAWDLEthernetInterface : public AirportItlwmEthernetInterface {
    OSDeclareDefaultStructors(AirportItlwmAWDLEthernetInterface)

public:
    const char *getNamePrefix() const override;
    // ifnet_allocate_extended rejects a uniqueid already used by another ifnet
    // (EBUSY), so awdl0 must not reuse the station's kIOMACAddress.
    bool initIfnetParams(struct ifnet_init_params *params) override;
    // ifnet_attach takes its link-layer address from the controller's
    // kIOMACAddress (the station MAC); replace it with awdl0's own.
    IOReturn attachToDataLinkLayer(IOOptionBits options, void *parameter) override;
    void setAWDLAddress(const ether_addr &mac) { memcpy(&fMac, &mac, sizeof(fMac)); }
    IOReturn getInterfaceAddress(IOEthernetAddress *addr) override
    {
        memcpy(addr, &fMac, sizeof(fMac));
        return kIOReturnSuccess;
    }
    SInt32 performCommand(IONetworkController *controller, unsigned long cmd,
                          void *arg0, void *arg1) override;
    UInt32 dropOutput(mbuf_t m, void *param);
    uint64_t droppedPackets() const { return fDropped; }
    // Stage 4: an AWDL data frame for awdl0; consumes m, returns 1.
    int inputAWDLData(mbuf_t m, class AirportItlwmAWDLInterface *awdl);
    // Stage 4 output: hand Ethernet frames to the AWDL interface's tx queue.
    UInt32 outputAWDL(mbuf_t m, void *param);
    void setAWDLOwner(class AirportItlwmAWDLInterface *owner) { fOwner = owner; }

private:
    class AirportItlwmAWDLInterface *fOwner;
    uint64_t fDropped;
    ether_addr fMac;
};

class AirportItlwmAWDLInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(AirportItlwmAWDLInterface)

public:
    static AirportItlwmAWDLInterface *withController(AirportItlwm *controller, ether_addr *mac);

    // IO80211VirtualInterface
    bool init(IO80211Controller *controller, ether_addr *mac, unsigned int role, char const *prefix) override;
    void free() override;
    IOReturn setMacAddress(ether_addr &) override;
    // Diagnostic tunables for the listen dwells, settable at run time via setProperties.
    IOReturn setProperties(OSObject *properties) override;

    // IO80211AWDLProtocol: getters
    IOReturn getSSID(apple80211_ssid_data *) override;
    IOReturn getCHANNEL(apple80211_channel_data *) override;
    IOReturn getBSSID(apple80211_bssid_data *) override;
    IOReturn getSTATE(apple80211_state_data *) override;
    IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *) override;
    IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *) override;
    IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *) override;
    IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id *) override;
    IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *) override;
    IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *) override;
    IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size *) override;
    IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *) override;
    IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state *) override;
    IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *) override;
    IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *) override;
    IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *) override;
    IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities *) override;
    IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *) override;
    IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *) override;
    IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate *) override;
    IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt *) override;
    IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period *) override;
    IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats *) override;
    IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config *) override;
    IOReturn getAWDL_CCA(apple80211_awdl_cca *) override;
    IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap *) override;
    IOReturn getLLW_PARAMS(apple80211_llw_params *) override;

    // IO80211AWDLProtocol: setters
    IOReturn setSSID(apple80211_ssid_data *) override;
    IOReturn setCHANNEL(apple80211_channel_data *) override;
    IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *) override;
    IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *) override;
    IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *) override;
    IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id *) override;
    IOReturn setAWDL_BSSID(apple80211_awdl_bssid *) override;
    IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *) override;
    IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *) override;
    IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *) override;
    IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state *) override;
    IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *) override;
    IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *) override;
    IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control *) override;
    IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *) override;
    IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *) override;
    IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *) override;
    IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration *) override;
    IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *) override;
    IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *) override;
    IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate *) override;
    IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key *) override;
    IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt *) override;
    IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *) override;
    IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period *) override;
    IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add *) override;
    IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config *) override;
    IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config *) override;
    IOReturn setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create *) override;
    IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request *) override;
    IOReturn setLLW_PARAMS(apple80211_llw_params *) override;
    IOReturn setTX_RATE(apple80211_tx_rate *) override;
    IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection *) override;
    IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params *) override;
    IOReturn setAWDL_CONFIG(apple80211_awdl_config_request *) override;
    IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg *) override;

    // Hand a received AWDL action frame (802.11 header included) to IO80211's
    // AWDL peer manager, packaged like AppleBCMWLANProximityInterface does.
    // Returns 1 when the frame was AWDL and has been consumed.
    int inputActionFrame(const uint8_t *frame, size_t len, const ieee80211_rxinfo *rxi);
    uint64_t rxActionFrames() const { return fRxAF; }
    // Stop and remove the AW timer; call before the interface is stopped.
    void teardownAW();

private:
    // Host-side availability window timeline (what Broadcom firmware does with
    // its AW start/end events). One "slot" is one channel sequence entry:
    // presence_mode AWs of aw_period TU each; times are host microseconds.
    static void awTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void startAWTimeline();
    void stopAWTimeline();
    void scheduleNextAW(uint64_t nowUs);
    void syncFromFrame(const uint8_t *af, size_t len, const uint8_t *src, uint64_t rxUs);
    void postAWEvents();
    uint64_t slotUs() const;
    uint16_t presenceMode() const;
    bool isSyncMaster() const;
    void publishSyncState();

    IOTimerEventSource *fAWTimer;
    bool fAWRunning;
    int64_t fRefUs;            // a slot boundary on the current timeline
    uint16_t fRefSeq;          // AW sequence number at fRefUs
    bool fLastValid;
    int64_t fLastStartUs;
    uint16_t fLastSeq;
    uint64_t fAWStarts;
    uint64_t fSyncUpdates;
    int64_t fLastSyncErrUs;
    ether_addr fSyncMaster;    // from setAWDL_SYNC_STATE; zero = none programmed
    uint32_t fSyncMode;
    uint32_t fPresenceMode;    // from setAWDL_PRESENCE_MODE, 0 = not set
    bool fInAW;                // an AW start was posted without its end
    uint32_t fTsfOffset;       // host_us - rx TSF (mod 2^32), min-latency estimate
    bool fTsfValid;
    uint32_t fSyncOutliers;      // consecutive large sync errors ignored
    uint64_t fSyncOutliersTotal;
    void maybeListen(uint32_t slotChannel, uint16_t seq);
    uint32_t fListenSlot;
    uint64_t fListenStarted;
    uint64_t fListenBusy;
    uint64_t fListenSkippedHome;
    uint32_t fListenDwellTU;        // AWDLListenDwellTU
    uint32_t fListenEveryNth;       // AWDLListenEveryNth: 1 = every eligible slot
    // Peer timeline, so a listen can start when the peer is on the channel.
    static void listenTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void trackPeer(const uint8_t *af, size_t len, const uint8_t *src, uint64_t rxUs, unsigned rxChan);
    bool parsePeerChanSeq(const uint8_t *v, size_t len);
    unsigned peerChannelAt(uint64_t us) const;
    void scheduleAlignedListen(uint64_t afterUs);
    // -itlawdldata: dwell on our own channel sequence (what we advertise), as
    // Broadcom firmware does, instead of chasing the peer's sequence.
    bool scheduleOwnListen(uint64_t afterUs);
    uint8_t ownChannelAt(uint16_t seq) const;
    uint64_t fListenOwn;            // own-sequence dwells started
    uint64_t fBssidSets;
    uint64_t fPeerCacheOps;
    uint64_t fDataTxNotCommon;      // data held: peer not on our channel now
    void startListen(unsigned chan, uint32_t dwellTU, unsigned home, bool aligned);
    IOTimerEventSource *fListenTimer;
    uint32_t fListenAlign;          // AWDLListenAlign
    bool fListenAlignedActive;      // the listen timer drives the listens
    uint8_t fListenNextChan;
    uint32_t fListenNextDwellTU;
    uint64_t fListenAligned;
    uint64_t fListenNoSlot;
    bool fPeerValid;
    ether_addr fPeerMac;
    uint64_t fPeerSeenUs;
    int64_t fPeerRefUs;             // a slot boundary of the peer
    uint16_t fPeerRefSeq;
    uint16_t fPeerPm;
    uint32_t fPeerSlotUs;
    uint8_t fPeerChan[16];
    bool fPeerChanValid;
    uint64_t fPeerUpdates;
    uint64_t fPeerPredHit;
    uint64_t fPeerPredMiss;
    int64_t fPeerLastErrUs;
    uint64_t fRxAFChan6;
    uint64_t fRxAFChan44;
    uint64_t fRxAFChan149;
    uint64_t fRxAFChanOther;
    uint64_t fListenPeerSlots;      // dwells scheduled on the peer's channel in our home/off slots
    // Per transmitter: when its last AWDL action frame arrived and the longest
    // gap so far. IO80211Family drops a peer from NDP after ~6 s without one
    // (shouldRemovePeerFromNDP, lastAF), which kills a running AirDrop.
    struct RxGap { uint8_t mac[6]; uint64_t lastUs; uint64_t maxGapUs; uint32_t count; uint32_t over6s; };
    enum { kRxGapSlots = 8 };
    RxGap fRxGap[kRxGapSlots];
    void noteRxGap(const uint8_t *src, uint64_t nowUs);
    uint64_t fRxAFChan161;          // also counted in Other
    uint64_t fListenOnChan[4];      // listens started on 6, 44, 149, 161
    void publishListenState();
    uint64_t hostRxUs(const ieee80211_rxinfo *rxi);
    // Stage 3: transmit payloads handed down by the peer manager (recorded only).
    void recordOOB(const void *req, bool autoReq);
    uint8_t fSyncTmpl[512];         // PSF payload TLVs (SYNC_FRAME_TEMPLATE)
    uint16_t fSyncTmplLen;
    uint64_t fSyncTmplSets;
    uint8_t fMITmpl[1200];          // MI payload TLVs (OOB_AUTO_REQUEST)
    uint16_t fMITmplLen;
    uint64_t fOOBSets;
    uint64_t fOOBAutoSets;
    uint32_t fAfTxMode;
    // Stage 3 step 1 (-itlawdltx): send a PSF and an MI in the peer's AW
    // during each aligned listen dwell.
    static void txTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void scheduleTx(uint8_t chan, uint64_t slotStartUs);
    bool sendAWDLFrame(bool mi);
    // TX pump (-itlawdltxpump): poll the firmware dwell during the slot and send
    // PSF / MI / queued data whenever the dwell actually runs and the peer is there.
    void pumpTick();
    bool pumpCanSend(uint8_t *dwellChan, int *dwellState);
    void noteRocStart();
    int64_t listenLeadUs() const { return fListenLeadUs; }
    uint32_t dwellExtraTU() const { return fDwellExtraTU; }
    bool homeTxAllowed() const { return fHomeTx; }
    // Behaviour switches (boot-args, also writable at run time via setProperties).
    bool fTxPump;                   // -itlawdltxpump
    bool fHomeTx;                   // -itlawdlhometx
    uint32_t fPumpUs;               // itlawdlpumpus=
    int64_t fListenLeadUs;          // itlawdllead= (default 6000)
    uint32_t fDwellExtraTU;         // itlawdldwellx=
    // Pump state and diagnostics.
    uint64_t fTxReqUs;              // when the dwell for fTxSlotUs was requested
    uint64_t fTxPumpEndUs;
    bool fTxPumpPsfDone, fTxPumpMiDone, fTxPumpSawRun, fTxStartNoted;
    uint64_t fPumpTicks, fPumpWindows, fPumpNoRun, fPumpPeerAway, fPumpImmediate, fPumpLateAF;
    uint32_t fRocStartHist[8];      // dwell START - planned slot start
    uint32_t fDataLatHist[8];       // enqueue -> handed to firmware
    uint64_t fDataLatMaxUs;
    uint64_t fDataQUs[256];         // enqueue time of fDataQ[i] (kDataQueueLen)
    // Peer alignment (see docs/research/airdrop-master-election-and-multicast.md). When we are
    // the sync master the tracked peer should be on our timeline; these record
    // whether it is, and -itlawdlfollow adopts the peer's timeline when not.
    bool fFollowPeer;               // -itlawdlfollow / AWDLFollowPeerOn
    ether_addr fPeerMaster;         // master address in the peer's sync parameters
    int64_t fPeerOwnErrUs;          // peer slot boundary - ours (nearest)
    uint16_t fPeerSlotOff;          // peer channel-sequence index - ours, mod 16
    uint32_t fFollowStreak;         // consecutive misaligned frames of the tracked peer
    uint64_t fPeerAlignedCnt, fPeerMisalignedCnt, fFollowAdopts;
    uint64_t fPeerFollowsUsMis;     // misaligned although the peer names us as its master
    uint64_t fPeerGateStale;        // TX not held back: tracked peer not heard recently
    uint64_t fPeerSwitchData;       // tracked peer replaced by the peer we exchange data with
    ether_addr fDataPeerMac;        // last unicast data peer (either direction)
    uint64_t fDataPeerUs;
    unsigned peerGateChannel(uint64_t us);
    // Group-addressed vs unicast data (mDNS / ND vs TCP): separate counters.
    uint8_t fDataQMc[256];          // fDataQ[i] is group-addressed
    uint64_t fMcQueued, fMcTx;
    uint32_t fMcLatHist[8];         // same buckets as fDataLatHist
    uint64_t fGateHeldTicks;        // pump ticks with the dwell running, data queued, peer away
    uint64_t fGateHeldMcTicks;      // ... and the head of the queue was group-addressed
    uint64_t fDataTxDropStale;      // frames older than kDataMaxAgeUs dropped instead of sent
    void publishTxDiag();
    size_t buildAWDLFrame(uint8_t *buf, size_t cap, bool mi, uint64_t nowUs);
    void publishTxState();
    void publishAWDLAddress();
public:
    // Stage 4 datapath counters, updated by the BSD interface.
    uint64_t fDataRx;
    uint64_t fDataRxBad;
    uint64_t fDataRxAmsdu;
    uint64_t fDataRxNoBuf;
    uint64_t fDataRxMc;             // group-addressed frames delivered to awdl0
    uint32_t fDataRxLastLen;
    uint16_t fDataRxLastType;
    void publishDataState();
    // Ethernet frames from awdl0, sent as AWDL data frames during a dwell.
    void enqueueData(mbuf_t m);
    // Unicast data to/from this station: it is the peer whose timeline matters.
    void noteDataPeer(const uint8_t *mac);
    void flushData();
    // 32 overflowed during AirDrop (AWDLDataTxDropFull): data only
    // leaves in the peer's slots, so a burst from the stack has to wait.
    enum { kDataQueueLen = 256 };
    mbuf_t fDataQ[kDataQueueLen];
    unsigned fDataQHead;
    unsigned fDataQCount;
    uint16_t fDataSeq;
    uint64_t fDataTx;
    uint64_t fDataTxQueued;
    uint64_t fDataTxDropFull;
    uint64_t fDataTxDeferred;
    uint64_t fDataTxErr;
    void noteDataSent(unsigned idx);
private:
    IOTimerEventSource *fTxTimer;
    uint8_t fTxChan;
    uint8_t fTxPhase;               // 0 idle, 1 PSF next, 2 MI next
    uint64_t fTxSlotUs;
    uint16_t fTxSeq;                // 802.11 sequence number
    uint32_t fMasterChannel;        // setAWDL_MASTER_CHANNEL
    uint64_t fTxPSF;
    uint64_t fTxMI;
    uint64_t fTxRefused;            // driver refused (not dwelling, ring full)
    uint64_t fTxSkipMode;           // AF tx mode said suppress
    uint32_t fTxLastErr;
    uint32_t fTxLastLen;
    uint8_t fChanSeq[400];     // raw apple80211_awdl_sync_channel_sequence
    bool fChanSeqValid;

    uint64_t fRxAF;
    uint64_t fRxAFDropped;
    uint64_t fRxAFNoInject;    // AFs not handed to IO80211 because of -itlawdlnoinject
    AirportItlwm *fController;
    // The HAL's ieee80211com, or NULL once the controller dropped its HAL
    // (AirportItlwm::releaseAll) or before init.
    struct ieee80211com *halIc() const;
    ether_addr fMac;
    // Cached configuration, returned by the matching getters.
    apple80211_awdl_sync_params fSyncParams;
    bool fSyncEnabled;
    uint32_t fSyncState;
    uint64_t fCallCount;
};

#endif /* AIRPORT_AWDL */
#endif /* AirportItlwmAWDLInterface_hpp */
