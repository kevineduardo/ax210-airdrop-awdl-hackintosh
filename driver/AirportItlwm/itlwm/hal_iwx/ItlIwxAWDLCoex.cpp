// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL coex backend, -itlawdlcoex).
// See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
// Ported from ForbiddenItlwm (retropineaple), commit a5f1eb9, itlwm/hal_iwx/ItlIwx.cpp, GPL-2.0.
//
//  ItlIwxAWDLCoex.cpp
//
//  The AWDL coex listener engine (see ItlIwxAWDLCoex.hpp). Not a separate compile unit: ItlIwx.cpp
//  includes it once at its end, inside #ifdef AIRPORT_WCL, so it shares ItlIwx.cpp's statics and
//  macros. It is not in the Xcode project's source list on purpose.
//

#ifndef ITLIWX_AWDLCOEX_FROM_ITLIWX
#error "ItlIwxAWDLCoex.cpp is included by ItlIwx.cpp; do not compile it on its own"
#endif

static uint64_t awdlUptimeUS()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

static uint32_t mccUptimeSeconds()
{
    return uint32_t(awdlUptimeUS() / 1000000ULL);
}

static void awdlStopTimers()
{
    // Set first; every arm site checks it inside the command gate, and the frees below run in the gate,
    // so work that was already past its check finishes, and its timer is freed after it.
    awdlStopping = true;
    if (awdlCoexTxTimer)
        timeout_free(&awdlCoexTxTimer);
    task_del(systq, &awdlCoexTxTaskEntry);
}

// The listener's channel, width and the operating class our frames should advertise, for the AWDL
// interface (ic_awdl_coex_*). 80 MHz is advertised only while the listener's own context really is at
// 80 MHz on 149 and no move is pending (ForbiddenItlwm's itlAWDLChannelClass).
static void awdlCoexShareChannel()
{
    struct ieee80211com *ic = awdlCoexIc;
    if (!ic)
        return;
    const bool on = awdlCoexAdded && awdlCoexChannel && awdlCoexChannel <= 255;
    ic->ic_awdl_coex_chan = on ? uint8_t(awdlCoexChannel) : 0;
    ic->ic_awdl_coex_width = on && awdlCoexMultiActive && awdlCoexOwnWidth == 80 ? 80 : 20;
    ic->ic_awdl_coex_class = on && awdlCoexMultiActive && awdlCoexOwnWidth == 80 && !awdlCoexMovePending &&
        awdlFixedChannel == 149 ? 128 : 0;
}

static void awdlCoexPublish()
{
    awdlCoexShareChannel();
    IOEthernetController *root = awdlCoexCtl;
    if (!root || !awdlCoexProbe)
        return;
    root->setProperty("ITLAWDLCoexAdded", awdlCoexAdded);
    root->setProperty("ITLAWDLCoexAdds", awdlCoexAdds, 32);
    root->setProperty("ITLAWDLCoexRemoves", awdlCoexRemoves, 32);
    root->setProperty("ITLAWDLCoexAddsAfterRemove", awdlCoexAddsAfterRemove, 32);
    root->setProperty("ITLAWDLCoexAddsAfterRestart", awdlCoexAddsAfterRestart, 32);
    root->setProperty("ITLAWDLCoexFirmwareGeneration", uint32_t(awdlCoexGeneration), 32);
    root->setProperty("ITLAWDLCoexCountryUnusableAnswers", awdlCoexCountryUnusableAnswers, 32);
    root->setProperty("ITLAWDLCoexHintClears", awdlCoexHintClears, 32);
    if (auto *seq = OSData::withBytes(awdlCoexPeerSeq, sizeof(awdlCoexPeerSeq))) {
        root->setProperty("ITLAWDLCoexPeerSequence", seq);
        seq->release();
    }
    root->setProperty("ITLAWDLCoexPeerSequenceFrames", awdlCoexPeerSeqFrames, 32);
    root->setProperty("ITLAWDLCoexPeerSequenceUpdates", awdlCoexPeerSeqChanges, 32);
    root->setProperty("ITLAWDLCoexMacResult", awdlCoexMacResult, 32);
    root->setProperty("ITLAWDLCoexBindResult", awdlCoexBindResult, 32);
    root->setProperty("ITLAWDLCoexBindStatus", awdlCoexBindStatus, 32);
    root->setProperty("ITLAWDLCoexChannel", awdlCoexChannel, 32);
    root->setProperty("ITLAWDLCoexAwdlAction", awdlCoexAwdlAction, 32);
    root->setProperty("ITLAWDLCoexAwdlData", awdlCoexAwdlData, 32);
    root->setProperty("ITLAWDLCoexOther", awdlCoexOther, 32);
    root->setProperty("ITLAWDLCoexFilter", awdlCoexFilter, 32);
    root->setProperty("ITLAWDLCoexActionOnChannel", awdlCoexActionOnChannel, 32);
    root->setProperty("ITLAWDLCoexActionOffChannel", awdlCoexActionOffChannel, 32);
    root->setProperty("ITLAWDLCoexAddrChanges", awdlCoexAddrChanges, 32);
    root->setProperty("ITLAWDLCoexAddrFailures", awdlCoexAddrFailures, 32);
    root->setProperty("ITLAWDLCoexAddrApplied", awdlCoexAddrApplied);
    root->setProperty("ITLAWDLCoexTeardownErrors", awdlCoexTeardownErrors, 32);
    root->setProperty("ITLAWDLCoexBroken", awdlCoexBrokenGeneration >= 0 && awdlCoexBrokenGeneration == awdlCoexGeneration);
    if (awdlCoexDataRxOn) {
        root->setProperty("ITLAWDLCoexDataRx", awdlCoexDataRx, 32);
        root->setProperty("ITLAWDLCoexDataRxHandedOff", awdlCoexDataRxDelivered, 32);
        root->setProperty("ITLAWDLCoexDataRxRefused", awdlCoexDataRxRefused, 32);
        root->setProperty("ITLAWDLCoexDataRxNotIPv6", awdlCoexDataRxNotIPv6, 32);
        root->setProperty("ITLAWDLCoexDataRxUnknownPeer", awdlCoexDataRxUnknownPeer, 32);
        root->setProperty("ITLAWDLCoexDataRxOffChannel", awdlCoexDataRxOffChannel, 32);
        root->setProperty("ITLAWDLCoexDataRxRetry", awdlCoexDataRxRetry, 32);
        root->setProperty("ITLAWDLCoexDataRxUcast", awdlCoexDataRxUcast, 32);
        root->setProperty("ITLAWDLCoexDataRxGroup", awdlCoexDataRxGroup, 32);
        root->setProperty("ITLAWDLCoexRxWide80", awdlCoexRxWide80, 32);
        if (auto *mods = OSData::withBytes(awdlCoexRxModulation, sizeof(awdlCoexRxModulation))) {
            root->setProperty("ITLAWDLCoexRxModulation", mods);
            mods->release();
        }
        if (auto *reasons = OSData::withBytes(awdlCoexRejectReasons, sizeof(awdlCoexRejectReasons))) {
            root->setProperty("ITLAWDLCoexRejectReasons", reasons);
            reasons->release();
        }
    }
    root->setProperty("ITLAWDLCoexPromiscLive", awdlCoexPromiscLive);
    root->setProperty("ITLAWDLCoexPromiscDropped", awdlCoexPromiscDropped, 32);
    root->setProperty("ITLAWDLCoexPromiscUncertain", awdlCoexPromiscUncertain);
    root->setProperty("ITLAWDLCoexPromiscAmbiguous", awdlCoexPromiscAmbiguous, 32);
    if (auto *masks = OSData::withBytes(awdlCoexMacMaskSeen, sizeof(awdlCoexMacMaskSeen))) {
        root->setProperty("ITLAWDLCoexMacMaskSeen", masks);
        masks->release();
    }
    if (auto *seen = OSData::withBytes(awdlCoexBaActionSeen, sizeof(awdlCoexBaActionSeen))) {
        root->setProperty("ITLAWDLCoexBaActionSeen", seen);
        seen->release();
    }
    if (awdlCoexRx) {
        root->setProperty("ITLAWDLCoexForwarded", awdlCoexForwarded, 32);
        root->setProperty("ITLAWDLCoexZeroPadTrimmed", awdlCoexZeroPadTrimmed, 32);
        root->setProperty("ITLAWDLCoexMultiState", awdlCoexMultiState);
        root->setProperty("ITLAWDLCoexPhyResult", awdlCoexPhyResult, 32);
        root->setProperty("ITLAWDLCoexMultiBindResult", awdlCoexMultiBindResult, 32);
        root->setProperty("ITLAWDLCoexOwnWidth", uint32_t(awdlCoexOwnWidth), 32);
        root->setProperty("ITLAWDLCoexMoves", awdlCoexMoves, 32);
        root->setProperty("ITLAWDLCoexMoveFailures", awdlCoexMoveFailures, 32);
        root->setProperty("ITLAWDLCoexCountryNotices", awdlCoexCountryNotices, 32);
        root->setProperty("ITLAWDLCoexCountryAnswers", awdlCoexCountryAnswers, 32);
        root->setProperty("ITLAWDLCoexCountryLastStatus", awdlCoexCountryLastStatus, 32);
        root->setProperty("ITLAWDLCoexCountryLearned", awdlCoexCountryLearned);
        root->setProperty("ITLAWDLCoexBeaconHints", awdlCoexBeaconHints, 32);
        root->setProperty("ITLAWDLCoexCountryFailures", awdlCoexCountryFailures, 32);
        root->setProperty("ITLAWDLCoexMoveDrainTimeouts", awdlCoexMoveDrainTimeouts, 32);
        root->setProperty("ITLAWDLCoexMovePending", awdlCoexMovePending);
        root->setProperty("ITLAWDLCoexForwardRefused", awdlCoexForwardRefused, 32);
        root->setProperty("ITLAWDLCoexForwardMalformed", awdlCoexForwardMalformed, 32);
    }
    if (awdlCoexTx) {
        root->setProperty("ITLAWDLCoexStaResult", awdlCoexStaResult, 32);
        root->setProperty("ITLAWDLCoexQueueResult", awdlCoexQueueResult, 32);
        root->setProperty("ITLAWDLCoexQueueStage", awdlCoexQueueStage);
        root->setProperty("ITLAWDLCoexQueueAssigned", uint32_t(awdlCoexQueueAssigned), 32);
        root->setProperty("ITLAWDLCoexQueueWritePointer", awdlCoexQueueWritePointer, 32);
        root->setProperty("ITLAWDLCoexSetupAttempts", awdlCoexSetupAttempts, 32);
        root->setProperty("ITLAWDLCoexTxHalted", awdlCoexTxHalted);
        root->setProperty("ITLAWDLCoexSetupAborts", awdlCoexSetupAborts, 32);
        root->setProperty("ITLAWDLCoexBadCompletions", awdlCoexBadCompletions, 32);
        root->setProperty("ITLAWDLCoexLastSlot", uint32_t(awdlCoexLastSlot), 32);
        root->setProperty("ITLAWDLCoexLastBoundary", uint32_t(awdlCoexLastBoundary), 32);
        root->setProperty("ITLAWDLCoexLastSsn", awdlCoexLastSsn, 32);
        if (awdlCoexFaultSnapshot[0])
            root->setProperty("ITLAWDLCoexFault", awdlCoexFaultSnapshot);
        root->setProperty("ITLAWDLCoexOrphanQueue", uint32_t(awdlCoexOrphanQueue), 32);
        root->setProperty("ITLAWDLCoexTxBuilt", awdlCoexTxBuilt, 32);
        root->setProperty("ITLAWDLCoexTxNotReady", awdlCoexTxNotReady, 32);
        root->setProperty("ITLAWDLCoexTxBuildRejected", awdlCoexTxBuildRejected, 32);
        root->setProperty("ITLAWDLCoexRefusedReason", awdlCoexRefusedReason, 32);
        root->setProperty("ITLAWDLCoexRefusedChannelFlags", awdlCoexRefusedChannelFlags, 32);
        if (awdlCoexRefusalSnapshot[0])
            root->setProperty("ITLAWDLCoexLastRefusal", awdlCoexRefusalSnapshot);
        root->setProperty("ITLAWDLCoexTxSubmitted", awdlCoexTxSubmitted, 32);
        root->setProperty("ITLAWDLCoexTxSubmitErrors", awdlCoexTxSubmitErrors, 32);
        root->setProperty("ITLAWDLCoexTxOK", awdlCoexTxOK, 32);
        root->setProperty("ITLAWDLCoexTxFailed", awdlCoexTxFailed, 32);
        root->setProperty("ITLAWDLCoexTxStuck", awdlCoexTxStuck, 32);
        root->setProperty("ITLAWDLCoexTxPauses", awdlCoexTxPauses, 32);
        root->setProperty("ITLAWDLCoexWedgedRebuilds", awdlCoexWedgedRebuilds, 32);
        root->setProperty("ITLAWDLCoexTxRefusedChannel", awdlCoexTxRefusedChannel, 32);
        root->setProperty("ITLAWDLCoexLateCompletions", awdlCoexLateCompletions, 32);
        root->setProperty("ITLAWDLCoexIdleSyncFrames", awdlCoexIdleSyncFrames, 32);
        root->setProperty("ITLAWDLCoexStatusIdxHits", awdlCoexStatusIdxHits, 32);
        root->setProperty("ITLAWDLCoexStatusIdxMisses", awdlCoexStatusIdxMisses, 32);
        root->setProperty("ITLAWDLCoexMultiReclaims", awdlCoexMultiReclaims, 32);
        root->setProperty("ITLAWDLCoexReclaimUnknown", awdlCoexReclaimUnknown, 32);
        root->setProperty("ITLAWDLCoexSyncReclaimedUnknown", awdlCoexSyncReclaimedUnknown, 32);
    }
    if (awdlCoexData) {
        root->setProperty("ITLAWDLCoexDataTx", awdlCoexDataTx, 32);
        root->setProperty("ITLAWDLCoexDataTxErrors", awdlCoexDataTxErrors, 32);
        root->setProperty("ITLAWDLCoexDataOK", awdlCoexDataOK, 32);
        root->setProperty("ITLAWDLCoexDataFailed", awdlCoexDataFailed, 32);
        root->setProperty("ITLAWDLCoexDataPumps", awdlCoexDataPumps, 32);
        root->setProperty("ITLAWDLCoexUcastOK", awdlCoexUcastOK, 32);
        root->setProperty("ITLAWDLCoexUcastFailed", awdlCoexUcastFailed, 32);
        root->setProperty("ITLAWDLCoexGroupOK", awdlCoexGroupOK, 32);
        root->setProperty("ITLAWDLCoexGroupFailed", awdlCoexGroupFailed, 32);
        root->setProperty("ITLAWDLCoexRateStep", uint32_t(awdlCoexRateStep), 32);
        root->setProperty("ITLAWDLCoexRateUps", awdlCoexRateUps, 32);
        root->setProperty("ITLAWDLCoexRateDowns", awdlCoexRateDowns, 32);
        root->setProperty("ITLAWDLCoexVhtFrames", awdlCoexVhtFrames, 32);
        root->setProperty("ITLAWDLCoexGate", awdlCoexGate);
        root->setProperty("ITLAWDLCoexGateHolds", awdlCoexGateHolds, 32);
        root->setProperty("ITLAWDLCoexGateAwayTicks", awdlCoexGateAwayTicks, 32);
        root->setProperty("ITLAWDLCoexGateDozeTicks", awdlCoexGateDozeTicks, 32);
        root->setProperty("ITLAWDLCoexGateForced", awdlCoexGateForced, 32);
        root->setProperty("ITLAWDLCoexAssumeZero", awdlCoexAssumeZero);
        root->setProperty("ITLAWDLCoexScheduleKnown", awdlCoexScheduleCurrent() && awdlCoexPeerSchedule.known() &&
                          awdlCoexPeerSchedule.fresh(awdlUptimeUS()));
        root->setProperty("ITLAWDLCoexScheduleCalibrated", awdlCoexScheduleCalibrated);
        root->setProperty("ITLAWDLCoexScheduleRefutations", awdlCoexPeerSchedule.refutations, 32);
        root->setProperty("ITLAWDLCoexPeerPresentEntries", awdlCoexPeerSchedule.presentEntries(), 32);
        root->setProperty("ITLAWDLCoexPeerSyncFrames", awdlCoexPeerSyncFrames, 32);
        root->setProperty("ITLAWDLCoexPeerSequenceRejected", awdlCoexPeerSequenceRejected, 32);
        root->setProperty("ITLAWDLCoexPeerSequenceChanges", awdlCoexPeerSchedule.sequenceChanges, 32);
        root->setProperty("ITLAWDLCoexPeerDecalibrations", awdlCoexPeerSchedule.decalibrations, 32);
    }
    root->setProperty("ITLAWDLCoexReclaimRefused", awdlReclaimRefused, 32);
}

