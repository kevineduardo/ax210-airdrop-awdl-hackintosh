// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop port to
// macOS 15 Sequoia and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  IO80211AWDLProtocol.h
//  AirportItlwm
//
//  Version-selecting shim for IO80211AWDLProtocol (WCL targets only).
//  Sequoia (__MAC_15_2): include/Airport/macOS15, verified against 15.8.1 24H32.
//  Tahoe  (__MAC_26_0): include/Airport/macOS26 (crazyhulk).
//

#ifndef IO80211AWDLProtocolShim_h
#define IO80211AWDLProtocolShim_h

#ifdef AIRPORT_WCL
#include "IO80211VirtualInterfaceWCL.h"
#if __IO80211_TARGET == __MAC_15_2
#include "macOS15/IO80211AWDLProtocol.h"
#else
#include "macOS26/IO80211AWDLProtocol.h"
#endif
#endif /* AIRPORT_WCL */

#endif /* IO80211AWDLProtocolShim_h */
