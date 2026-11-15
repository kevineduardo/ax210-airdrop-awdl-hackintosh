// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop port to
// macOS 15 Sequoia and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  IO80211VirtualInterfaceWCL.h
//  AirportItlwm
//
//  Version-selecting shim for the IO80211FamilyV2 (WCL) IO80211VirtualInterface.
//  Named *WCL so it does not collide with the legacy, pre-Skywalk
//  include/Airport/IO80211VirtualInterface.h used by the non-WCL targets.
//

#ifndef IO80211VirtualInterfaceWCL_h
#define IO80211VirtualInterfaceWCL_h

#ifdef AIRPORT_WCL
#if __IO80211_TARGET == __MAC_15_2
#include "macOS15/IO80211VirtualInterface.h"
#else
#include "macOS26/IO80211VirtualInterface.h"
#endif
#endif /* AIRPORT_WCL */

#endif /* IO80211VirtualInterfaceWCL_h */
