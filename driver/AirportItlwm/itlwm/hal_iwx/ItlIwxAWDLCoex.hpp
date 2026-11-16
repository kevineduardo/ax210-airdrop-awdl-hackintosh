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

// The engine is selected for this boot (ic_awdl_coex_on at attach). Read by the P2P path's guards.
static bool awdlCoexSelected;
// Set at detach: AWDL timers and tasks stop re-arming.
static bool awdlStopping;
// The firmware generation whose capabilities were last published.
static int awdlCoexFwPublishedGen = -1;

static uint64_t awdlUptimeUS();
static void awdlStopTimers();

#endif /* ItlIwxAWDLCoex_hpp */