// Passive (no initiating) for the listener: the NVM's passive flag, unless a beaconing AP was heard on that
// channel (net80211's beacon hint, cleared when the country changes). Radar and no-ad-hoc are checked apart.
static inline bool awdlCoexNoIr(const struct ieee80211com *ic, unsigned ch)
{
    return ch <= IEEE80211_CHAN_MAX && (ic->ic_channels[ch].ic_flags & IEEE80211_CHAN_PASSIVE) &&
        !isset(ic->ic_beacon_hint, ch);
}

static void awdlCoexPeerHeard(const uint8_t *addr, uint64_t nowUS)
{
    AwdlCoexPeer *slot = &awdlCoexPeers[0];
    for (auto &p : awdlCoexPeers) {
        if (!memcmp(p.addr, addr, 6)) { slot = &p; break; }
        if (p.heardUS < slot->heardUS) slot = &p;
    }
    memcpy(slot->addr, addr, 6);
    slot->heardUS = nowUS;
    slot->channel = awdlCoexChannel;
    slot->token = awdlCoexToken;
}
static void awdlCoexNoteRejectReason(const uint8_t *addr, uint64_t nowUS)
{
    unsigned reason = 0;
    for (const auto &p : awdlCoexPeers) {
        if (!p.heardUS || memcmp(p.addr, addr, 6))
            continue;
        reason = nowUS - p.heardUS >= 60000000ULL ? 1 : p.channel != awdlCoexChannel ? 2 : 3;
        break;
    }
    ++awdlCoexRejectReasons[reason];
}
static bool awdlCoexPeerKnown(const uint8_t *addr, uint64_t nowUS, bool refresh = false)
{
    for (auto &p : awdlCoexPeers) {
        // Previously discovered on this channel by this listener. Valid directed
        // traffic from the established BA peer can renew an existing entry.
        if (p.heardUS && !memcmp(p.addr, addr, 6) && nowUS - p.heardUS < 60000000ULL &&
            p.channel == awdlCoexChannel && p.token == awdlCoexToken) {
            if (refresh)
                p.heardUS = nowUS;
            return true;
        }
    }
    return false;
}
static bool awdlCoexPeerSequence(const uint8_t *frame, size_t length, uint8_t out[16])
{
    if (!frame || length < 40) return false;
    for (size_t i = 40; i + 3 <= length;) {
        const size_t value = size_t(frame[i + 1]) | size_t(frame[i + 2]) << 8;
        if (i + 3 + value > length) return false;
        if (frame[i] == 18) {
            if (value < 6 + 32) return false;
            for (unsigned n = 0; n < 16; ++n) out[n] = frame[i + 3 + 6 + 2 * n];
            return true;
        }
        i += 3 + value;
    }
    return false;
}

// Called from iwx_attach before the AWDL callbacks are installed. The front end decided (ic_awdl_coex_on).
void ItlIwx::
awdlCoexAttach(struct iwx_softc *sc)
{
    // A new attach in the same loaded image starts with its timers allowed again.
    awdlStopping = false;
    awdlCoexSelected = sc->sc_ic.ic_awdl_coex_on != 0;
    awdlCoexFwPublishedGen = -1;
    awdlCoexIc = &sc->sc_ic;
    awdlCoexCtl = getController();
    if (!awdlCoexSelected)
        return;
    XYLog("%s: AWDL coex backend selected; P2P listen path disabled\n", DEVNAME(sc));
    if (awdlCoexCtl)
        awdlCoexCtl->setProperty("ITLAWDLCoexSelected", true);
    int dummy;
    // Phase 1: the listener on its own channel (DYNAMIC_QUOTA), the firmware's country answered, and
    // every AWDL action and data frame it hears handed to awdl0. Nothing is sent from it yet.
    awdlCoexProbe = true;
    awdlCoexRx = true;
    awdlCoexDataRxOn = true;
    // Phase 2: the listener's own station and queue, macOS's sync frames every 45 to 100 ms, awdl0's
    // data one frame at a time with the rate ladder, and the send gate.
    awdlCoexTx = true;
    awdlCoexData = true;
    awdlCoexGate = !PE_parse_boot_argn("-itlawdlcoexnogate", &dummy, sizeof(dummy));
    awdlCoexAssumeZero = !PE_parse_boot_argn("-itlawdlcoexcalwait", &dummy, sizeof(dummy));
    awdlCoexPeerSchedule.assumeZero = awdlCoexAssumeZero;
    awdlCoexShared = PE_parse_boot_argn("-itlawdlcoexshared", &dummy, sizeof(dummy));
    awdlCoexMulti = !awdlCoexShared;
    awdlCoexSameBand = PE_parse_boot_argn("-itlawdlsameband", &dummy, sizeof(dummy));
    // Data needs promiscuous receive on the listener's own channel; see awdlCoexMacSend.
    awdlCoexFilter = 3;
    awdlCoexMultiState = awdlCoexMulti ? "not-added" : "off";
    task_set(&awdlCoexTxTaskEntry, awdlCoexTxTask, sc, "awdlCoexTxTask");
    timeout_set(&awdlCoexTxTimer, awdlCoexTxTimeout, sc);
    timeout_add_msec(&awdlCoexTxTimer, kAwdlCoexTxPeriodMS);
    awdlCoexPublish();
}

// Phase 0 of the port: what this firmware offers the engine, published once per firmware start, before
// the engine sends anything. ForbiddenItlwm needs DYNAMIC_QUOTA for the listener's own channel, TX_CMD
// above version 8 for its fixed VHT rates, TLC_MNG_CONFIG version 4 and ADD_STA 12 or later.
void ItlIwx::
awdlCoexPublishFirmware(struct iwx_softc *sc)
{
    if (!awdlCoexSelected || awdlCoexFwPublishedGen == sc->sc_generation)
        return;
    awdlCoexFwPublishedGen = sc->sc_generation;
    IOEthernetController *ctl = getController();
    if (!ctl)
        return;
    ctl->setProperty("ITLAWDLCoexFwDynamicQuota", isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA) != 0);
    ctl->setProperty("ITLAWDLCoexFwCdb", isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_CDB_SUPPORT) != 0);
    static const struct { const char *key; uint8_t group, cmd; } kVersions[] = {
        {"ITLAWDLCoexFwVerMacContext", IWX_LONG_GROUP, IWX_MAC_CONTEXT_CMD},
        {"ITLAWDLCoexFwVerPhyContext", IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD},
        {"ITLAWDLCoexFwVerBinding", IWX_LONG_GROUP, IWX_BINDING_CONTEXT_CMD},
        {"ITLAWDLCoexFwVerAddSta", IWX_LONG_GROUP, IWX_ADD_STA},
        {"ITLAWDLCoexFwVerScdQueueCfg", IWX_LONG_GROUP, IWX_SCD_QUEUE_CFG},
        {"ITLAWDLCoexFwVerTxCmd", IWX_LONG_GROUP, IWX_TX_CMD},
        {"ITLAWDLCoexFwVerTlcConfig", IWX_DATA_PATH_GROUP, IWX_TLC_MNG_CONFIG_CMD},
        {"ITLAWDLCoexFwVerMccUpdate", IWX_LONG_GROUP, IWX_MCC_UPDATE_CMD},
    };
    // 99 (IWX_FW_CMD_VER_UNKNOWN): the firmware's table doesn't list the command.
    for (const auto &v : kVersions)
        ctl->setProperty(v.key, iwx_lookup_cmd_ver(sc, v.group, v.cmd), 32);
    ctl->setProperty("ITLAWDLCoexFwValidTxAnt", iwx_fw_valid_tx_ant(sc), 32);
    ctl->setProperty("ITLAWDLCoexFwValidRxAnt", iwx_fw_valid_rx_ant(sc), 32);
    ctl->setProperty("ITLAWDLCoexFwGeneration", uint32_t(sc->sc_generation), 32);
}

// ---------------------------------------------------------------------------------------------------
// Phase 1: the listener, its channel, the firmware's country, receive.
// ---------------------------------------------------------------------------------------------------

// The one channel check for the listener's sending, used before building and again just before sending.
// The listener's own context at 80 MHz covers 149-161: every one of them has to stay allowed, not only the
// primary, and the move has to be complete.
static bool awdlCoexBlockIneligible(struct ieee80211com *ic, struct ieee80211_channel *channel)
{
    if (awdlCoexMovePending)
        return true;
    if (!channel || channel != awdlCoexOwnChan || awdlCoexOwnWidth != 80)
        return false;
    if (!(ic->ic_channels[149].ic_flags & IEEE80211_CHAN_VHT80))
        return true;
    for (unsigned ch = 149; ch <= 161; ch += 4) {
        const auto &sub = ic->ic_channels[ch];
        if (!sub.ic_freq || !isset(ic->ic_chan_active, ch) || awdlCoexNoIr(ic, ch) ||
            (sub.ic_flags & (IEEE80211_CHAN_DFS | IEEE80211_CHAN_NOADHOC)))
            return true;
    }
    return false;
}

void ItlIwx::
awdlCoexTxTimeout(void *arg)
{
    if (!awdlStopping)
        task_add(systq, &awdlCoexTxTaskEntry);
}

