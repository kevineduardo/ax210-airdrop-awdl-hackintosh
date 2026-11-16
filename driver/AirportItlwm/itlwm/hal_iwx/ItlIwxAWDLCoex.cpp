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

static void awdlStopTimers()
{
    // Set first; every arm site checks it inside the command gate, and the frees below run in the gate,
    // so work that was already past its check finishes, and its timer is freed after it.
    awdlStopping = true;
}

// Called from iwx_attach before the AWDL callbacks are installed. The front end decided (ic_awdl_coex_on).
void ItlIwx::
awdlCoexAttach(struct iwx_softc *sc)
{
    // A new attach in the same loaded image starts with its timers allowed again.
    awdlStopping = false;
    awdlCoexSelected = sc->sc_ic.ic_awdl_coex_on != 0;
    awdlCoexFwPublishedGen = -1;
    if (!awdlCoexSelected)
        return;
    XYLog("%s: AWDL coex backend selected; P2P listen path disabled\n", DEVNAME(sc));
    if (IOEthernetController *ctl = getController())
        ctl->setProperty("ITLAWDLCoexSelected", true);
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
