// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop port to
// macOS 15 Sequoia and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  AirportItlwmAWDLConfig.h
//  AirportItlwm
//
//  Compile-time switch for the experimental awdl0 support (crazyhulk, ported to
//  Sequoia). The code is built for both WCL targets; at run time everything stays
//  off unless the -itlawdl boot argument is present.
//
//  AIRPORT_AWDL              AWDL code is compiled in (Sequoia 15.2+ and Tahoe 26).
//  AIRPORT_AWDL_ABI_SEQUOIA  IO80211Family ABI of macOS 15 (verified on 15.8.1 24H32).
//  AIRPORT_AWDL_ABI_TAHOE    IO80211Family ABI of macOS 26 (crazyhulk's original).
//

#ifndef AirportItlwmAWDLConfig_h
#define AirportItlwmAWDLConfig_h

#if defined(AIRPORT_WCL)
#define AIRPORT_AWDL 1
#if __IO80211_TARGET == __MAC_15_2
#define AIRPORT_AWDL_ABI_SEQUOIA 1
#else
#define AIRPORT_AWDL_ABI_TAHOE 1
#endif
#endif

#endif /* AirportItlwmAWDLConfig_h */