// Sends the listener's MAC context command: add, modify or remove. Its address is awdl0's once the AWDL
// interface set it (ic_awdl_addr), before that a locally administered one, never the station's.
int ItlIwx::
awdlCoexMacSend(struct iwx_softc *sc, uint32_t action)
{
    struct ieee80211com *ic = &sc->sc_ic;
    struct iwx_node *in = (struct iwx_node *)ic->ic_bss;
    struct iwx_mac_ctx_cmd mac;
    memset(&mac, 0, sizeof(mac));
    mac.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kAwdlCoexMacId, 0));
    mac.action = htole32(action);
    bool promiscWanted = false;
    if (action != IWX_FW_CTXT_ACTION_REMOVE) {
        mac.mac_type = htole32(IWX_FW_MAC_TYPE_LISTENER);
        mac.tsf_id = htole32(IWX_TSF_ID_B);
        if (awdlCoexAddrKnown) {
            IEEE80211_ADDR_COPY(mac.node_addr, awdlCoexAddr);
        } else {
            IEEE80211_ADDR_COPY(mac.node_addr, ic->ic_myaddr);
            mac.node_addr[0] |= 0x02;
            mac.node_addr[5] ^= 0x5a;
        }
        IEEE80211_ADDR_COPY(mac.bssid_addr, (awdlCoexFilter == 1 || awdlCoexFilter == 3) ? kAwdlCoexBssid : etherbroadcastaddr);
        int cck = 0, ofdm = 0;
        if (awdlCoexOwnChan) {
            // Its own channel's band, not the BSS's: the mandatory OFDM rates (6, 12, 24 Mbit/s) on both
            // bands, CCK only on 2.4 GHz.
            cck = IEEE80211_IS_CHAN_2GHZ(awdlCoexOwnChan) ? 0x0f : 0;
            ofdm = 0x15;
        } else {
            iwx_ack_rates(sc, in, &cck, &ofdm);
        }
        mac.cck_rates = htole32(cck);
        mac.ofdm_rates = htole32(ofdm);
        // Filter 3: AWDL's BSSID and group frames, plus promiscuous receive while the listener is on its
        // own channel, different from the station's. A LISTENER context without it passes AWDL's broadcast
        // action frames but no data. On the station's own channel it would take the station's frames from
        // it, so not there. (ForbiddenItlwm's same-channel probe window, for Block Ack TX, is not ported.)
        struct ieee80211_channel *stationChan = in && in->in_phyctxt ? in->in_phyctxt->channel : nullptr;
        promiscWanted = awdlCoexFilter == 3 && !awdlCoexBaFilterOff && awdlCoexOwnChan && stationChan &&
            ieee80211_chan2ieee(ic, awdlCoexOwnChan) != ieee80211_chan2ieee(ic, stationChan);
        mac.filter_flags = htole32(awdlCoexFilter == 0 ?
            IWX_MAC_FILTER_IN_PROMISC | IWX_MAC_FILTER_IN_CONTROL_AND_MGMT | IWX_MAC_FILTER_ACCEPT_GRP |
            IWX_MAC_FILTER_IN_BEACON | IWX_MAC_FILTER_IN_PROBE_REQUEST :
            promiscWanted ? IWX_MAC_FILTER_IN_PROMISC | IWX_MAC_FILTER_IN_CONTROL_AND_MGMT |
                IWX_MAC_FILTER_ACCEPT_GRP :
            IWX_MAC_FILTER_ACCEPT_GRP);
    }
    // The state is recorded only once the firmware has taken the command. A failure leaves it unknown, and
    // the RX isolation then treats the listener as promiscuous until a later command succeeds.
    // An answer that comes back after the listener changed belongs to no one: isolation stays on
    // (uncertain) until a current command settles it. A firmware restart clears everything anyway.
    const uint32_t macTok = awdlCoexToken;
    const int macGen = sc->sc_generation;
    const int result = iwx_send_cmd_pdu(sc, IWX_MAC_CONTEXT_CMD, 0, sizeof(mac), &mac);
    if (macGen != sc->sc_generation)
        return result;
    if (!result && macTok == awdlCoexToken) {
        awdlCoexPromiscLive = promiscWanted;
        awdlCoexPromiscUncertain = false;
    } else {
        awdlCoexPromiscUncertain = true;
    }
    return result;
}

// The listener's own channel: 149 at 80 MHz when the firmware's current channel map lets us start on all of
// 149-161 (VHT80, no passive, no radar, ad hoc allowed), else 6 at 20 MHz under the same test, else none.
// 80 MHz is not tried again in a generation where its PHY command failed.
static int
awdlCoexPickOwn(struct iwx_softc *sc, uint8_t *width)
{
    struct ieee80211com *ic = &sc->sc_ic;
    auto usable = [ic](unsigned ch) {
        const auto &c = ic->ic_channels[ch];
        return c.ic_freq && isset(ic->ic_chan_active, ch) && !awdlCoexNoIr(ic, ch) &&
            !(c.ic_flags & (IEEE80211_CHAN_DFS | IEEE80211_CHAN_NOADHOC));
    };
    uint32_t hints = isset(ic->ic_beacon_hint, 6) ? 16U : 0U;
    for (unsigned ch = 149, bit = 1; ch <= 161; ch += 4, bit <<= 1)
        if (isset(ic->ic_beacon_hint, ch))
            hints |= bit;
    if (hints != awdlCoexBeaconHints) {
        awdlCoexBeaconHints = hints;
        awdlCoexPublish();
    }
    if (awdlCoexSameBand && ic->ic_bss && ic->ic_bss->ni_chan && IEEE80211_IS_CHAN_2GHZ(ic->ic_bss->ni_chan) &&
        usable(6)) {
        *width = IWX_PHY_VHT_CHANNEL_MODE20;
        return 6;
    }
    if (awdlCoexWide80FailedGen != sc->sc_generation && (ic->ic_channels[149].ic_flags & IEEE80211_CHAN_VHT80)) {
        bool all = true;
        for (unsigned ch = 149; ch <= 161; ch += 4)
            all = all && usable(ch);
        if (all) {
            *width = IWX_PHY_VHT_CHANNEL_MODE80;
            return 149;
        }
    }
    if (usable(6)) {
        *width = IWX_PHY_VHT_CHANNEL_MODE20;
        return 6;
    }
    return 0;
}

// The firmware's country notice (MCC_CHUB_UPDATE_CMD), from iwx_mcc_update. Station mode with the
// listener's own channel: answered from the coex task, which can wait on the firmware. A change after an
// answer (a withdrawal included) is answered at once, so the map never stays more permissive than the
// firmware's.
void ItlIwx::
awdlCoexMccNotice(struct iwx_softc *sc, struct iwx_mcc_chub_notif *notif)
{
    if (!awdlCoexMulti || !awdlCoexRx)
        return;
    const uint16_t reported = le16toh(notif->mcc);
    ++awdlCoexCountryNotices;
    if (awdlCoexCountryGen != sc->sc_generation) {
        // A restart: nothing from the old generation is answered or replayed.
        awdlCoexCountryGen = sc->sc_generation;
        awdlCoexCountryAnswered = 0;
        awdlCoexCountrySourceAnswered = 0;
        awdlCoexCountryLearned = false;
        awdlCoexCountryPending = false;
        awdlCoexCountryRetryUS = 0;
    }
    // Any change is answered, not only a usable country: after a firmware restart the map is the firmware's
    // current one (it keeps a learned country), and a later withdrawal has to reach it even though this
    // generation never answered a country itself.
    const bool changed = reported != awdlCoexCountryAnswered || notif->source_id != awdlCoexCountrySourceAnswered;
    // Only the latest notice is ever answered: one that matches the last answer supersedes anything
    // still pending.
    if (changed) {
        awdlCoexCountryNotice = reported;
        awdlCoexCountrySource = notif->source_id;
        awdlCoexCountryPending = true;
    } else {
        awdlCoexCountryPending = false;
    }
}

int ItlIwx::
iwx_mcc_request(struct iwx_softc *sc, uint16_t country, uint8_t source, ItlAWDL::MccSample *sample, bool apply)
{
    // The reply is recorded. The channel map changes only with apply, and then as the firmware says.
    struct iwx_mcc_update_cmd mcc_cmd;
    struct iwx_host_cmd hcmd = {
        .id = IWX_MCC_UPDATE_CMD,
        .flags = IWX_CMD_WANT_RESP,
        .data = { &mcc_cmd },
    };
    memset(&mcc_cmd, 0, sizeof(mcc_cmd));
    mcc_cmd.mcc = htole16(country);
    mcc_cmd.source_id = source;
    hcmd.len[0] = sizeof(struct iwx_mcc_update_cmd);
    hcmd.resp_pkt_len = IWX_CMD_RESP_MAX;
    int err = iwx_send_cmd(sc, &hcmd);
    if (err)
        return err;
    struct iwx_rx_packet *pkt = hcmd.resp_pkt;
    size_t resp_len = 0;
    struct iwx_mcc_update_resp *resp = nullptr;
    if (!pkt || (pkt->hdr.group_id & IWX_CMD_FAILED_MSK)) {
        err = EIO;
        goto out;
    }
    resp_len = iwx_rx_packet_payload_len(pkt);
    if (resp_len < sizeof(*resp)) {
        err = EIO;
        goto out;
    }
    resp = (struct iwx_mcc_update_resp *)pkt->data;
    if (resp_len != sizeof(*resp) + resp->n_channels * sizeof(resp->channels[0])) {
        err = EIO;
        goto out;
    }
    {
        sample->mcc = le16toh(resp->mcc);
        sample->source = resp->source_id;
        sample->status = uint8_t(le32toh(resp->status));
        const uint8_t *table = sc->sc_uhb_supported ? iwx_nvm_channels_uhb : iwx_nvm_channels_8000;
        const size_t tableCount = sc->sc_uhb_supported ? nitems(iwx_nvm_channels_uhb) : nitems(iwx_nvm_channels_8000);
        ItlAWDL::regulatoryChannelFlags(table, tableCount, 149, resp->channels, resp->n_channels, &sample->channel149);
        ItlAWDL::regulatoryChannelFlags(table, tableCount, 36, resp->channels, resp->n_channels, &sample->channel36);
        ItlAWDL::regulatoryChannelFlags(table, tableCount, 6, resp->channels, resp->n_channels, &sample->channel6);
        if (apply)
            iwx_init_channel_map(sc, NULL, resp->channels, resp->n_channels);
    }
out:
    iwx_free_resp(sc, &hcmd);
    return err;
}

// Station mode: the firmware's country notice answered with its own country and source, and its channel map
// applied, as Linux does. Nothing is invented: the firmware's reply is the map.
void ItlIwx::
awdlCoexCountryAnswer(struct iwx_softc *sc)
{
    awdlCoexCountryPending = false;
    const uint16_t mcc = awdlCoexCountryNotice;
    const uint8_t source = awdlCoexCountrySource;
    const int gen = sc->sc_generation;
    ItlAWDL::MccSample reply{};
    const int err = iwx_mcc_request(sc, mcc, source, &reply, true);
    if (gen != sc->sc_generation)
        return;
    awdlCoexCountryLastStatus = err ? 0x80000000U | uint32_t(err) : reply.status;
    if (err) {
        // Not answered: the pair isn't recorded, and the latest notice is tried again in 5 s, unless a newer
        // one replaced it meanwhile (the handler keeps only the latest).
        ++awdlCoexCountryFailures;
        awdlCoexCountryRetryUS = awdlUptimeUS() + 5000000ULL;
        if (awdlCoexCountryNotice == mcc && awdlCoexCountrySource == source)
            awdlCoexCountryPending = true;
        awdlCoexPublish();
        return;
    }
    // Beacon hints belong to the old country's rules; Linux drops them on a regulatory change too. A
    // detour through the firmware's world code isn't a change of country: the hints stay until a
    // different usable country is answered. The firmware's own channel flags still apply on every use.
    const bool usableCountry = ItlAWDL::countryNoticeUsable(mcc, source);
    if (!usableCountry)
        ++awdlCoexCountryUnusableAnswers;
    else if (mcc != awdlCoexHintCountry) {
        if (awdlCoexHintCountry)
            ++awdlCoexHintClears;
        memset(sc->sc_ic.ic_beacon_hint, 0, sizeof(sc->sc_ic.ic_beacon_hint));
        awdlCoexHintCountry = mcc;
    }
    awdlCoexCountryAnswered = mcc;
    awdlCoexCountrySourceAnswered = source;
    awdlCoexCountryAnswerSeconds = mccUptimeSeconds();
    awdlCoexCountryRetryUS = 0;
    ++awdlCoexCountryAnswers;
    awdlCoexCountryLearned = ItlAWDL::countryNoticeUsable(mcc, source);
    // A newer notice that arrived while this one waited on the firmware is still pending, and goes next.
    if (awdlCoexCountryNotice == mcc && awdlCoexCountrySource == source)
        awdlCoexCountryPending = false;
    awdlCoexPublish();
}

// Block Ack with the peer is a later phase of the port: nothing to remove yet.
int ItlIwx::
awdlCoexBaRemovePeer(struct iwx_softc *sc)
{
    awdlCoexBaPeerAdded = false;
    return 0;
}

// The listener's station and queue (phase 2). Nothing to tear down until awdlCoexTxSetup made them.
int ItlIwx::
awdlCoexTxTeardown(struct iwx_softc *sc)
{
    if (!awdlCoexTxStaAdded)
        return 0;
    const int gen = sc->sc_generation;
    const int flushErr = iwx_flush_sta_tids(sc, IWX_MONITOR_STA_ID, 0xffff);
    struct iwx_rm_sta_cmd rm;
    memset(&rm, 0, sizeof(rm));
    rm.sta_id = IWX_MONITOR_STA_ID;
    // After a firmware restart the station is gone, and the restart resets the ring.
    const int rmErr = gen == sc->sc_generation ? iwx_send_cmd_pdu(sc, IWX_REMOVE_STA, 0, sizeof(rm), &rm) : 0;
    // A firmware restart while waiting: the restart resets the ring, and the new generation's state is
    // left alone.
    if (gen != sc->sc_generation)
        return 0;
    // Only when the firmware has let go of the queue is the ring freed here, with its node references.
    // Otherwise it is kept as an orphan: a late completion or a later reset still knows it is ours.
    if (awdlCoexQueue >= 0 && awdlCoexQueue < int(nitems(sc->txq))) {
        if (!flushErr && !rmErr)
            iwx_reset_tx_ring(sc, &sc->txq[awdlCoexQueue]);
        else
            awdlCoexOrphanQueue = awdlCoexQueue;
    }
    awdlCoexTxStaAdded = false;
    awdlCoexInFlight = false;
    awdlCoexTxHalted = false;
    awdlCoexPendingSlot = -1;
    awdlCoexQueue = -1;
    awdlCoexPublish();
    return flushErr ? flushErr : rmErr;
}

// What a move of the listener's channel resets: the data rate ladder and the peer's schedule.
static void awdlCoexMoved(struct iwx_softc *)
{
    awdlCoexRateStep = 0;
    awdlCoexRateWindowOK = awdlCoexRateWindowFailed = 0;
    awdlCoexPeerSchedule.reset();
}

