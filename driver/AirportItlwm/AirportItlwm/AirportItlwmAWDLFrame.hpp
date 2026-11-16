// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL coex backend, -itlawdlcoex).
// See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  AirportItlwmAWDLFrame.hpp
//  AirportItlwm
//
//  The 802.11 header of an AWDL data frame as awdl0 sends it, shared by the P2P path (flushData)
//  and the coex backend (coexPopData). Header-only and freestanding (no kernel headers), so the
//  host test tests/unit/awdlcoex/awdl_frame_compat.cpp checks it against ForbiddenItlwm's decoder,
//  which the coex backend's transmit gate uses.
//
//  Layout, as OWL's awdl_init_full_data_frame: 802.11 data header (no QoS), LLC/SNAP
//  aa aa 03 00 17 f2 08 00, AWDL data header 03 04, sequence (little endian), 2 pad bytes,
//  the ethertype (big endian, from the Ethernet header), then the Ethernet payload.
//

#ifndef AirportItlwmAWDLFrame_hpp
#define AirportItlwmAWDLFrame_hpp

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace AirportAWDLFrame {

constexpr size_t kWifiHeaderLen = 24;
constexpr size_t kLlcLen = 8;
constexpr size_t kAwdlHeaderLen = 8;
constexpr size_t kDataHeaderLen = kWifiHeaderLen + kLlcLen + kAwdlHeaderLen;    // 40
constexpr size_t kEthernetHeaderLen = 14;
static const uint8_t kBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

// Writes the kDataHeaderLen bytes in front of the payload. eh is the frame's Ethernet header
// (destination, source, ethertype); src is awdl0's address; wifiSeq is the 802.11 sequence
// number (12 bits), awdlSeq the AWDL data sequence number. The caller appends
// (Ethernet length - 14) payload bytes right after.
inline void writeDataHeader(uint8_t *frame, const uint8_t eh[kEthernetHeaderLen], const uint8_t src[6],
                            uint16_t wifiSeq, uint16_t awdlSeq)
{
    memset(frame, 0, kDataHeaderLen);
    frame[0] = 0x08;                        // version 0, data, subtype data
    memcpy(frame + 4, eh, 6);               // addr1: destination
    memcpy(frame + 10, src, 6);             // addr2: awdl0
    memcpy(frame + 16, kBssid, 6);          // addr3: AWDL BSSID
    const uint16_t sc = uint16_t(wifiSeq << 4);
    frame[22] = uint8_t(sc & 0xff);
    frame[23] = uint8_t(sc >> 8);
    uint8_t *l = frame + kWifiHeaderLen;
    l[0] = 0xaa; l[1] = 0xaa; l[2] = 0x03;
    l[3] = 0x00; l[4] = 0x17; l[5] = 0xf2;
    l[6] = 0x08; l[7] = 0x00;
    uint8_t *a = l + kLlcLen;
    a[0] = 0x03; a[1] = 0x04;
    a[2] = uint8_t(awdlSeq & 0xff); a[3] = uint8_t(awdlSeq >> 8);
    a[6] = eh[12]; a[7] = eh[13];
}

// Length of the 802.11 frame for an Ethernet frame of ethLen bytes (0 if it has no header).
inline size_t frameLength(size_t ethLen)
{
    return ethLen < kEthernetHeaderLen ? 0 : kDataHeaderLen + ethLen - kEthernetHeaderLen;
}

} // namespace AirportAWDLFrame

#endif /* AirportItlwmAWDLFrame_hpp */
