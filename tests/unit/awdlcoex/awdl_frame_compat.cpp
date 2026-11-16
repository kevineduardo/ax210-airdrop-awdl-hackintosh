// SPDX-License-Identifier: GPL-2.0-only
// ax210-airdrop-awdl-hackintosh: our own test, GPL-2.0 because it includes GPL-2.0 driver headers.
// awdl0's data framing (AirportItlwmAWDLFrame.hpp, used by flushData and the coex backend's
// coexPopData) must pass ForbiddenItlwm's decoder (AWDLDataFrame.hpp): the coex backend's transmit
// gate in iwx_tx drops every data frame the decoder refuses.
#include <AirportItlwmAWDLFrame.hpp>
#include <AWDLDataFrame.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

// An Ethernet frame carrying an IPv6 packet with `payload` bytes after the IPv6 header.
size_t ethernetIPv6(uint8_t *out, const uint8_t dst[6], const uint8_t src[6], size_t payload, uint16_t type = 0x86dd)
{
    memcpy(out, dst, 6);
    memcpy(out + 6, src, 6);
    out[12] = uint8_t(type >> 8);
    out[13] = uint8_t(type & 0xff);
    uint8_t *ip = out + 14;
    memset(ip, 0, 40 + payload);
    ip[0] = 0x60;
    ip[4] = uint8_t(payload >> 8);
    ip[5] = uint8_t(payload & 0xff);
    ip[6] = 17;     // UDP
    ip[7] = 64;
    for (size_t i = 0; i < payload; ++i)
        ip[40 + i] = uint8_t(i * 7);
    return 14 + 40 + payload;
}

// What flushData and coexPopData do: header, then the Ethernet payload.
size_t frame(uint8_t *out, const uint8_t *eth, size_t ethLen, const uint8_t mac[6], uint16_t wifiSeq, uint16_t awdlSeq)
{
    AirportAWDLFrame::writeDataHeader(out, eth, mac, wifiSeq, awdlSeq);
    memcpy(out + AirportAWDLFrame::kDataHeaderLen, eth + 14, ethLen - 14);
    return AirportAWDLFrame::frameLength(ethLen);
}

} // namespace

int main()
{
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t peer[6] = {0x06, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    const uint8_t mcast[6] = {0x33, 0x33, 0x00, 0x00, 0x00, 0xfb};
    uint8_t eth[1600], wifi[1700];

    // Unicast IPv6, full MTU.
    size_t ethLen = ethernetIPv6(eth, peer, mac, 1460);
    size_t len = frame(wifi, eth, ethLen, mac, 0x123, 0xbeef);
    assert(len == ethLen - 14 + 40);
    ItlAWDL::DataFrameView view;
    assert(ItlAWDL::parseDataFrame(wifi, len, false, &view) == ItlAWDL::ParseResult::Valid);
    assert(!memcmp(view.source, mac, 6));
    assert(!memcmp(view.destination, peer, 6));
    assert(view.wifiSequence == 0x123);
    assert(view.ip.sequence == 0xbeef);
    assert(view.ip.length == 1500);
    assert(!memcmp(view.ip.bytes, eth + 14, view.ip.length));
    // The decoder rebuilds the Ethernet frame we started from.
    uint8_t back[1600];
    size_t written = 0;
    assert(ItlAWDL::copyDataEthernet(view, back, sizeof(back), &written) == ItlAWDL::BuildResult::Ready);
    assert(written == ethLen && !memcmp(back, eth, ethLen));
    // With four zero FCS bytes, as ForbiddenItlwm's monitor path hands frames over.
    memset(wifi + len, 0, 4);
    assert(ItlAWDL::parseDataFrame(wifi, len + 4, true, &view) == ItlAWDL::ParseResult::Valid);

    // Group-addressed (mDNS), smallest IPv6 packet.
    ethLen = ethernetIPv6(eth, mcast, mac, 0);
    len = frame(wifi, eth, ethLen, mac, 0xfff, 0);
    assert(ItlAWDL::parseDataFrame(wifi, len, false, &view) == ItlAWDL::ParseResult::Valid);
    assert(!memcmp(view.destination, mcast, 6));
    assert(view.wifiSequence == 0xfff);

    // The sequence counters wrap as the drivers' do: only the low 12 / 16 bits are sent.
    ethLen = ethernetIPv6(eth, peer, mac, 100);
    len = frame(wifi, eth, ethLen, mac, uint16_t(0x1001), uint16_t(0x10000 + 5));
    assert(ItlAWDL::parseDataFrame(wifi, len, false, &view) == ItlAWDL::ParseResult::Valid);
    assert(view.wifiSequence == 0x001 && view.ip.sequence == 5);

    // Anything but IPv6 is not accepted by the coex transmit gate (awdl0 only carries IPv6).
    ethLen = ethernetIPv6(eth, peer, mac, 100, 0x0800);
    len = frame(wifi, eth, ethLen, mac, 1, 1);
    assert(ItlAWDL::parseDataFrame(wifi, len, false, &view) != ItlAWDL::ParseResult::Valid);

    // A frame shorter than an Ethernet header has no 802.11 length.
    assert(AirportAWDLFrame::frameLength(13) == 0);

    std::puts("AWDL data framing vs ForbiddenItlwm's decoder: passed");
    return 0;
}