// The peer's schedule, from its sync frames, as ForbiddenItlwm's dedicated mode tracks it: its Block Ack
// peer once there is one, otherwise the first peer heard on our channel.
static void awdlCoexNoteSchedule(const uint8_t *cf, size_t coexLen, uint64_t heardUS)
{
    if ((!awdlCoexBaPeerAdded || ItlAWDL::sameAddress(cf + 10, awdlCoexBaPeer)) &&
        (ItlAWDL::sameAddress(cf + 10, awdlCoexSchedPeer) || !awdlCoexScheduleCurrent() ||
         !awdlCoexPeerSchedule.fresh(heardUS))) {
        ItlAWDL::PeerSyncInfo peerSync{};
        bool badSequence = false;
        if (ItlAWDL::peerSyncFromAction(cf, coexLen, &peerSync, &badSequence)) {
            if (!awdlCoexScheduleCurrent() || !ItlAWDL::sameAddress(cf + 10, awdlCoexSchedPeer)) {
                awdlCoexPeerSchedule.reset();
                awdlCoexSchedToken = awdlCoexToken;
                memcpy(awdlCoexSchedPeer, cf + 10, sizeof(awdlCoexSchedPeer));
            }
            awdlCoexPeerSchedule.notePeerFrame(heardUS);
            awdlCoexPeerSchedule.note(peerSync, heardUS, uint8_t(awdlCoexChannel));
            ++awdlCoexPeerSyncFrames;
        } else if (awdlCoexScheduleCurrent() && ItlAWDL::sameAddress(cf + 10, awdlCoexSchedPeer)) {
            if (badSequence) {
                ++awdlCoexPeerSequenceRejected;
                awdlCoexPeerSchedule.invalidate();
            } else {
                awdlCoexPeerSchedule.notePeerFrame(heardUS);
            }
        }
    }
}

// Data received from a peer: the schedule peer is awake (the send gate's doze test).
static void awdlCoexNoteDataFrom(const uint8_t *addr, uint64_t nowUS)
{
    if (awdlCoexScheduleCurrent() && ItlAWDL::sameAddress(addr, awdlCoexSchedPeer)) {
        awdlCoexSchedPeerRxUS = nowUS;
        awdlCoexPeerSchedule.notePeerFrame(nowUS);
    }
}

// Moves the listener's own context when the firmware's channel map changes: up to 149 at 80 MHz when it is
// allowed (at most every 10 s), back down to 6 at once when it is withdrawn. The binding stays; only the PHY
// context moves, then the listener's MAC context takes the new band's rates.
void ItlIwx::
awdlCoexRetarget(struct iwx_softc *sc)
{
    const int gen = sc->sc_generation;
    if (!awdlCoexMultiActive || !awdlCoexAdded || awdlCoexGeneration != gen || awdlCoexAddingGen == gen ||
        awdlCoexRemovingGen == gen || awdlCoexPhyUncertainGen == gen || awdlCoexBrokenGeneration == gen ||
        awdlStopping) {
        awdlCoexMovePending = false;
        return;
    }
    struct ieee80211com *ic = &sc->sc_ic;
    struct iwx_phy_ctxt *own = &sc->sc_phyctxt[kAwdlCoexPhyId];
    uint8_t width = IWX_PHY_VHT_CHANNEL_MODE20;
    const int want = awdlCoexPickOwn(sc, &width);
    const int have = own->channel ? ieee80211_chan2ieee(ic, own->channel) : 0;
    const int haveWidth = awdlCoexOwnWidth;
    if (!want || (want == have && (want != 149 || haveWidth == 80))) {
        awdlCoexMovePending = false;
        return;
    }
    const uint64_t now = awdlUptimeUS();
    if (want == 149 && awdlCoexLastMoveUS && now - awdlCoexLastMoveUS < 10000000ULL) {
        awdlCoexMovePending = false;
        return;
    }
    // Nothing new is submitted from here (the eligibility guard refuses while a move is pending), and the
    // move waits until every sync and data descriptor has completed. After 2 s it goes anyway, but nothing
    // more is sent this generation: what is left may still go out on the old channel.
    if (!awdlCoexMovePending) {
        awdlCoexMovePending = true;
        awdlCoexMovePendingUS = now;
    }
    const bool drained = !awdlCoexInFlight && (!awdlCoexTxStaAdded ||
        (awdlCoexQueue >= 0 && awdlCoexQueue < int(nitems(sc->txq)) && sc->txq[awdlCoexQueue].queued == 0));
    if (!drained) {
        if (now - awdlCoexMovePendingUS < 2000000ULL)
            return;
        ++awdlCoexMoveDrainTimeouts;
        awdlCoexMultiState = "move-drain-timeout";
        // A software halt doesn't stop what the firmware already holds: flush and remove the listener's
        // station and queue first. If that fails, the firmware may still send on the old channel, so the
        // listener is quarantined and not moved; a firmware restart clears it. Either way listener TX stays
        // off for this generation: teardown clears the halt, so it is set again after, and setup isn't
        // retried.
        const uint32_t drainTok = awdlCoexToken;
        const struct ieee80211_node *drainBss = ic->ic_bss;
        const int flushErr = awdlCoexTxTeardown(sc);
        if (gen != sc->sc_generation)
            return;
        awdlCoexTxHalted = true;
        awdlCoexTxSetupTried = kAwdlCoexTxSetupTries;
        if (drainTok != awdlCoexToken || ic->ic_bss != drainBss || !awdlCoexAdded || !awdlCoexMultiActive) {
            // The listener changed while flushing: this move's decision is stale.
            awdlCoexMovePending = false;
            awdlCoexPublish();
            return;
        }
        if (flushErr) {
            awdlCoexBrokenGeneration = gen;
            awdlCoexMovePending = false;
            awdlCoexMultiState = "move-flush-failed";
            awdlCoexPublish();
            return;
        }
    }
    // The peer station was set up for the old channel's width: the phone asks again after the move. If it
    // can't be removed, its ownership is unknown, so the move waits (BA is quarantined) and TX stays guarded.
    const uint32_t removeTok = awdlCoexToken;
    const int peerErr = awdlCoexBaRemovePeer(sc);
    if (gen != sc->sc_generation)
        return;
    if (peerErr || removeTok != awdlCoexToken || !awdlCoexAdded) {
        awdlCoexMovePending = false;
        if (peerErr)
            awdlCoexMultiState = "move-peer-remove-failed";
        awdlCoexPublish();
        return;
    }
    const uint32_t tok = awdlCoexToken;
    // Re-decide after any wait above: the map may have changed again.
    {
        uint8_t again = IWX_PHY_VHT_CHANNEL_MODE20;
        if (awdlCoexPickOwn(sc, &again) != want || again != width) {
            awdlCoexMovePending = false;
            return;
        }
    }
    struct ieee80211_channel *before = own->channel;
    own->channel = &ic->ic_channels[want];
    awdlCoexPhyWidth = width;
    const int err = iwx_phy_ctxt_cmd(sc, own, 1, 1, IWX_FW_CTXT_ACTION_MODIFY, 0);
    awdlCoexPhyWidth = -1;
    awdlCoexLastMoveUS = now;
    if (gen != sc->sc_generation)
        return;
    if (err || tok != awdlCoexToken) {
        // No answer to read, or the listener changed while waiting: the context may be on either channel.
        // Nothing more is sent this generation.
        awdlCoexMovePending = false;
        ++awdlCoexMoveFailures;
        own->channel = before;
        awdlCoexPhyUncertainGen = gen;
        if (width == IWX_PHY_VHT_CHANNEL_MODE80)
            awdlCoexWide80FailedGen = gen;
        awdlCoexTxHalted = true;
        awdlCoexMultiState = "move-uncertain";
        awdlCoexPublish();
        return;
    }
    ++awdlCoexMoves;
    awdlCoexMoved(sc);
    awdlCoexOwnChan = own->channel;
    awdlCoexOwnWidth = width == IWX_PHY_VHT_CHANNEL_MODE80 ? 80 : 20;
    awdlCoexChannel = want;
    awdlFixedChannel = want;
    // PHY and MAC are one transition: sending resumes only once the MAC context has the new band's rates.
    const int macErr = awdlCoexMacSend(sc, IWX_FW_CTXT_ACTION_MODIFY);
    if (gen != sc->sc_generation)
        return;
    awdlCoexMovePending = false;
    if (macErr || tok != awdlCoexToken || !awdlCoexAdded) {
        ++awdlCoexMoveFailures;
        awdlCoexTxHalted = true;
        awdlCoexMultiState = "move-mac-failed";
    }
    awdlCoexPublish();
}

// The listener's first PHY command of a generation. PHY context 1 was added at firmware start on channel 1
// (2.4 GHz) and nothing binds it in this backend. The P2P path of this driver found that a band change
// needs REMOVE + ADD of the PHY context (iwx_p2p_set_channel) on the AX210, so while the context is
// unbound a band change is done that way; ForbiddenItlwm sends MODIFY. A same-band change is a MODIFY.
int ItlIwx::
awdlCoexOwnPhy(struct iwx_softc *sc, struct iwx_phy_ctxt *own, struct ieee80211_channel *target, uint8_t width)
{
    const uint16_t bands = IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_5GHZ;
    const bool crossBand = !own->channel || (own->channel->ic_flags & bands) != (target->ic_flags & bands);
    const int gen = sc->sc_generation;
    int err;
    awdlCoexPhyWidth = width;
    if (crossBand) {
        err = iwx_phy_ctxt_cmd(sc, own, 1, 1, IWX_FW_CTXT_ACTION_REMOVE, 0);
        if (!err && gen == sc->sc_generation) {
            own->channel = target;
            err = iwx_phy_ctxt_cmd(sc, own, 1, 1, IWX_FW_CTXT_ACTION_ADD, 0);
        }
    } else {
        own->channel = target;
        err = iwx_phy_ctxt_cmd(sc, own, 1, 1, IWX_FW_CTXT_ACTION_MODIFY, 0);
    }
    awdlCoexPhyWidth = -1;
    return err;
}

