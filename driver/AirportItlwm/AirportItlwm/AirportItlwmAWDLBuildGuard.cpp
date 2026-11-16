// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop port to
// macOS 15 Sequoia and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  AirportItlwmAWDLBuildGuard.cpp
//  AirportItlwm
//
//  Ported from ForbiddenItlwm's AWDLBuildGuard by retropineaple, GPL-2.0.
//
//  Decides once whether AWDL may start on this macOS build. Two checks:
//  1. Apple's IO80211AWDLProtocol vtable, which AirportItlwmAWDLInterface subclasses slot for
//     slot, still has the expected functions at spread-out anchor slots, its own pure virtual
//     methods where expected, and ends where expected (568 slots on Tahoe, 563 on Sequoia).
//     This catches a changed table on any build.
//  2. The running kernel is on the verified list (AirportItlwmAWDLBuildGuard.hpp).
//  -itlawdlanybuild skips check 2, never check 1. When AWDL may not start, the driver behaves
//  as if -itlawdl was not set: normal Wi-Fi works and nothing AWDL-related is created or sent.
//
//  This file must not include the IO80211 headers: the anchors below are declared under the
//  same mangled names as methods those headers declare.
//

// AirportItlwmAWDLConfig.h compares __IO80211_TARGET with __MAC_15_2; both come from the
// target's build settings and prefix header (itlwm/PrivateSPI.pch).
#include <IOKit/IOLib.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>
#include "AirportItlwmAWDLConfig.h"

#if defined(AIRPORT_AWDL)

#include "AirportItlwmAWDLBuildGuard.hpp"

// Every symbol below is defined by IO80211Family, IOSkywalkFamily or the kernel, which are in
// OSBundleLibraries of both WCL Info.plists. Only the address is used; nothing is called.
extern "C" {
extern const uintptr_t airportAWDLProtocolVtable[] __asm__("__ZTV19IO80211AWDLProtocol");
void airportAnchorAWDLProtocolD1() __asm__("__ZN19IO80211AWDLProtocolD1Ev");
void airportAnchorWakeFlags() __asm__("__ZN25IOSkywalkNetworkInterface21getSupportedWakeFlagsEPj");
void airportAnchorInterfaceCCA() __asm__("__ZN23IO80211SkywalkInterface15setInterfaceCCAE18apple80211_channeli");
void airportAnchorStatsDump() __asm__("__ZN23IO80211SkywalkInterface9statsDumpEv");
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
void airportAnchorVirtualInit() __asm__("__ZN23IO80211VirtualInterface4initEP17IO80211ControllerP10ether_addrjPKc");
#else
void airportAnchorSyncDPSStats() __asm__("__ZN23IO80211SkywalkInterface12syncDPSStatsEP26apple82011_postMessage_dps");
#endif
void airportAnchorReserved15() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface15Ev");
void airportAnchorPureVirtual() __asm__("___cxa_pure_virtual");
}

namespace {

// Slot numbers count from the address point (the vtable symbol plus two words: offset-to-top
// and RTTI). Anchors are checked in ascending order and the check stops at the first
// mismatch, so a shorter table is read as little as possible past its first difference.
struct Anchor {
    unsigned slot;
    uintptr_t expected;
};

int gAllowed = -1;
const char *gStatus = "not-checked";

uintptr_t addr(void (*fn)())
{
    return reinterpret_cast<uintptr_t>(fn);
}

bool vtableMatches()
{
    const uintptr_t *slots = airportAWDLProtocolVtable + 2;
    const uintptr_t pure = addr(&airportAnchorPureVirtual);
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    // 563 slots; IO80211VirtualInterface::_RESERVED15 at 0xf98, then 63 AWDL pure virtuals
    // at 0xfa0..0x1190 (include/Airport/macOS15/IO80211AWDLProtocol.h).
    const Anchor anchors[] = {
        {0, addr(&airportAnchorAWDLProtocolD1)},
        {299, addr(&airportAnchorWakeFlags)},
        {380, addr(&airportAnchorInterfaceCCA)},
        {415, pure},                    // setMacAddress, pure in IO80211SkywalkInterface here
        {420, addr(&airportAnchorStatsDump)},
        {460, addr(&airportAnchorVirtualInit)},
        {499, addr(&airportAnchorReserved15)},
        {500, pure},
        {562, pure},
        {563, 0},
    };
#else
    // 568 slots; _RESERVED15 at 0xfc0, then 63 AWDL pure virtuals at 0xfc8..0x11b8
    // (include/Airport/macOS26/IO80211AWDLProtocol.h, docs/test-results-tahoe.md).
    const Anchor anchors[] = {
        {0, addr(&airportAnchorAWDLProtocolD1)},
        {300, addr(&airportAnchorWakeFlags)},
        {380, addr(&airportAnchorInterfaceCCA)},
        {420, addr(&airportAnchorStatsDump)},
        {460, addr(&airportAnchorSyncDPSStats)},
        {504, addr(&airportAnchorReserved15)},
        {505, pure},
        {567, pure},
        {568, 0},
    };
#endif
    for (const Anchor &a : anchors)
        if (slots[a.slot] != a.expected)
            return false;
    return true;
}

} // namespace

bool airportAWDLBuildAllowed()
{
    if (gAllowed >= 0)
        return gAllowed == 1;
    int dummy = 0;
    const bool anyBuild = PE_parse_boot_argn("-itlawdlanybuild", &dummy, sizeof(dummy));
    // kern.osversion is usually still empty when the driver starts, so only the kernel is
    // checked here; the build lists in the header are for callers that have it.
    const AirportAWDLGuard::BuildCheck build = AirportAWDLGuard::checkBuild(version, nullptr);
    if (!vtableMatches()) {
        gStatus = "blocked: IO80211AWDLProtocol vtable differs";
        gAllowed = 0;
    } else if (build == AirportAWDLGuard::BuildCheck::Verified) {
        gStatus = "verified";
        gAllowed = 1;
    } else if (anyBuild) {
        gStatus = "allowed by -itlawdlanybuild: unverified macOS kernel";
        gAllowed = 1;
    } else {
        gStatus = "blocked: unverified macOS kernel";
        gAllowed = 0;
    }
    IOLog("itlwm: AWDL build guard: %s\n", gStatus);
    return gAllowed == 1;
}

const char *airportAWDLBuildStatus()
{
    airportAWDLBuildAllowed();
    return gStatus;
}

bool airportAWDLEnabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        int v = 0;
        enabled = PE_parse_boot_argn("-itlawdl", &v, sizeof(v)) && airportAWDLBuildAllowed() ? 1 : 0;
    }
    return enabled == 1;
}

bool airportAWDLCoexEnabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        int v = 0;
        enabled = airportAWDLEnabled() && PE_parse_boot_argn("-itlawdlcoex", &v, sizeof(v)) ? 1 : 0;
    }
    return enabled == 1;
}

const char *airportAWDLBackendName()
{
    if (!airportAWDLEnabled())
        return "none";
    if (airportAWDLCoexEnabled())
        return "coex";
    int v = 0;
    // Without -itlawdlp2p the HAL listens with aux remain-on-channel or scan dwells.
    return PE_parse_boot_argn("-itlawdlp2p", &v, sizeof(v)) ? "p2p" : "scan";
}

#endif /* AIRPORT_AWDL */
