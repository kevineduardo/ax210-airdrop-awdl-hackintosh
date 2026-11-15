// Modified by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop on macOS 15
// Sequoia and 26 Tahoe, and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#ifndef ItlDriverInfo_h
#define ItlDriverInfo_h

class ItlDriverInfo {
    
public:
    
    virtual const char *getFirmwareVersion() = 0;
    
    virtual int16_t getBSSNoise() = 0;

    // Channel load (CCA busy, percent) from the firmware statistics, -1 when unknown.
    virtual int getChannelLoad() { return -1; }
    // Ask the firmware for a fresh statistics report (asynchronous, no-op when unsupported).
    virtual void requestStatistics() {}
    // Diagnostics for the two calls above: samples, requests, last error, last deltas.
    virtual void getStatisticsInfo(char *buf, size_t len) { if (len) buf[0] = 0; }
    
    virtual bool is5GBandSupport() = 0;
    
    virtual int getTxNSS() = 0;
    
    virtual const char *getFirmwareName() = 0;
    
    virtual UInt32 supportedFeatures() = 0;

    virtual const char *getFirmwareCountryCode() = 0;

    virtual uint32_t getTxQueueSize() = 0;
};

#endif /* ItlDriverInfo_h */