// Adds or removes the listener. The binding always keeps the station first, so removing the listener
// leaves exactly what the station had.
int ItlIwx::
awdlCoexListener(struct iwx_softc *sc, bool add)
{
    struct ieee80211com *ic = &sc->sc_ic;
    struct iwx_node *in = (struct iwx_node *)ic->ic_bss;
    if (!in || !in->in_phyctxt)
        return EINVAL;
    if (awdlCoexGeneration != sc->sc_generation) {
        // A firmware restart dropped every context, ours included, and its station and queue.
        if (awdlCoexAdded)
            awdlCoexLostToRestart = true;
        awdlCoexAdded = false;
        awdlCoexTxStaAdded = false;
        awdlCoexInFlight = false;
        awdlCoexPendingSlot = -1;
        awdlCoexAddrApplied = false;
        if (awdlCoexMulti)
            awdlCoexMultiState = "not-added";
        awdlCoexMultiActive = false;
        awdlCoexOwnChan = nullptr;
        awdlCoexMovePending = false;
        awdlCoexPromiscLive = false;
        awdlCoexPromiscUncertain = false;
        awdlCoexBaPeerAdded = false;
        awdlCoexBaFilterOff = false;
        awdlCoexQueue = -1;
        awdlCoexTxSetupTried = 0;
        awdlCoexGeneration = sc->sc_generation;
        ++awdlCoexToken;
    }
    // A removal while an add waits on the firmware: the add sees this and undoes what it made.
    if (!add && awdlCoexAddingGen == sc->sc_generation) {
        awdlCoexRemoveRequested = true;
        ++awdlCoexToken;
        return 0;
    }
    if (add == awdlCoexAdded)
        return 0;
    const int gen = sc->sc_generation;
    if (awdlCoexRemovingGen == gen || (add && awdlCoexAddingGen == gen))
        return EBUSY;
    // Anything that was waiting on the firmware for the old state sees this and stops.
    const uint32_t tok = ++awdlCoexToken;
    // After a failed teardown the firmware may still hold our context or station.
    if (add && awdlCoexBrokenGeneration == sc->sc_generation)
        return EIO;
    // -itlawdlcoexshared on a DFS station channel: the listener could never send there (radar rules), so it
    // isn't added at all.
    if (add && awdlCoexShared && in->in_phyctxt->channel &&
        (in->in_phyctxt->channel->ic_flags & IEEE80211_CHAN_DFS)) {
        awdlCoexMultiState = "shared-dfs-refused";
        awdlCoexPublish();
        return EPERM;
    }
    // While adding: no second add, and a removal request makes this add roll back. Cleared on every return.
    struct AddingScope {
        bool on;
        uint32_t owner;
        explicit AddingScope(bool a, int g) : on(a), owner(0) {
            if (on) {
                owner = ++awdlCoexAddingNext;
                awdlCoexAddingOwner = owner;
                awdlCoexAddingGen = g;
                awdlCoexRemoveRequested = false;
            }
        }
        // Clears only its own record: an old add waking after a restart leaves a newer add's guard alone.
        ~AddingScope() {
            if (on && awdlCoexAddingOwner == owner) {
                awdlCoexAddingGen = -1;
                awdlCoexAddingOwner = 0;
            }
        }
    } addingScope(add, gen);
    // Each firmware command can sleep in the gate. After each, the add goes on only for the same listener
    // operation, firmware and association, with no removal or detach meanwhile; the BSS node is fetched again.
    // The association this add is for: its node, the station's PHY context, and the station's MAC id.
    const struct ieee80211_node *addBss = ic->ic_bss;
    const struct iwx_phy_ctxt *addStationPhy = in->in_phyctxt;
    const uint32_t addStationMac = IWX_FW_CMD_ID_AND_COLOR(in->in_id, in->in_color);
    auto stillWanted = [&]() {
        if (ic->ic_bss != addBss)
            return false;
        in = (struct iwx_node *)ic->ic_bss;
        if (!in || in->in_phyctxt != addStationPhy || IWX_FW_CMD_ID_AND_COLOR(in->in_id, in->in_color) != addStationMac)
            return false;
        // Not ic_state: the add runs inside iwx_run, before net80211 commits RUN. A RUN exit arrives as a
        // removal request, which changes the token.
        return tok == awdlCoexToken && gen == sc->sc_generation && !awdlStopping && !awdlCoexRemoveRequested &&
            in && in->in_phyctxt;
    };
    // Its own context on its own channel when asked for and allowed (the firmware marks the channel active,
    // no radar, and schedules bindings itself); otherwise the station's. Removal uses whichever add chose.
    bool multi = add ? false : awdlCoexMultiActive;
    if (add) {
        awdlCoexOwnChan = nullptr;
        awdlCoexMovePending = false;
    }
    if (add && awdlCoexMulti) {
        uint8_t ownWidth = IWX_PHY_VHT_CHANNEL_MODE20;
        const int ownNumber = awdlCoexPickOwn(sc, &ownWidth);
        if (!isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA)) {
            awdlCoexMultiState = "no-dynamic-quota";
        } else if (awdlCoexPhyUncertainGen == gen) {
            awdlCoexMultiState = "phy-uncertain";
        } else if (ownNumber) {
            struct ieee80211_channel *target = &ic->ic_channels[ownNumber];
            struct iwx_phy_ctxt *own = &sc->sc_phyctxt[kAwdlCoexPhyId];
            struct ieee80211_channel *before = own->channel;
            const int phyErr = awdlCoexOwnPhy(sc, own, target, ownWidth);
            awdlCoexPhyResult = phyErr ? 0x80000000U | uint32_t(phyErr) : 1;
            if (gen != sc->sc_generation)
                return ECANCELED;
            if (phyErr) {
                // No answer to read: the firmware's context may or may not have moved. Keep it reserved.
                own->channel = before;
                awdlCoexPhyUncertainGen = gen;
                if (ownWidth == IWX_PHY_VHT_CHANNEL_MODE80)
                    awdlCoexWide80FailedGen = gen;
                awdlCoexMultiState = "phy-uncertain";
            } else {
                multi = true;
                awdlCoexOwnChan = target;
                awdlCoexOwnWidth = ownWidth == IWX_PHY_VHT_CHANNEL_MODE80 ? 80 : 20;
                awdlCoexLastMoveUS = awdlUptimeUS();
            }
            if (!stillWanted()) {
                awdlCoexOwnChan = nullptr;
                awdlCoexMovePending = false;
                awdlCoexMultiState = "add-cancelled";
                awdlCoexPublish();
                return ECANCELED;
            }
        } else {
            awdlCoexMultiState = "fallback-same";
        }
    }
    struct iwx_phy_ctxt *phy = multi ? &sc->sc_phyctxt[kAwdlCoexPhyId] : in->in_phyctxt;
    struct iwx_binding_cmd bind;
    memset(&bind, 0, sizeof(bind));
    bind.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(phy->id, phy->color));
    bind.action = htole32(IWX_FW_CTXT_ACTION_MODIFY);
    bind.phy = htole32(IWX_FW_CMD_ID_AND_COLOR(phy->id, phy->color));
    bind.macs[0] = htole32(IWX_FW_CMD_ID_AND_COLOR(in->in_id, in->in_color));
    for (int i = 1; i < IWX_MAX_MACS_IN_BINDING; i++)
        bind.macs[i] = htole32(IWX_FW_CTXT_INVALID);
    bind.lmac_id = htole32(IEEE80211_IS_CHAN_2GHZ(phy->channel) ||
                           !isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_CDB_SUPPORT) ?
                           IWX_LMAC_24G_INDEX : IWX_LMAC_5G_INDEX);
    if (multi) {
        // Its own binding: the listener alone on its own PHY context. The station's binding is untouched.
        bind.action = htole32(add ? IWX_FW_CTXT_ACTION_ADD : IWX_FW_CTXT_ACTION_REMOVE);
        bind.macs[0] = htole32(IWX_FW_CTXT_INVALID);
    }
    int err = 0;
    uint32_t status = 0;
    if (add) {
        // awdl0's address, if the AWDL interface has set it already.
        awdlCoexAddrSyncWanted();
        err = awdlCoexMacSend(sc, IWX_FW_CTXT_ACTION_ADD);
        awdlCoexMacResult = err ? 0x80000000U | uint32_t(err) : 1;
        if (gen != sc->sc_generation)
            return err ? err : ECANCELED;
        if (!err && !stillWanted()) {
            // Made for a listener that is no longer wanted: take it away again.
            if (awdlCoexMacSend(sc, IWX_FW_CTXT_ACTION_REMOVE) && gen == sc->sc_generation) {
                ++awdlCoexTeardownErrors;
                awdlCoexBrokenGeneration = gen;
            }
            if (gen == sc->sc_generation) {
                if (multi)
                    awdlCoexMultiState = "add-cancelled";
                awdlCoexPublish();
            }
            return ECANCELED;
        }
        if (err) {
            // This command has no response to read, so a failure doesn't say whether the firmware made
            // the context. Don't add it again until the firmware restarts.
            ++awdlCoexTeardownErrors;
            awdlCoexBrokenGeneration = gen;
            awdlCoexPublish();
            return err;
        }
        bind.macs[multi ? 0 : 1] = htole32(IWX_FW_CMD_ID_AND_COLOR(kAwdlCoexMacId, 0));
        err = iwx_send_cmd_pdu_status(sc, IWX_BINDING_CONTEXT_CMD, sizeof(bind), &bind, &status);
        // A transport failure, unlike a refusal in the answer, doesn't say whether the binding exists.
        const bool bindAmbiguous = err != 0;
        awdlCoexBindStatus = status;
        if (multi)
            awdlCoexMultiBindResult = err ? 0x80000000U | uint32_t(err) : (status ? 0x40000000U | status : 1);
        if (!err && status)
            err = EIO;
        awdlCoexBindResult = err ? 0x80000000U | uint32_t(err) : 1;
        if (err) {
            // Leave no context the binding doesn't know about. If that fails too, the firmware may still
            // have it, so nothing is added again until the firmware restarts.
            if (gen == sc->sc_generation &&
                (awdlCoexMacSend(sc, IWX_FW_CTXT_ACTION_REMOVE) || bindAmbiguous) && gen == sc->sc_generation) {
                ++awdlCoexTeardownErrors;
                awdlCoexBrokenGeneration = gen;
                if (multi)
                    awdlCoexPhyUncertainGen = gen;
            }
            awdlCoexPublish();
            return err;
        }
        // (ForbiddenItlwm's -itlawdlcoexlowlat block stood here. LOW_LATENCY_CMD took Wi-Fi down on an
        // AX211 and is not ported.)
        if (gen != sc->sc_generation)
            return ECANCELED;
        if (!stillWanted()) {
            // Bound for a listener that is no longer wanted: unbind, then remove it.
            struct iwx_binding_cmd undo = bind;
            if (multi) {
                undo.action = htole32(IWX_FW_CTXT_ACTION_REMOVE);
                undo.macs[0] = htole32(IWX_FW_CTXT_INVALID);
            } else {
                undo.macs[1] = htole32(IWX_FW_CTXT_INVALID);
            }
            uint32_t undoStatus = 0;
            int undoErr = iwx_send_cmd_pdu_status(sc, IWX_BINDING_CONTEXT_CMD, sizeof(undo), &undo, &undoStatus);
            if (!undoErr && undoStatus)
                undoErr = EIO;
            if (gen == sc->sc_generation && !undoErr)
                undoErr = awdlCoexMacSend(sc, IWX_FW_CTXT_ACTION_REMOVE);
            if (undoErr && gen == sc->sc_generation) {
                ++awdlCoexTeardownErrors;
                awdlCoexBrokenGeneration = gen;
            }
            if (gen == sc->sc_generation) {
                if (multi)
                    awdlCoexMultiState = "add-cancelled";
                awdlCoexPublish();
            }
            return ECANCELED;
        }
        awdlCoexAdded = true;
        awdlCoexMultiActive = multi;
        if (multi)
            awdlCoexMultiState = "own-channel";

        if (awdlCoexAdds) {
            if (awdlCoexLostToRestart)
                ++awdlCoexAddsAfterRestart;
            else
                ++awdlCoexAddsAfterRemove;
        }
        awdlCoexLostToRestart = false;
        ++awdlCoexAdds;
        awdlCoexAddrApplied = awdlCoexAddrKnown;
        memcpy(awdlCoexAddrInFirmware, awdlCoexAddr, sizeof(awdlCoexAddrInFirmware));
        awdlCoexChannel = ieee80211_chan2ieee(ic, phy->channel);
        // Our frames name the channel we're on.
        awdlFixedChannel = awdlCoexChannel;
        awdlCoexTxSetupTried = 0;
    } else {
        // Its station (and with it its queue) first, then the station's binding again, then the listener.
        // The listener is unavailable from here on: no new TX or setup starts while these commands wait.
        // A command can sleep in the gate. If the firmware restarted meanwhile, everything is gone already,
        // and the new generation's state, which may already be set up again, is left alone.
        awdlCoexAdded = false;
        awdlCoexAddrApplied = false;
        awdlCoexOwnChan = nullptr;
        awdlCoexMovePending = false;
        if (awdlCoexMultiActive)
            awdlCoexMultiState = "removed";
        awdlCoexMultiActive = false;
        awdlCoexRemovingGen = gen;
        awdlCoexShareChannel();
        awdlCoexBaRemovePeer(sc);
        if (gen != sc->sc_generation)
            return 0;
        const int txErr = awdlCoexTxTeardown(sc);
        if (gen != sc->sc_generation)
            return 0;
        err = iwx_send_cmd_pdu_status(sc, IWX_BINDING_CONTEXT_CMD, sizeof(bind), &bind, &status);
        if (!err && status)
            err = EIO;
        if (gen != sc->sc_generation)
            return 0;
        const int macErr = awdlCoexMacSend(sc, IWX_FW_CTXT_ACTION_REMOVE);
        if (gen != sc->sc_generation)
            return 0;
        // The host can't take the context back by itself, so it forgets it either way; but after any
        // failure nothing is added again until the firmware restarts.
        awdlCoexRemovingGen = -1;
        ++awdlCoexRemoves;
        if (!err)
            err = macErr;
        if (!err)
            err = txErr;
        if (err) {
            ++awdlCoexTeardownErrors;
            awdlCoexBrokenGeneration = gen;
        }
    }
    awdlCoexPublish();
    return err;
}

// awdl0's address, as the AWDL interface set it (ForbiddenItlwm learned it from the first sync frame
// macOS built). Only what is wanted changes here; the firmware's copy changes in awdlCoexAddrApply.
static void awdlCoexAddrSyncWanted()
{
    struct ieee80211com *ic = awdlCoexIc;
    if (!ic || !ic->ic_awdl_addr_gen || !ItlAWDL::knownAddress(ic->ic_awdl_addr))
        return;
    memcpy(awdlCoexAddr, ic->ic_awdl_addr, sizeof(awdlCoexAddr));
    awdlCoexAddrKnown = true;
}

// The firmware recognises acknowledgements only to the address in its MAC context. When awdl0's address
// changes, the listener's context is modified (from the task, at most once a second after a failure).
void ItlIwx::
awdlCoexAddrApply(struct iwx_softc *sc)
{
    awdlCoexAddrSyncWanted();
    if (!awdlCoexAdded || !awdlCoexAddrKnown ||
        (awdlCoexAddrApplied && !memcmp(awdlCoexAddrInFirmware, awdlCoexAddr, sizeof(awdlCoexAddr))))
        return;
    const uint64_t now = awdlUptimeUS();
    if (awdlCoexAddrRetryUS && now < awdlCoexAddrRetryUS)
        return;
    const uint32_t token = awdlCoexToken;
    const int gen = sc->sc_generation;
    // The address placed in the command, in case the wanted one changes while it waits.
    uint8_t sent[6];
    memcpy(sent, awdlCoexAddr, sizeof(sent));
    const int modifyErr = awdlCoexMacSend(sc, IWX_FW_CTXT_ACTION_MODIFY);
    // Commit nothing for a listener that went away while the command waited.
    if (token != awdlCoexToken || gen != sc->sc_generation || awdlStopping || !awdlCoexAdded)
        return;
    if (modifyErr) {
        ++awdlCoexAddrFailures;
        awdlCoexAddrApplied = false;
        awdlCoexAddrRetryUS = now + 1000000ULL;
        awdlCoexPublish();
        return;
    }
    awdlCoexAddrRetryUS = 0;
    ++awdlCoexAddrChanges;
    memcpy(awdlCoexAddrInFirmware, sent, sizeof(awdlCoexAddrInFirmware));
    awdlCoexAddrApplied = !memcmp(sent, awdlCoexAddr, sizeof(sent));
    awdlCoexPublish();
}

void ItlIwx::
awdlCoexTxTask(void *arg)
{
    struct iwx_softc *sc = (struct iwx_softc *)arg;
    ItlIwx *that = container_of(sc, ItlIwx, com);
    if (awdlStopping)
        return;
    if (!that->getMainWorkLoop() || !that->getMainCommandGate()) {
        // Too early in startup: try again later.
        timeout_add_msec(&awdlCoexTxTimer, kAwdlCoexTxPeriodMS);
        return;
    }
    if (!that->getMainWorkLoop()->inGate()) {
        that->getMainCommandGate()->runAction(
            [](OSObject *, void *arg0, void *, void *, void *) -> IOReturn {
                awdlCoexTxTask(arg0);
                return kIOReturnSuccess;
            }, arg);
        return;
    }
    if (awdlStopping)
        return;
    struct ieee80211com *ic = &sc->sc_ic;
    const bool running = (IC2IFP(ic)->if_flags & IFF_RUNNING) && ic->ic_state == IEEE80211_S_RUN &&
        ic->ic_opmode == IEEE80211_M_STA && !(sc->sc_flags & (IWX_FLAG_HW_ERR | IWX_FLAG_SHUTDOWN | IWX_FLAG_RFKILL));
    if (running && awdlCoexCountryPending && awdlCoexCountryGen == sc->sc_generation &&
        (!awdlCoexCountryRetryUS || awdlUptimeUS() >= awdlCoexCountryRetryUS) &&
        (!ItlAWDL::countryNoticeUsable(awdlCoexCountryNotice, awdlCoexCountrySource) ||
         ItlAWDL::mccEchoDue(awdlCoexCountryAnswers != 0, mccUptimeSeconds(), awdlCoexCountryAnswerSeconds)))
        that->awdlCoexCountryAnswer(sc);
    if (running && !awdlStopping && awdlCoexAdded && awdlCoexGeneration == sc->sc_generation)
        that->awdlCoexRetarget(sc);
    if (running && !awdlStopping && awdlCoexAdded && awdlCoexGeneration == sc->sc_generation)
        that->awdlCoexAddrApply(sc);
    if (running && awdlCoexAdded && awdlCoexGeneration == sc->sc_generation) {
        if (awdlCoexTx && !awdlCoexTxStaAdded && awdlCoexTxSetupTried < kAwdlCoexTxSetupTries &&
            awdlCoexBrokenGeneration != sc->sc_generation) {
            ++awdlCoexTxSetupTried;
            (void)that->awdlCoexTxSetup(sc);
        }
        const uint64_t watchNowUS = awdlUptimeUS();
        if (awdlCoexTxStaAdded && awdlCoexInFlight && watchNowUS - awdlCoexInFlightUS > kAwdlCoexTxStuckUS &&
            (!awdlCoexLastCompletionUS || watchNowUS - awdlCoexLastCompletionUS > 1000000ULL ||
             watchNowUS - awdlCoexInFlightUS > 5000000ULL)) {
            // A late completion for this frame no longer matches the pending one.
            awdlCoexInFlight = false;
            awdlCoexPendingSlot = -1;
            ++awdlCoexTxStuck;
            const bool halt = ++awdlCoexStuckStreak >= 3;
            if (halt) {
                awdlCoexTxHalted = true;
                // Three stalls with no completion: the queue may be wedged. Flush and remove the listener's
                // station and queue (descriptors included); setup then rebuilds them, within its try limit.
                // If the firmware doesn't confirm, quarantine until it restarts.
                const int wedgedGen = sc->sc_generation;
                const uint32_t wedgedTok = awdlCoexToken;
                const int teardownErr = that->awdlCoexTxTeardown(sc);
                if (wedgedGen == sc->sc_generation) {
                    if (teardownErr)
                        awdlCoexBrokenGeneration = wedgedGen;
                    else if (wedgedTok == awdlCoexToken)
                        ++awdlCoexWedgedRebuilds;
                    awdlCoexStuckStreak = 0;
                }
            } else {
                ++awdlCoexTxPauses;
                awdlCoexTxPauseUntilUS = watchNowUS + 500000ULL;
            }
            {
                const auto &haltRing = sc->txq[awdlCoexQueue >= 0 ? awdlCoexQueue : 0];
                snprintf(awdlCoexFaultSnapshot, sizeof(awdlCoexFaultSnapshot),
                         "%s queue %d token %u gen %d last-ssn %u tail %d queued %d last-slot %d streak %u "
                         "pending-ms %u last-completion-ms %u",
                         halt ? "halt" : "pause", awdlCoexQueue, awdlCoexToken, sc->sc_generation, awdlCoexLastSsn,
                         haltRing.tail, haltRing.queued, awdlCoexLastSlot, awdlCoexStuckStreak,
                         unsigned((watchNowUS - awdlCoexInFlightUS) / 1000),
                         awdlCoexLastCompletionUS ? unsigned((watchNowUS - awdlCoexLastCompletionUS) / 1000) : 0xffffffffU);
            }
            awdlCoexPublish();
        }
        // Setup waits on the firmware; detach or a RUN exit may have come meanwhile.
        // With the data pump the task runs every few milliseconds; sync frames keep their own pace.
        const uint64_t taskNowUS = awdlUptimeUS();
        // Sync pacing. While data moves, every 100 ms. While idle, every 45 ms (kSyncIdleIntervalUS).
        // (ForbiddenItlwm's -itlawdlawsync, AW-timed idle sync frames, is a later phase of the port.)
        const bool coexDataActive = awdlCoexLastDataUS && taskNowUS - awdlCoexLastDataUS < 1000000ULL;
        awdlCoexScheduleCalibrated = awdlCoexScheduleCurrent() && awdlCoexPeerSchedule.calibrated &&
            awdlCoexPeerSchedule.fresh(taskNowUS);
        const uint64_t sinceSync = taskNowUS - awdlCoexLastSyncUS;
        const bool syncDue = !awdlCoexData ||
            sinceSync >= (coexDataActive ? 100000ULL : ItlAWDL::kSyncIdleIntervalUS);
        if (awdlCoexTxStaAdded && !awdlCoexInFlight && !awdlCoexTxHalted && !awdlStopping && awdlCoexAdded &&
            taskNowUS >= awdlCoexTxPauseUntilUS && syncDue) {
            awdlCoexLastSyncUS = taskNowUS;
            if (awdlCoexData && !coexDataActive)
                ++awdlCoexIdleSyncFrames;
            that->awdlCoexTxSend(sc);
        }
        if (awdlCoexData && !awdlStopping && awdlCoexAdded) {
            // (Block Ack handling and the peer pause are later phases of the port.)
            that->awdlCoexDataPump(sc);
        }
        const uint64_t publishNow = awdlUptimeUS();
        if (!awdlStopping && publishNow - awdlCoexLastRoutinePublishUS >= 1000000ULL) {
            awdlCoexLastRoutinePublishUS = publishNow;
            awdlCoexPublish();
        }
    }
    if (!awdlStopping) {
        // Every 2 ms while data moved in the last second, otherwise the sync period.
        const uint64_t timerNowUS = awdlUptimeUS();
        const bool dataActive = awdlCoexData && awdlCoexLastDataUS && timerNowUS - awdlCoexLastDataUS < 1000000ULL;
        unsigned nextMS = dataActive ? 2 : (awdlCoexData ? 20 : unsigned(kAwdlCoexTxPeriodMS));
        if (awdlCoexGate && awdlCoexGateSinceUS && running && awdlCoexGeneration == sc->sc_generation)
            nextMS = 1;  // Holding for the phone: look again as soon as it may be back.
        timeout_add_msec(&awdlCoexTxTimer, nextMS);
    }
}

// Receive hook, from iwx_rx_frame before net80211 sees the frame. Frames with AWDL's BSSID (and Block Ack
// action frames to our AWDL address) are the listener's: action frames go to awdl0's management path
// (ic_vendor_action_handler), data from a peer heard on our channel to awdl0's data path
// (ic_awdl_data_handler). Returns true when the frame was consumed (freed or handed off).
bool ItlIwx::
awdlCoexRxHook(struct iwx_softc *sc, mbuf_t m, int chanidx, bool chanValid, int rate_n_flags,
               struct ieee80211_rxinfo *rxi)
{
    struct ieee80211com *ic = &sc->sc_ic;
    if (!awdlCoexProbe || !awdlCoexAdded)
        goto isolation;
    if (awdlRxMacMaskValid)
        ++awdlCoexMacMaskSeen[awdlRxMacMask < 3 ? awdlRxMacMask : 3];
    if (mbuf_len(m) >= 24) {
        const uint8_t *cf = mtod(m, const uint8_t *);
        const bool awdlFrame = !memcmp(cf + 16, kAwdlCoexBssid, 6);
        // A Block Ack action frame to our AWDL address is ours whatever BSSID it carries.
        const bool baToUs = cf[0] == 0xd0 && mbuf_len(m) >= 26 && cf[24] == 3 && awdlCoexAddrKnown &&
            ItlAWDL::sameAddress(cf + 4, awdlCoexAddr);
        if (baToUs)
            ++awdlCoexBaActionSeen[cf[25] < 3 ? cf[25] : 3];
        if ((awdlFrame || baToUs) && cf[0] == 0xd0) {
            ++awdlCoexAwdlAction;
            ++(chanValid && unsigned(chanidx) == awdlCoexChannel ? awdlCoexActionOnChannel : awdlCoexActionOffChannel);
            // Some setups pad the frame with zeros after its last TLV; that padding is cut, so the frame
            // parses and is handed off as sent (ForbiddenItlwm issue #6).
            size_t coexLen = mbuf_len(m);
            if (awdlCoexRx && !mbuf_next(m)) {
                const size_t unpadded = ItlAWDL::trimZeroPadding(cf, coexLen);
                if (unpadded != coexLen) {
                    coexLen = unpadded;
                    ++awdlCoexZeroPadTrimmed;
                }
            }
            ItlAWDL::ActionFrame coexAction;
            if (awdlCoexRx && !mbuf_next(m) &&
                ItlAWDL::parseAction(cf, coexLen, &coexAction) == ItlAWDL::ParseResult::Valid) {
                const uint64_t heardUS = awdlUptimeUS();
                const bool ourChannel = chanValid && unsigned(chanidx) == awdlCoexChannel;
                const bool fromOwnAddr = awdlCoexAddrKnown && ItlAWDL::sameAddress(cf + 10, awdlCoexAddr);
                if (!(cf[10] & 1) && !fromOwnAddr &&
                    (!awdlCoexBaPeerAdded || ItlAWDL::sameAddress(cf + 10, awdlCoexBaPeer))) {
                    uint8_t seq[16];
                    if (awdlCoexPeerSequence(cf, coexLen, seq)) {
                        ++awdlCoexPeerSeqFrames;
                        if (memcmp(seq, awdlCoexPeerSeq, sizeof(seq))) {
                            memcpy(awdlCoexPeerSeq, seq, sizeof(seq));
                            ++awdlCoexPeerSeqChanges;
                            awdlCoexPublish();
                        }
                    }
                }
                if (!(cf[10] & 1) && ourChannel && !fromOwnAddr) {
                    awdlCoexPeerHeard(cf + 10, heardUS);
                    awdlCoexNoteSchedule(cf, coexLen, heardUS);
                }
                // To awdl0's management path. The handler copies the bytes and doesn't keep the mbuf.
                if (coexLen != mbuf_len(m)) {
                    mbuf_setlen(m, coexLen);
                    mbuf_pkthdr_setlen(m, coexLen);
                }
                if (ic->ic_vendor_action_handler && ic->ic_vendor_action_handler(ic, m, rxi))
                    ++awdlCoexForwarded;
                else
                    ++awdlCoexForwardRefused;
            } else if (awdlCoexRx) {
                // Not AWDL's own format: a Block Ack request or teardown (a later phase of the port).
                ++awdlCoexForwardMalformed;
            }
        } else if (awdlFrame && (cf[0] & 0x0c) == 0x08) {
            ++awdlCoexAwdlData;
            // AWDL data heard by the listener.
            const size_t dataLen = mbuf_len(m);
            ++((cf[4] & 1) ? awdlCoexDataRxGroup : awdlCoexDataRxUcast);
            if (!(cf[4] & 1)) {
                if (cf[1] & 0x08)
                    ++awdlCoexDataRxRetry;
                ++awdlCoexRxModulation[(uint32_t(rate_n_flags) & IWX_RATE_MCS_MOD_TYPE_MSK) >> IWX_RATE_MCS_MOD_TYPE_POS];
                if ((uint32_t(rate_n_flags) & IWX_RATE_MCS_CHAN_WIDTH_MSK) == IWX_RATE_MCS_CHAN_WIDTH_80)
                    ++awdlCoexRxWide80;
            }
            if (awdlCoexDataRxOn && !mbuf_next(m)) {
                ++awdlCoexDataRx;
                ItlAWDL::DataFrameView view;
                if (!chanValid || unsigned(chanidx) != awdlCoexChannel) {
                    ++awdlCoexDataRxOffChannel;
                } else if (!awdlCoexPeerKnown(cf + 10, awdlUptimeUS())) {
                    ++awdlCoexDataRxUnknownPeer;
                    awdlCoexNoteRejectReason(cf + 10, awdlUptimeUS());
                } else {
                    // ForbiddenItlwm hands over only frames its IPv6 decoder accepts. awdl0's own input
                    // path here also takes QoS and A-MSDU subframes, so the decoder only counts.
                    if (ItlAWDL::parseDataFrame(cf, dataLen, false, &view) != ItlAWDL::ParseResult::Valid)
                        ++awdlCoexDataRxNotIPv6;
                    awdlCoexNoteDataFrom(cf + 10, awdlUptimeUS());
                    awdlCoexLastDataUS = awdlUptimeUS();
                    // The data handler takes the mbuf when it returns non-zero.
                    if (ic->ic_awdl_data_handler && ic->ic_awdl_data_handler(ic, m, rxi)) {
                        ++awdlCoexDataRxDelivered;
                        m = nullptr;
                    } else {
                        ++awdlCoexDataRxRefused;
                    }
                }
            }
        } else {
            ++awdlCoexOther;
        }
        if (++awdlCoexPublished % 512 == 0)
            awdlCoexPublish();
        // Frames with AWDL's BSSID are never the station's, whatever the filter: they stop at the hand-off.
        if ((awdlFrame || baToUs) && awdlCoexRx) {
            if (m)
                mbuf_freem(m);
            return true;
        }
    }
isolation:
    // With promiscuous receive (or a filter state the firmware may hold), what the listener hears goes no
    // further than the AWDL hand-off above, whatever its length. A frame is the listener's if the firmware's
    // MAC mask names the listener and not the station, or if it came in on the listener's channel while the
    // station is on another. A frame with neither usable is counted and left to the station.
    if (awdlCoexProbe && (awdlCoexPromiscLive || awdlCoexPromiscUncertain)) {
        struct iwx_node *stationNode = (struct iwx_node *)ic->ic_bss;
        const unsigned stationChannel = stationNode && stationNode->in_phyctxt && stationNode->in_phyctxt->channel ?
            unsigned(ieee80211_chan2ieee(ic, stationNode->in_phyctxt->channel)) : 0;
        const bool maskListenerOnly = awdlRxMacMaskValid && (awdlRxMacMask & (1U << kAwdlCoexMacId)) &&
            !(awdlRxMacMask & 1U);
        const bool channelListener = chanValid && stationChannel && unsigned(chanidx) == awdlCoexChannel &&
            stationChannel != awdlCoexChannel;
        if (maskListenerOnly || channelListener) {
            ++awdlCoexPromiscDropped;
            mbuf_freem(m);
            return true;
        }
        // A present but zero mask names no context, so it doesn't count as evidence.
        if (!chanValid && !(awdlRxMacMaskValid && awdlRxMacMask))
            ++awdlCoexPromiscAmbiguous;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------
// Phase 2: transmit from the listener (sync frames, single-frame data, rate ladder, send gate).
// ---------------------------------------------------------------------------------------------------

// The one channel check for the listener's sending, used before building and again just before sending.
// Records the reason (and clears it when the channel is fine) and publishes when what it would show changes.
static bool awdlCoexChannelEligible(struct ieee80211com *ic, struct ieee80211_channel *channel, const char *phase,
                                    int generation)
{
    const unsigned number = channel ? ieee80211_chan2ieee(ic, channel) : 0;
    const uint32_t flags = channel ? channel->ic_flags : 0;
    const uint32_t reason = (!channel || number > 255 || !ItlAWDL::fixedScheduleChannel(uint8_t(number)) ? 1U : 0U) |
        ((channel && awdlCoexNoIr(ic, number)) ? 2U : 0U) | ((flags & IEEE80211_CHAN_DFS) ? 4U : 0U) |
        ((flags & IEEE80211_CHAN_NOADHOC) ? 8U : 0U) |
        (!channel || number > 255 || !isset(ic->ic_chan_active, number) ? 16U : 0U) |
        (number != awdlFixedChannel ? 32U : 0U) |
        (awdlCoexBlockIneligible(ic, channel) ? 64U : 0U);
    const bool changed = reason != awdlCoexRefusedReason || (reason && flags != awdlCoexRefusedChannelFlags);
    awdlCoexRefusedReason = reason;
    if (reason) {
        awdlCoexRefusedChannelFlags = flags;
        ++awdlCoexTxRefusedChannel;
        snprintf(awdlCoexRefusalSnapshot, sizeof(awdlCoexRefusalSnapshot),
                 "%s channel %u expected %u flags 0x%x reason 0x%x generation %d", phase, number, awdlFixedChannel,
                 flags, reason, generation);
    }
    if (changed)
        awdlCoexPublish();
    return !reason;
}

// The listener's station and queue. The station has no peer address: like a monitor's, it sends to anyone,
// at fixed rates. With this firmware the queue goes when the station is removed.
int ItlIwx::
awdlCoexTxSetup(struct iwx_softc *sc)
{
    struct iwx_add_sta_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.sta_id = IWX_MONITOR_STA_ID;
    cmd.station_type = IWX_STA_GENERAL_PURPOSE;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(kAwdlCoexMacId, 0));
    IEEE80211_ADDR_COPY(&cmd.addr, etheranyaddr);
    cmd.tid_disable_tx = htole16(0xffff);
    cmd.station_flags = htole32(IWX_STA_FLG_FAT_EN_20MHZ | IWX_STA_FLG_MIMO_EN_SISO);
    cmd.station_flags_msk = htole32(IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK);
    // Who this setup is for. Each firmware command can sleep in the gate, and on return the listener
    // may be gone, the association changed, the firmware restarted or the driver detaching.
    const uint32_t token = awdlCoexToken;
    const int gen = sc->sc_generation;
    auto stillOurs = [&]() {
        return token == awdlCoexToken && gen == sc->sc_generation && awdlCoexAdded && !awdlStopping;
    };
    struct iwx_rm_sta_cmd rm;
    memset(&rm, 0, sizeof(rm));
    rm.sta_id = IWX_MONITOR_STA_ID;
    // Each attempt's diagnostics start fresh, all together.
    ++awdlCoexSetupAttempts;
    awdlCoexQueueStage = "not-tried";
    awdlCoexQueueAssigned = -1;
    awdlCoexQueueWritePointer = 0xffffffffU;
    awdlCoexStaResult = 0;
    awdlCoexQueueResult = 0;
    uint32_t status = IWX_ADD_STA_SUCCESS;
    const int transportErr = iwx_send_cmd_pdu_status(sc, IWX_ADD_STA, sizeof(cmd), &cmd, &status);
    int err = transportErr;
    if (!err && (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS)
        err = EIO;
    awdlCoexStaResult = err ? 0x80000000U | uint32_t(err) | (status << 8) | (transportErr ? 0x40000000U : 0U) : 1;
    if (err) {
        // A refusal in the response leaves nothing behind. A transport failure doesn't say, so in the same
        // firmware nothing is tried again until it restarts.
        if (transportErr && gen == sc->sc_generation) {
            ++awdlCoexTeardownErrors;
            awdlCoexBrokenGeneration = gen;
        }
        awdlCoexPublish();
        return err;
    }
    if (!stillOurs()) {
        // The station was added for a listener that is gone. Within the same firmware, remove it.
        ++awdlCoexSetupAborts;
        awdlCoexQueueStage = "aborted";
        if (gen == sc->sc_generation && iwx_send_cmd_pdu(sc, IWX_REMOVE_STA, 0, sizeof(rm), &rm) &&
            gen == sc->sc_generation) {
            ++awdlCoexTeardownErrors;
            awdlCoexBrokenGeneration = gen;
        }
        awdlCoexPublish();
        return ECANCELED;
    }
    awdlCoexQueueStage = "pending";
    awdlCoexSetupToken = token;
    awdlCoexSetupGen = gen;
    const int queue = iwx_tvqm_enable_txq(sc, IWX_MGMT_TID, 0, IWX_DEFAULT_QUEUE_SIZE, IWX_MONITOR_STA_ID);
    awdlCoexQueueResult = queue < 0 ? 0x80000000U | uint32_t(-queue) : uint32_t(queue);
    if (queue < 0) {
        // A failure before the firmware was asked leaves the stage at "pending".
        if (!strcmp(awdlCoexQueueStage, "pending"))
            awdlCoexQueueStage = "preflight";
        // The station, and any queue the firmware made for it, has to go. If removing it fails, the
        // firmware may still own both, so setup stops until the firmware restarts. After a restart
        // there is nothing to remove.
        if (gen == sc->sc_generation && iwx_send_cmd_pdu(sc, IWX_REMOVE_STA, 0, sizeof(rm), &rm) &&
            gen == sc->sc_generation) {
            ++awdlCoexTeardownErrors;
            awdlCoexBrokenGeneration = gen;
        }
        awdlCoexPublish();
        return -queue;
    }
    if (!stillOurs()) {
        // The queue exists, but its listener is gone. Remove the station (and with it the queue); free
        // the ring only if that worked, otherwise keep it as an orphan for the next reset.
        ++awdlCoexSetupAborts;
        awdlCoexQueueStage = "aborted";
        if (gen == sc->sc_generation) {
            if (iwx_send_cmd_pdu(sc, IWX_REMOVE_STA, 0, sizeof(rm), &rm)) {
                if (gen == sc->sc_generation) {
                    ++awdlCoexTeardownErrors;
                    awdlCoexBrokenGeneration = gen;
                    awdlCoexOrphanQueue = queue;
                }
            } else if (gen == sc->sc_generation) {
                iwx_reset_tx_ring(sc, &sc->txq[queue]);
            }
        }
        awdlCoexPublish();
        return ECANCELED;
    }
    awdlCoexQueue = queue;
    awdlCoexInFlight = false;
    awdlCoexTxHalted = false;
    // A new queue: no stall history or pause from an earlier one.
    awdlCoexLastCompletionUS = 0;
    awdlCoexTxPauseUntilUS = 0;
    awdlCoexStuckStreak = 0;
    memset(awdlCoexSlotKind, 0, sizeof(awdlCoexSlotKind));
    awdlCoexTxStaAdded = true;
    awdlCoexPublish();
    return 0;
}

// The listener's own frames are built by the AWDL interface (ic_awdl_coex_sync / ic_awdl_coex_pop) into
// this buffer, then copied into an mbuf for iwx_tx. Command gate only.
static uint8_t awdlCoexFrameBuf[2304];

static mbuf_t awdlCoexFrameMbuf(const uint8_t *bytes, size_t length)
{
    mbuf_t m = nullptr;
    if (!length || mbuf_getpacket(MBUF_DONTWAIT, &m) || !m)
        return nullptr;
    if (mbuf_maxlen(m) < length) {
        mbuf_freem(m);
        return nullptr;
    }
    memcpy(mbuf_data(m), bytes, length);
    mbuf_setlen(m, length);
    mbuf_pkthdr_setlen(m, length);
    return m;
}

void ItlIwx::
awdlCoexTxSend(struct iwx_softc *sc)
{
    struct ieee80211com *ic = &sc->sc_ic;
    struct iwx_node *in = (struct iwx_node *)ic->ic_bss;
    if (!in || !in->in_phyctxt || !in->in_phyctxt->channel)
        return;
    const int gen = sc->sc_generation;
    {
        // A channel our frames can't describe, or one the firmware won't let us start on, is refused
        // before anything is built. Checked again before sending.
        if (!awdlCoexChannelEligible(ic, awdlCoexMultiActive ? sc->sc_phyctxt[kAwdlCoexPhyId].channel :
                                     in->in_phyctxt->channel, "before-build", gen))
            return;
    }
    // The firmware ACKs only its context's address, so that has to be the one awdl0 sends from: nothing
    // goes out until the context has awdl0's address (awdlCoexAddrApply, from this task).
    if (!awdlCoexAddrKnown || !awdlCoexAddrApplied) {
        ++awdlCoexTxNotReady;
        return;
    }
    const size_t built = ic->ic_awdl_coex_sync ?
        ic->ic_awdl_coex_sync(ic, awdlCoexFrameBuf, sizeof(awdlCoexFrameBuf), awdlUptimeUS()) : 0;
    if (built < 24) {
        // Not ready: awdl0 doesn't exist yet or macOS hasn't sent its template.
        ++awdlCoexTxNotReady;
        return;
    }
    // A frame from another address than the firmware's context would never be acknowledged.
    if (!ItlAWDL::sameAddress(awdlCoexFrameBuf + 10, awdlCoexAddrInFirmware)) {
        ++awdlCoexTxBuildRejected;
        return;
    }
    mbuf_t packet = awdlCoexFrameMbuf(awdlCoexFrameBuf, built);
    if (!packet) {
        ++awdlCoexTxBuildRejected;
        return;
    }
    ++awdlCoexTxBuilt;
    // The firmware's regulatory decision stands: nothing is started on a channel it marks passive.
    in = (struct iwx_node *)ic->ic_bss;
    if (!in || !in->in_phyctxt ||
        !awdlCoexChannelEligible(ic, awdlCoexMultiActive ? sc->sc_phyctxt[kAwdlCoexPhyId].channel :
                                 in->in_phyctxt->channel, "before-send", gen)) {
        mbuf_freem(packet);
        return;
    }
    auto *node = ieee80211_ref_node(ic->ic_bss);
    awdlCoexSending = true;
    const int err = iwx_tx(sc, packet, node, EDCA_AC_BE, awdlCoexQueue);
    awdlCoexSending = false;
    if (err) {
        ieee80211_release_node(ic, node);
        ++awdlCoexTxSubmitErrors;
        return;
    }
    ++awdlCoexTxSubmitted;
    awdlCoexInFlight = true;
    awdlCoexInFlightUS = awdlUptimeUS();
}

// Sends what macOS queued on awdl0 out the listener's queue, keeping room in the ring for sync frames.
// Single frames only in this phase; the aggregate (Block Ack) path is a later phase of the port.
void ItlIwx::
awdlCoexDataPump(struct iwx_softc *sc, unsigned budget)
{
    struct ieee80211com *ic = &sc->sc_ic;
    const int gen = sc->sc_generation;
    struct iwx_node *in = (struct iwx_node *)ic->ic_bss;
    if (!awdlCoexData || !awdlCoexTxStaAdded || !awdlCoexAdded || awdlCoexGeneration != gen || awdlCoexTxHalted ||
        awdlStopping || awdlCoexQueue < 0 || awdlCoexQueue >= int(nitems(sc->txq)) || !in || !in->in_phyctxt ||
        ic->ic_state != IEEE80211_S_RUN || ic->ic_opmode != IEEE80211_M_STA || !ic->ic_awdl_coex_pop)
        return;
    if (awdlUptimeUS() < awdlCoexTxPauseUntilUS || !awdlCoexAddrApplied) {
        if (ic->ic_awdl_coex_service)
            ic->ic_awdl_coex_service(ic);
        return;
    }
    ++awdlCoexDataPumps;
    // The firmware's regulatory decision stands: nothing starts on a channel it doesn't allow.
    struct ieee80211_channel *listen = awdlCoexMultiActive ? sc->sc_phyctxt[kAwdlCoexPhyId].channel :
        in->in_phyctxt->channel;
    if (!awdlCoexChannelEligible(ic, listen, "data", gen)) {
        if (ic->ic_awdl_coex_service)
            ic->ic_awdl_coex_service(ic);
        return;
    }
    const struct iwx_tx_ring &ring = sc->txq[awdlCoexQueue];
    // Send gate; see awdlCoexGate. Frames for the schedule peer leave only while it is on our channel.
    bool gateClosed = false;
    if (awdlCoexGate && awdlCoexScheduleCurrent()) {
        const uint64_t gateNowUS = awdlUptimeUS();
        const bool dozing = awdlCoexPeerSchedule.presentEntries() <= ItlAWDL::kPeerDozeMaxEntries &&
            (!awdlCoexSchedPeerRxUS || gateNowUS - awdlCoexSchedPeerRxUS > kAwdlGateAwakeUS);
        const auto gate = awdlCoexPeerSchedule.sendGate(gateNowUS, dozing, kAwdlGateAwWindowUS);
        if (gate == ItlAWDL::PeerSchedule::SendGate::Open) {
            awdlCoexGateSinceUS = 0;
            awdlCoexGateKind = 0;
        } else {
            const uint8_t kind = uint8_t(gate);
            if (!awdlCoexGateSinceUS || kind != awdlCoexGateKind) {
                if (!awdlCoexGateSinceUS)
                    ++awdlCoexGateHolds;
                awdlCoexGateSinceUS = gateNowUS;
                awdlCoexGateKind = kind;
                // Away: through the away run (450 ms ceiling). Doze: until the next AW start, with 2 ms spare.
                awdlCoexGateLimitUS = gate == ItlAWDL::PeerSchedule::SendGate::Away ?
                    awdlCoexPeerSchedule.pauseLimitUS(gateNowUS) : awdlCoexPeerSchedule.awUS() + 2000;
            }
            if (gateNowUS - awdlCoexGateSinceUS < awdlCoexGateLimitUS) {
                gateClosed = true;
                if (gate == ItlAWDL::PeerSchedule::SendGate::Away)
                    ++awdlCoexGateAwayTicks;
                else
                    ++awdlCoexGateDozeTicks;
            } else if (awdlCoexGateLimitUS) {
                // The schedule still says no past its own limit: send rather than starve, until it opens.
                ++awdlCoexGateForced;
                awdlCoexGateLimitUS = 0;
            }
        }
    } else {
        awdlCoexGateSinceUS = 0;
        awdlCoexGateKind = 0;
    }
    while (budget && ring.queued < 64 && awdlCoexAdded && awdlCoexGeneration == sc->sc_generation && !awdlStopping) {
        // A closed gate holds every frame for the schedule peer; frames for others still go.
        const uint8_t *holdFor = gateClosed ? awdlCoexSchedPeer : nullptr;
        const size_t length = ic->ic_awdl_coex_pop(ic, awdlCoexFrameBuf, sizeof(awdlCoexFrameBuf), holdFor,
                                                   gateClosed ? 1 : 0);
        if (length < 24)
            break;
        --budget;
        mbuf_t next = awdlCoexFrameMbuf(awdlCoexFrameBuf, length);
        if (!next) {
            ++awdlCoexDataTxErrors;
            break;
        }
        auto *node = ieee80211_ref_node(ic->ic_bss);
        awdlCoexSending = true;
        awdlCoexSendingData = true;
        const int err = iwx_tx(sc, next, node, EDCA_AC_BE, awdlCoexQueue);
        awdlCoexSendingData = false;
        awdlCoexSending = false;
        if (err) {
            ieee80211_release_node(ic, node);
            ++awdlCoexDataTxErrors;
            continue;
        }
        ++awdlCoexDataTx;
        awdlCoexLastDataUS = awdlUptimeUS();
    }
    // Hands macOS back room in awdl0's queue, so its output keeps moving.
    if (ic->ic_awdl_coex_service)
        ic->ic_awdl_coex_service(ic);
}

// Transmit gate in iwx_tx for the listener's queue: only valid AWDL action frames, or (while the data pump
// submits) AWDL IPv6 data, in the gate, while the listener and its station exist. Frees m when refusing.
bool ItlIwx::
awdlCoexTxAllowed(struct iwx_softc *sc, mbuf_t m, int qid)
{
    ItlAWDL::ActionFrame decoded;
    ItlAWDL::DataFrameView data;
    const uint8_t *bytes = mtod(m, const uint8_t *);
    const bool ok = qid == awdlCoexQueue && awdlCoexQueue >= 0 && awdlCoexTx && awdlCoexSending && awdlCoexTxStaAdded && awdlCoexAdded &&
        awdlCoexGeneration == sc->sc_generation && getMainWorkLoop()->inGate() &&
        sc->sc_ic.ic_opmode == IEEE80211_M_STA && sc->sc_ic.ic_state == IEEE80211_S_RUN && !mbuf_next(m) &&
        mbuf_len(m) == mbuf_pkthdr_len(m) && mbuf_len(m) <= 2304 &&
        (awdlCoexSendingData ?
            awdlCoexData && ItlAWDL::parseDataFrame(bytes, mbuf_len(m), false, &data) == ItlAWDL::ParseResult::Valid :
            ItlAWDL::parseAction(bytes, mbuf_len(m), &decoded) == ItlAWDL::ParseResult::Valid);
    if (!ok)
        mbuf_freem(m);
    return ok;
}

// Rate for a frame on the listener's queue (iwx_tx_fill_cmd). The listener's station has no rate table: a
// fixed OFDM rate valid on either band. 6 Mbit/s for group and management frames, 24 Mbit/s for unicast
// data at step 0, then HT MCS 4/5/7 at 20 MHz, and VHT80 two streams MCS 4/7 at 80 MHz.
const struct iwx_rate *ItlIwx::
awdlCoexTxRate(struct iwx_softc *sc, struct iwx_node *in, struct ieee80211_frame *wh, uint32_t *flags,
               uint32_t *rate_n_flags, int *ridxOut)
{
    struct ieee80211_node *ni = &in->in_ni;
    const int type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
    const int ridx = (IEEE80211_IS_MULTICAST(wh->i_addr1) || type != IEEE80211_FC0_TYPE_DATA) ?
        IWX_RATE_6M_INDEX : IWX_RATE_24M_INDEX;
    *ridxOut = ridx;
    *flags |= IWX_TX_FLAGS_CMD_RATE;
    const uint8_t vhtCmdVer = iwx_lookup_cmd_ver(sc, IWX_LONG_GROUP, IWX_TX_CMD);
    const uint8_t bothAntennas = IWX_RATE_MCS_ANT_AB_MSK >> IWX_RATE_MCS_ANT_POS;
    if (ridx == IWX_RATE_24M_INDEX && awdlCoexRateStep >= 4 && awdlCoexRateStep < nitems(kAwdlCoexRateMcs) &&
        awdlCoexOwnWidth == 80 && vhtCmdVer > 8 && vhtCmdVer != IWX_FW_CMD_VER_UNKNOWN &&
        (iwx_fw_valid_tx_ant(sc) & bothAntennas) == bothAntennas) {
        // VHT, 80 MHz, two streams, as ForbiddenItlwm's dedicated mode sends at 80 MHz.
        *rate_n_flags = htole32(IWX_RATE_MCS_VHT_MSK | IWX_RATE_MCS_CHAN_WIDTH_80 | IWX_RATE_MCS_NSS_MSK |
                                IWX_RATE_MCS_ANT_AB_MSK | uint32_t(kAwdlCoexRateMcs[awdlCoexRateStep]));
        ++awdlCoexVhtFrames;
        return &iwx_rates[IWX_RATE_MCS_7_INDEX];
    }
    if (ridx == IWX_RATE_24M_INDEX && awdlCoexRateStep >= 4)
        awdlCoexRateStep = 3; // Not at 80 MHz any more, or no VHT command format: back to HT.
    if (ridx == IWX_RATE_24M_INDEX && awdlCoexRateStep > 0 && awdlCoexRateStep < 4) {
        // A fixed HT rate, 20 MHz, one stream, long guard interval.
        const unsigned mcs = kAwdlCoexRateMcs[awdlCoexRateStep];
        const iwx_rate *htInfo = &iwx_rates[iwx_mcs2ridx[mcs]];
        uint32_t htFlags = iwx_get_tx_ant(sc, ni, htInfo, type, wh);
        const uint8_t htCmdVer = iwx_lookup_cmd_ver(sc, IWX_LONG_GROUP, IWX_TX_CMD);
        if (htCmdVer > 8 && htCmdVer != IWX_FW_CMD_VER_UNKNOWN)
            htFlags |= IWX_RATE_MCS_HT_MSK | uint32_t(mcs & IWX_RATE_HT_MCS_CODE_MSK);
        else
            htFlags |= IWX_RATE_MCS_HT_MSK_V1 | uint32_t(mcs);
        *rate_n_flags = htole32(htFlags);
        return htInfo;
    }
    return nullptr;   // A legacy OFDM rate (ridxOut): the caller builds rate_n_flags as usual.
}

// TX response on the listener's queue (iwx_rx_tx_cmd). This completion frees slots from the tail up to,
// not including, idx; that has to be at least one and at most what is outstanding, anything else is a
// fault and nothing is freed for it. Returns false for such a fault (the caller then returns too).
bool ItlIwx::
awdlCoexTxDone(struct iwx_softc *sc, struct iwx_rx_packet *pkt, struct iwx_tx_resp *tx_resp,
               struct iwx_tx_ring *ring, int qid, int idx, uint32_t ssn)
{
    const int n = ring->ring_count;
    const int freed = n > 0 ? (idx - ring->tail + n) % n : 0;
    awdlCoexLastBoundary = idx;
    awdlCoexLastSsn = ssn;
    if (qid == awdlCoexQueue && n > 0 && freed >= 1 && freed <= ring->queued) {
        awdlCoexLastCompletionUS = awdlUptimeUS();
        awdlCoexStuckStreak = 0;
    }
    if (n <= 0 || freed < 1 || freed > ring->queued) {
        ++awdlCoexBadCompletions;
        snprintf(awdlCoexFaultSnapshot, sizeof(awdlCoexFaultSnapshot),
                 "bad completion queue %d token %u gen %d ssn %u tail %d queued %d pending %d status 0x%x",
                 qid, awdlCoexToken, sc->sc_generation, ssn, ring->tail, ring->queued, awdlCoexPendingSlot,
                 unsigned(le16toh(tx_resp->status.status)));
        awdlCoexPublish();
        return false;
    }
    // The status belongs to the descriptor the response header names (the index we put in the command
    // header). The SSN is only the reclaim boundary. Every reclaimed slot is cleared; only the named one
    // takes this status. A missing index identifies a candidate only when one descriptor was reclaimed.
    const int status = le16toh(tx_resp->status.status) & IWX_TX_STATUS_MSK;
    const bool ok = status == IWX_TX_STATUS_SUCCESS;
    int statusSlot = ItlAWDL::reclaimStatusSlot(ring->tail, freed, n, pkt->hdr.idx);
    const bool indexed = statusSlot >= 0;
    if (indexed) {
        ++awdlCoexStatusIdxHits;
    } else {
        ++awdlCoexStatusIdxMisses;
        if (freed == 1)
            statusSlot = ring->tail;
    }
    if (freed > 1)
        ++awdlCoexMultiReclaims;
    bool attributed = false;
    for (int k = 0; k < freed; ++k) {
        const int slot = (ring->tail + k) % n;
        const bool own = slot == statusSlot;
        if (qid == awdlCoexQueue && slot == awdlCoexPendingSlot && awdlCoexInFlight &&
            awdlCoexPendingGeneration == sc->sc_generation) {
            awdlCoexInFlight = false;
            awdlCoexPendingSlot = -1;
            if (own) {
                ++(ok ? awdlCoexTxOK : awdlCoexTxFailed);
                attributed = true;
            } else {
                ++awdlCoexSyncReclaimedUnknown; // Reclaimed; its own status isn't in this response.
            }
        }
        if (!awdlCoexDataTx || qid != awdlCoexQueue || slot >= int(nitems(awdlCoexSlotKind)) ||
            !awdlCoexSlotKind[slot])
            continue;
        const uint8_t kind = awdlCoexSlotKind[slot];
        awdlCoexSlotKind[slot] = 0;
        if (!own) {
            ++awdlCoexReclaimUnknown;
            continue;
        }
        attributed = true;
        ++(ok ? awdlCoexDataOK : awdlCoexDataFailed);
        if (kind == 2) {
            ++(ok ? awdlCoexUcastOK : awdlCoexUcastFailed);
            ++(ok ? awdlCoexRateWindowOK : awdlCoexRateWindowFailed);
            const uint32_t window = awdlCoexRateWindowOK + awdlCoexRateWindowFailed;
            if (window >= 64) {
                if (awdlCoexRateWindowFailed * 5 > window && awdlCoexRateStep > 0) {
                    --awdlCoexRateStep;
                    ++awdlCoexRateDowns;
                } else if (awdlCoexRateWindowFailed * 20 < window &&
                           awdlCoexRateStep + 1 < (awdlCoexOwnWidth == 80 ? nitems(kAwdlCoexRateMcs) : 4U)) {
                    ++awdlCoexRateStep;
                    ++awdlCoexRateUps;
                }
                awdlCoexRateWindowOK = awdlCoexRateWindowFailed = 0;
            }
        } else if (kind == 1) {
            ++(ok ? awdlCoexGroupOK : awdlCoexGroupFailed);
        }
    }
    if (!attributed)
        ++awdlCoexLateCompletions;
    return true;
}

// Slot bookkeeping in iwx_tx for a frame just placed on the listener's queue.
static void awdlCoexNoteSubmitted(struct iwx_softc *sc, int qid, int idx, const struct ieee80211_frame *wh)
{
    if (qid != awdlCoexQueue || awdlCoexQueue < 0)
        return;
    if (idx >= 0 && idx < int(nitems(awdlCoexSlotKind)))
        awdlCoexSlotKind[idx] = awdlCoexSendingData ? (IEEE80211_IS_MULTICAST(wh->i_addr1) ? 1 : 2) : 0;
    if (!awdlCoexSendingData) {
        awdlCoexPendingSlot = idx;
        awdlCoexLastSlot = idx;
        awdlCoexPendingGeneration = sc->sc_generation;
    }
}

// The listener's frames hold a node reference, released on completion; a ring reset has to release it too.
static bool awdlCoexOwnsRing(const struct iwx_tx_ring *ring)
{
    return (awdlCoexQueue >= 0 && ring->qid == awdlCoexQueue) ||
        (awdlCoexOrphanQueue >= 0 && ring->qid == awdlCoexOrphanQueue);
}
