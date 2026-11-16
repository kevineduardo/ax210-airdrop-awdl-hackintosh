// Modified by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop port to
// macOS 15 Sequoia and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  AirportItlwmAWDLInterface.cpp
//  AirportItlwm
//
//  awdl0 for macOS 26 (Tahoe) and macOS 15 (Sequoia, AIRPORT_AWDL_ABI_SEQUOIA),
//  stage 1: accept and log the AWDL control plane.
//

#include "AirportItlwmV2.hpp"
#include "AirportItlwmAWDLInterface.hpp"
#include "AirportItlwmAWDLBuildGuard.hpp"
#include "AirportItlwmSkywalkInterface.hpp"
#include <IOKit/IOUserClient.h>

#if defined(AIRPORT_AWDL)

extern IOCommandGate *_fCommandGate;

OSDefineMetaClassAndStructors(AirportItlwmAWDLInterface, IO80211AWDLProtocol);
OSDefineMetaClassAndStructors(AirportItlwmAWDLEthernetInterface, AirportItlwmEthernetInterface);

#define AWDL_LOG(fmt, ...) XYLog("AWDL %s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__)

#pragma mark - awdl0 BSD interface

const char *AirportItlwmAWDLEthernetInterface::
getNamePrefix() const
{
    return "awdl";
}

bool AirportItlwmAWDLEthernetInterface::
initIfnetParams(struct ifnet_init_params *params)
{
    // Publish awdl0's own address before IOEthernetInterface reads it as the
    // ifnet uniqueid; en6 already owns the station MAC.
    setProperty(kIOMACAddress, (void *)&fMac, kIOEthernetAddressSize);
    if (!AirportItlwmEthernetInterface::initIfnetParams(params))
        return false;
    params->uniqueid = &fMac;
    params->uniqueid_len = sizeof(fMac);
    return true;
}

IOReturn AirportItlwmAWDLEthernetInterface::
attachToDataLinkLayer(IOOptionBits options, void *parameter)
{
    IOReturn ret = AirportItlwmEthernetInterface::attachToDataLinkLayer(options, parameter);
    if (ret == kIOReturnSuccess && getIfnet()) {
        errno_t err = ifnet_set_lladdr(getIfnet(), &fMac, sizeof(fMac));
        AWDL_LOG("lladdr %s err %d", ether_sprintf(fMac.octet), err);
        // MTU stays 1500: the iPhone sends 1497-byte IPv6 packets on AWDL (QUIC PMTU) and a
        // 1484 MTU here made the AirDrop transfer stall for ~20 s (tested 2026-11-12).
        // Bring awdl0 up from a thread call (not from this attach path, which may hold
        // the network stack or the command gate), 1 s later. -itlawdlnoup disables it.
        int v;
        if (!PE_parse_boot_argn("-itlawdlnoup", &v, sizeof(v))) {
            if (!fBringUp)
                fBringUp = thread_call_allocate(bringUpCall, this);
            if (fBringUp) {
                uint64_t deadline;
                clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
                retain();
                if (thread_call_enter_delayed(fBringUp, deadline))
                    release();   // already pending, it holds its own reference
            }
        }
    }
    return ret;
}

void AirportItlwmAWDLEthernetInterface::
bringUpCall(thread_call_param_t p0, thread_call_param_t)
{
    auto self = static_cast<AirportItlwmAWDLEthernetInterface *>(p0);
    ifnet_t ifp = self->getIfnet();
    if (ifp && self->fOwner && !(ifnet_flags(ifp) & IFF_UP)) {
        ifnet_set_flags(ifp, IFF_UP, IFF_UP);
        // Same request ifconfig/configd would send; performCommand turns it into
        // setAWDLLinkGated(up) on the command gate.
        errno_t err = ifnet_ioctl(ifp, 0, SIOCSIFFLAGS, nullptr);
        XYLog("AWDL %s: awdl0 up by driver, err %d, flags 0x%x\n", __FUNCTION__, err, ifnet_flags(ifp));
    }
    self->release();
}

void AirportItlwmAWDLEthernetInterface::
free()
{
    if (fBringUp) {
        thread_call_cancel(fBringUp);
        thread_call_free(fBringUp);
        fBringUp = nullptr;
    }
    AirportItlwmEthernetInterface::free();
}

SInt32 AirportItlwmAWDLEthernetInterface::
performCommand(IONetworkController *controller, unsigned long cmd, void *arg0, void *arg1)
{
    // The default SIOCSIFFLAGS path calls controller->enable(this), which would
    // reinitialise the shared radio. The flags are already set on the ifnet.
    if (cmd == SIOCSIFFLAGS) {
        AWDL_LOG("flags 0x%x", ifnet_flags(getIfnet()));
        // Link state is what starts IO80211AWDLPeerManager::configure(). It has
        // to run in the gate but off the work queue thread.
        uintptr_t up = (ifnet_flags(getIfnet()) & IFF_UP) != 0;
        if (_fCommandGate)
            _fCommandGate->runAction(AirportItlwm::setAWDLLinkGated, (void *)up);
        return 0;
    }
    return AirportItlwmEthernetInterface::performCommand(controller, cmd, arg0, arg1);
}

static bool awDataAllowed();
static uint64_t hostUs();

int AirportItlwmAWDLEthernetInterface::
inputAWDLData(mbuf_t m, AirportItlwmAWDLInterface *awdl)
{
    if (!awDataAllowed() || !getIfnet()) {
        return 0;
    }
    if (mbuf_pullup(&m, mbuf_pkthdr_len(m) < 64 ? mbuf_pkthdr_len(m) : 64) != 0 || !m) {
        awdl->fDataRxBad++;
        return 1;   // mbuf_pullup freed it
    }
    const uint8_t *p = (const uint8_t *)mbuf_data(m);
    size_t len = mbuf_len(m);
    const struct ieee80211_frame *wh = (const struct ieee80211_frame *)p;
    if (len < sizeof(*wh)) {
        awdl->fDataRxBad++;
        mbuf_freem(m);
        return 1;
    }
    // Null / QoS-Null (power save, keep-alive): no payload, nothing to deliver.
    if (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_NODATA) {
        awdl->fDataRxNull++;
        mbuf_freem(m);
        return 1;
    }
    // Full 802.11 header length: QoS control, a fourth address and, with the Order bit
    // on a QoS frame, the 4-byte HT Control field. HE peers (iPhone) start adding HT
    // Control once data is flowing; the previous fixed 24(+2) made every such frame
    // fail the SNAP check below (19 % of the data frames in the 2026-11-12 Tahoe tests,
    // seen as multi-second stalls of the AirDrop QUIC transfer).
    size_t hdr = ieee80211_get_hdrlen(wh);
    if (ieee80211_has_htc(wh))
        awdl->fDataRxHtc++;
    if (ieee80211_has_qos(wh) && len >= hdr) {
        size_t qoff = sizeof(*wh) + (ieee80211_has_addr4(wh) ? IEEE80211_ADDR_LEN : 0);
        uint16_t qos = p[qoff] | (p[qoff + 1] << 8);
        if (qos & IEEE80211_QOS_AMSDU) {
            // The hal clears this bit on hardware-deaggregated subframes; if it is
            // still set the payload is a raw A-MSDU. Count and drop.
            awdl->fDataRxAmsdu++;
            mbuf_freem(m);
            return 1;
        }
    }
    if ((wh->i_fc[1] & IEEE80211_FC1_MORE_FRAG) || (wh->i_seq[0] & 0x0f))
        awdl->fDataRxFrag++;
    static const uint8_t kSnap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2 };
    const size_t llc = 8, awdlHdr = 8;
    // A-MSDU subframes. The Intel hardware de-aggregates A-MSDUs and hands every subframe
    // over as its own MPDU. It only strips the subframe header when the payload starts with
    // an RFC 1042 / bridge-tunnel SNAP; AWDL uses Apple's OUI (00:17:f2), so the subframe
    // comes out as an 802.3 length frame wrapped in RFC 1042: aa aa 03 00 00 00 <len16>,
    // followed by the real AWDL LLC. Example from a 25G332 capture (129-byte frame):
    //   8800 2c00 <a1> <a2> <a3> 300e 0000 | aaaa03000000 005f | aaaa030017f20800 0304d700 0000 86dd | 60...
    // The iPhone aggregates as soon as traffic flows, so without this 19-50 % of the data
    // frames were dropped (AWDLDataRxBad) and the AirDrop QUIC transfer stalled for seconds.
    static const uint8_t kRfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    if (len >= hdr + 8 + llc + awdlHdr && !memcmp(p + hdr, kRfc1042, sizeof(kRfc1042)) &&
        !memcmp(p + hdr + 8, kSnap, sizeof(kSnap))) {
        size_t inner = ((size_t)p[hdr + 6] << 8) | p[hdr + 7];
        if (inner <= mbuf_pkthdr_len(m) - hdr - 8) {
            awdl->fDataRxAmsduSub++;
            hdr += 8;
        }
    }
    if (len < hdr + llc + awdlHdr || memcmp(p + hdr, kSnap, sizeof(kSnap)) ||
        p[hdr + 6] != 0x08 || p[hdr + 7] != 0x00) {
        awdl->fDataRxBad++;
        // Keep the start of the frame so the cause can be read from ioreg.
        awdl->fDataRxBadLen = (uint32_t)mbuf_pkthdr_len(m);
        size_t n = len < sizeof(awdl->fDataRxBadHead) ? len : sizeof(awdl->fDataRxBadHead);
        memcpy(awdl->fDataRxBadHead, p, n);
        awdl->fDataRxBadHeadLen = (uint8_t)n;
        if (awdl->fDataRxBad <= 4 || (awdl->fDataRxBad & 0x1f) == 0)
            awdl->publishDataState();
        mbuf_freem(m);
        return 1;
    }
    const uint8_t *ad = p + hdr + llc;
    uint8_t ethType[2] = { ad[6], ad[7] };
    uint8_t dst[6], src[6];
    memcpy(dst, wh->i_addr1, 6);
    memcpy(src, wh->i_addr2, 6);

    // Keep 14 bytes of the stripped headers for the Ethernet header.
    size_t strip = hdr + llc + awdlHdr - 14;
    mbuf_adj(m, (int)strip);
    uint8_t *eh = (uint8_t *)mbuf_data(m);
    memcpy(eh, dst, 6);
    memcpy(eh + 6, src, 6);
    eh[12] = ethType[0];
    eh[13] = ethType[1];
    mbuf_pkthdr_setrcvif(m, getIfnet());
    awdl->fDataRx++;
    if (!(dst[0] & 1)) {
        awdl->noteDataPeer(src);
        awdl->fDataRxUcUs = hostUs();
        awdl->notePeerDataRx(src, awdl->fDataRxUcUs);
    }
    else
        awdl->fDataRxMc++;
    awdl->fDataRxLastLen = (uint32_t)mbuf_pkthdr_len(m);
    awdl->fDataRxLastType = (uint16_t)((ethType[0] << 8) | ethType[1]);
    if (awdl->fDataRx <= 4 || (awdl->fDataRx & 0x1f) == 0)
        awdl->publishDataState();
    // IONetworkInterface does not account these on awdl0 (netstat showed 0 in,
    // bytes included), so count them on the ifnet here.
    ifnet_stat_increment_in(getIfnet(), 1, (u_int32_t)mbuf_pkthdr_len(m), 0);
    inputPacket(m, (UInt32)mbuf_pkthdr_len(m), 0, m);
    return 1;
}

// Runs on the network output thread: move the frame onto the work loop.
UInt32 AirportItlwmAWDLEthernetInterface::
outputAWDL(mbuf_t m, void *param)
{
    if (!awDataAllowed() || !fOwner || !_fCommandGate)
        return dropOutput(m, param);
    // fOwner is read again inside the gate: destroyAWDLInterface() clears it
    // there before the AWDL interface is released.
    _fCommandGate->runAction([](OSObject *, void *self, void *pkt, void *, void *) -> IOReturn {
        auto bsd = static_cast<AirportItlwmAWDLEthernetInterface *>(self);
        if (bsd->fOwner)
            bsd->fOwner->enqueueData((mbuf_t)pkt);
        else
            mbuf_freem((mbuf_t)pkt);
        return kIOReturnSuccess;
    }, this, m);
    return kIOReturnOutputSuccess;
}

UInt32 AirportItlwmAWDLEthernetInterface::
dropOutput(mbuf_t m, void *param)
{
    fDropped++;
    mbuf_freem(m);
    return kIOReturnOutputDropped;
}

// Stage 2c is opt-in while it is being brought up: a test machine boot-looped
// with it always on. Without -itlawdlaw awdl0 only receives action frames.
// Stage 2b, also opt-in: visit the social channel when the station is not on it.
static bool awListenAllowed()
{
    static int allowed = -1;
    if (allowed < 0) {
        int v;
        allowed = PE_parse_boot_argn("-itlawdllisten", &v, sizeof(v)) ? 1 : 0;
    }
    return allowed == 1;
}

// Diagnostic: itlawdllistench=<n> sends every listen dwell to channel n
// instead of the AW slot channel, e.g. the channel the phone sits on.
static unsigned awListenChannelOverride()
{
    static int chan = -1;
    if (chan < 0) {
        int v = 0;
        chan = PE_parse_boot_argn("itlawdllistench", &v, sizeof(v)) && v > 0 && v <= 196 ? v : 0;
    }
    return (unsigned)chan;
}

// -itlawdlnopeerlisten turns off listening on the tracked peer's channel in
// slots where our own sequence stays home.
static bool awPeerListenAllowed()
{
    static int allowed = -1;
    if (allowed < 0) {
        int v = 0;
        allowed = PE_parse_boot_argn("-itlawdlnopeerlisten", &v, sizeof(v)) ? 0 : 1;
    }
    return allowed == 1;
}

// Stage 4: AWDL data frames to and from awdl0's BSD interface.
static bool awDataAllowed()
{
    static int allowed = -1;
    if (allowed < 0) {
        int v;
        allowed = PE_parse_boot_argn("-itlawdldata", &v, sizeof(v)) ? 1 : 0;
    }
    return allowed == 1;
}

// Stage 3: transmit AWDL frames during the listen dwells.
static bool awTxAllowed()
{
    static int allowed = -1;
    if (allowed < 0) {
        int v;
        allowed = PE_parse_boot_argn("-itlawdltx", &v, sizeof(v)) ? 1 : 0;
    }
    return allowed == 1;
}

static bool awTimelineAllowed()
{
    static int allowed = -1;
    if (allowed < 0) {
        int v;
        allowed = PE_parse_boot_argn("-itlawdlaw", &v, sizeof(v)) ? 1 : 0;
    }
    return allowed == 1;
}

// Safety switch for bring-up: -itlawdlnoinject still parses and tracks received AWDL
// action frames (peer timeline, sync) but never hands them to IO80211 (no
// getMgmtFramePool / IO80211Buffer / bpfTapInput), so an ABI mismatch on the receive
// path cannot panic the machine. Off by default; same meaning on Sequoia and Tahoe.
static bool awInjectAllowed()
{
    static int allowed = -1;
    if (allowed < 0) {
        int v;
        allowed = PE_parse_boot_argn("-itlawdlnoinject", &v, sizeof(v)) ? 0 : 1;
    }
    return allowed == 1;
}

#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
// Sequoia only, opt-in: -itlawdlsetif also registers awdl0 with
// IO80211Controller::setAwdlInterface() (Unopcoooo does this on 24H32;
// AppleBCMWLANCoreMac does not import it, so it may not be needed).
static bool awSetIfAllowed()
{
    static int allowed = -1;
    if (allowed < 0) {
        int v;
        allowed = PE_parse_boot_argn("-itlawdlsetif", &v, sizeof(v)) ? 1 : 0;
    }
    return allowed == 1;
}
#endif

#pragma mark - receive path, mirrors AppleBCMWLANProximityInterface::handleActionFrame_rx

// IO80211BufferPool / IO80211Buffer vtable slots (Tahoe KC, address point relative).
// Sequoia 24H32 (decoded from __ZTV17IO80211BufferPool in the 15.8.1 KC):
// +0x118 getBuffer(), +0x120 returnBuffer(IO80211Buffer*), +0x128 flushPool(),
// +0x130 resetPool(), +0x138 printStats(); so kPoolGetBuffer is valid on both.
// __ZTV13IO80211Buffer on 24H32 has no slots past OSObject (+0x110), so the kBuf*
// slots are Tahoe-only; Sequoia calls the exported functions by name instead.
enum : size_t {
    kPoolGetBuffer   = 0x118,
    kBufReturn       = 0x118,
    kBufSetLength    = 0x138,
    kBufGetCapacity  = 0x140,
    kBufGetBytes     = 0x158,
};
// IO80211SkywalkInterface::updateInterfaceDataStats, checked with tools/vtcheck.py --dump on
// the 25G332 KC (IO80211AWDLProtocol and IO80211VirtualInterface: +0xe28).
enum : size_t { kIfUpdateInterfaceDataStats = 0xe28 };
template <typename R, typename... A>
static inline R vcall(const void *obj, size_t slot, A... args)
{
    auto fn = *reinterpret_cast<R (* const *)(const void *, A...)>(*reinterpret_cast<const uint8_t * const *>(obj) + slot);
    return fn(obj, args...);
}

static inline AWDLBufferABI *awdlPoolGetBuffer(AWDLBufferPoolABI *pool)
{
    // Virtual on both ABIs, slot verified on 24H32 (keeps dynamic dispatch).
    return vcall<AWDLBufferABI *>(pool, kPoolGetBuffer);
}

static inline void awdlBufReturn(AWDLBufferABI *buf)
{
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    IO80211Buffer_returnBuffer(buf);
#else
    vcall<void>(buf, kBufReturn);
#endif
}

static inline size_t awdlBufCapacity(AWDLBufferABI *buf)
{
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    return (uint32_t)IO80211Buffer_getCapacity(buf);
#else
    return vcall<size_t>(buf, kBufGetCapacity);
#endif
}

static inline uint8_t *awdlBufBytes(AWDLBufferABI *buf)
{
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    return (uint8_t *)IO80211Buffer_getBytesNoCopy(buf);
#else
    return vcall<uint8_t *>(buf, kBufGetBytes);
#endif
}

static inline void awdlBufSetLength(AWDLBufferABI *buf, size_t len)
{
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    IO80211Buffer_setLength(buf, len);
#else
    vcall<void>(buf, kBufSetLength, len);
#endif
}

// Metadata AppleBCMWLANProximityInterface passes to bpfTapInput (0x48 bytes).
// 24H32: handleActionFrame_rx builds it at rbp-0x78 and calls bpfTapInput with
// edx=12 (DLT), rcx=&meta, r8d=0x48; it stores the channel via a pointer to +0x0c
// and byte-swapped firmware values to +0x28, +0x2c and +0x30 (same as Tahoe). The
// transmitter address at +0x00 could not be confirmed without a disassembler.
struct AWDLRxMeta {
    uint8_t  addr[6];          // 0x00 transmitter
    uint8_t  pad0[6];
    apple80211_channel chan;   // 0x0c
    uint8_t  pad1[0x28 - 0x18];
    uint32_t rate;             // 0x28
    int32_t  rssi;             // 0x2c, peer manager drops new peers below a threshold
    uint32_t tsf;              // 0x30 receive timestamp (us)
    uint8_t  pad2[0x48 - 0x34];
};
static_assert(sizeof(AWDLRxMeta) == 0x48, "AWDL rx meta size");
static_assert(offsetof(AWDLRxMeta, chan) == 0x0c, "AWDL rx meta chan");
static_assert(offsetof(AWDLRxMeta, rssi) == 0x2c, "AWDL rx meta rssi");

// Category 127, OUI 00:17:f2, type 8 (AWDL), version 0: checked by
// IO80211AWDLPeerManager::actionFrameHasCorrectOUI over the first 16 bytes.
static const uint8_t kAWDLFixedHeaderLen = 16;

void AirportItlwmAWDLInterface::
noteRxGap(const uint8_t *src, uint64_t nowUs)
{
    RxGap *e = nullptr, *oldest = &fRxGap[0];
    for (unsigned i = 0; i < kRxGapSlots; i++) {
        if (!memcmp(fRxGap[i].mac, src, 6) && fRxGap[i].lastUs) {
            e = &fRxGap[i];
            break;
        }
        if (fRxGap[i].lastUs < oldest->lastUs)
            oldest = &fRxGap[i];
    }
    if (e) {
        uint64_t gap = nowUs - e->lastUs;
        if (gap > e->maxGapUs)
            e->maxGapUs = gap;
        if (gap > 6000000)
            e->over6s++;
    } else {
        e = oldest;
        memset(e, 0, sizeof(*e));
        memcpy(e->mac, src, 6);
    }
    e->lastUs = nowUs;
    e->count++;
}

int AirportItlwmAWDLInterface::
inputActionFrame(const uint8_t *frame, size_t len, const ieee80211_rxinfo *rxi)
{
    const size_t hdr = sizeof(struct ieee80211_frame);
    if (len < hdr + kAWDLFixedHeaderLen)
        return 0;
    // The firmware path delivers the frame body, starting at the category byte.
    const uint8_t *af = frame + hdr;
    if (af[0] != 127 || af[1] != 0x00 || af[2] != 0x17 || af[3] != 0xf2 || af[4] != 0x08)
        return 0;

    fRxAF++;
    noteRxGap(((const struct ieee80211_frame *)frame)->i_addr2, hostUs());
    uint64_t *chanCount = rxi->rxi_chan == 6 ? &fRxAFChan6 :
        rxi->rxi_chan == 44 ? &fRxAFChan44 :
        rxi->rxi_chan == 149 ? &fRxAFChan149 : &fRxAFChanOther;
    if (rxi->rxi_chan == 161)
        fRxAFChan161++;
    bool firstOnChan = (*chanCount)++ == 0;
    // A frame on a social channel is rare and is the result 2b is looking for.
    bool socialChan = chanCount != &fRxAFChanOther;
    // XYLog goes to kprintf, so publish progress where ioreg can see it.
    if (firstOnChan || socialChan || fRxAF <= 8 || (fRxAF & 0xf) == 0) {
        publishListenState();
        setProperty("AWDLRxFrames", fRxAF, 64);
        setProperty("AWDLRxDropped", fRxAFDropped, 64);
        setProperty("AWDLRxLastChannel", rxi->rxi_chan, 32);
    }

    if (!awInjectAllowed()) {
        // -itlawdlnoinject: keep the host-side timeline fed, touch nothing in IO80211.
        const struct ieee80211_frame *nwh = (const struct ieee80211_frame *)frame;
        size_t npayload = len - hdr;
        uint64_t nrxUs = awTimelineAllowed() ? hostRxUs(rxi) : 0;
        trackPeer(af, npayload, nwh->i_addr2, nrxUs, rxi->rxi_chan, (int)rxi->rxi_rssi - 100);
        syncFromFrame(af, npayload, nwh->i_addr2, nrxUs);
        fRxAFNoInject++;
        if (fRxAFNoInject <= 8 || (fRxAFNoInject & 0xf) == 0)
            setProperty("AWDLRxNoInject", fRxAFNoInject, 64);
        return 1;
    }

    AWDLBufferPoolABI *pool = IO80211VirtualInterface_getMgmtFramePool(this);
    AWDLBufferABI *buf = pool ? awdlPoolGetBuffer(pool) : nullptr;
    if (!buf) {
        fRxAFDropped++;
        return 1;
    }
    size_t payload = len - hdr;
    if (payload > awdlBufCapacity(buf)) {
        awdlBufReturn(buf);
        fRxAFDropped++;
        return 1;
    }
    // Copied verbatim: bytes 8..15 are phy_tx_time / target_tx_time, which the
    // peer manager uses for time sync.
    uint8_t *bytes = awdlBufBytes(buf);
    if (!bytes) {
        awdlBufReturn(buf);
        fRxAFDropped++;
        return 1;
    }
    memcpy(bytes, af, payload);
    awdlBufSetLength(buf, payload);

    const struct ieee80211_frame *wh = (const struct ieee80211_frame *)frame;
    AWDLRxMeta meta = {};
    memcpy(meta.addr, wh->i_addr2, sizeof(meta.addr));
    meta.chan.version = APPLE80211_VERSION;
    meta.chan.channel = rxi->rxi_chan;
    meta.chan.flags = APPLE80211_C_FLAG_20MHZ |
        (rxi->rxi_chan <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
    // rxi_rssi is normalized to 0..67 (dBm - IWX_MIN_DBM); the peer manager
    // compares dBm against its threshold.
    meta.rssi = rxi->rxi_rssi - 100;
    // Same clock as the AW start times we report: the peer manager subtracts
    // one from the other (IO80211AWDLPeer::calculatePeerTimeDiff).
    uint64_t rxUs = awTimelineAllowed() ? hostRxUs(rxi) : 0;
    meta.tsf = awTimelineAllowed() ? (uint32_t)rxUs : rxi->rxi_tstamp;

    if (fRxAF <= 8 || (fRxAF & 0xff) == 0) {
        // Fixed header bytes 5..7 (version, subtype, reserved) of real frames.
        char hv[8];
        snprintf(hv, sizeof(hv), "%02x%02x%02x", af[5], af[6], af[7]);
        setProperty("AWDLRxHdrVer", hv);
    }
    trackPeer(af, payload, wh->i_addr2, rxUs, rxi->rxi_chan, (int)rxi->rxi_rssi - 100);
    syncFromFrame(af, payload, wh->i_addr2, rxUs);

    // Consumes buf (peer manager input + bpf, then returnBuffer).
    IO80211VirtualInterface_bpfTapInput(this, buf, 12, &meta, sizeof(meta));
    return 1;
}

#pragma mark - availability window timeline (host side of the firmware AW engine)

// Layout as read by AppleBCMWLANProximityInterface::buildAWDLChanSequence and
// IO80211AWDLPeerManager::setAwdlChannelSequence (the header's struct is stale):
// +5 entry count, +6 encoding, +7 step count (presence mode - 1),
// +12 entries of apple80211_channel (12 bytes each). 400 bytes in total.
enum : size_t {
    kChanSeqCount = 5,
    kChanSeqStep = 7,
    kChanSeqEntries = 12,
    kChanSeqMax = 32,
};

// Payloads of the AW messages, as built by AppleBCMWLANProximityInterface::
// handleAvailabilityWindowStart/End and read by IO80211AWDLPeerManager::
// reportDataPathEvents / monitorTimeline / logAWStart / logAWEnd. Times are
// big-endian microseconds; the peer manager subtracts the AW start time from
// the rx timestamp in the action frame meta, so both use hostUs().
// Posted through IO80211VirtualInterface::postMessage, which hands them to
// reportDataPathEvents -> IO80211AWDLPeerManager::reportDataPathEvents.
enum : unsigned int {
    kAWDLMsgAWStart = 41,
    kAWDLMsgAWExtension = 42,
    kAWDLMsgAWEnd = 43,
};

struct AWDLAWStartEvent {
    uint32_t seq;              // 0x00 AW sequence number (AWSeqNum)
    uint32_t role;             // 0x04 1 master, 2 slave, 3 non-election master
    uint8_t  master[6];        // 0x08 master we are synced to
    uint8_t  topMaster[6];     // 0x0e root of the sync tree
    uint32_t startUsBE;        // 0x14 AW start time
    apple80211_channel chan;   // 0x18 channel of this slot
} __attribute__((packed));
static_assert(sizeof(AWDLAWStartEvent) == 0x24, "AW start event size");

struct AWDLAWEndEvent {
    uint32_t extCount;         // 0x00
    uint32_t endUsBE;          // 0x04 AW end time
    uint8_t  pad[0x28 - 8];    // 0x08 extension and CCA statistics
} __attribute__((packed));
static_assert(sizeof(AWDLAWEndEvent) == 0x28, "AW end event size");

static const uint8_t kAWDLSyncParamsTLV = 4;
static const uint8_t kAWDLChanSeqTLV = 18;
static const int64_t kTimerEarlyUs = 2000;     // timer may fire this much early

static uint64_t hostUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

uint64_t AirportItlwmAWDLInterface::
hostRxUs(const ieee80211_rxinfo *rxi)
{
    uint64_t now = hostUs();
    if (!rxi->rxi_tstamp)
        return now;
    // The NIC TSF and the host clock drift slowly and the TSF can jump when it
    // follows the AP. Track the smallest host-minus-TSF offset (least rx
    // latency), let it creep up slowly for drift and reset on large jumps.
    uint32_t off = (uint32_t)now - rxi->rxi_tstamp;
    int32_t diff = (int32_t)(off - fTsfOffset);
    if (!fTsfValid || diff < 0 || diff > 20000) {
        fTsfOffset = off;
        fTsfValid = true;
    } else {
        fTsfOffset += diff < 20 ? diff : 20;
    }
    uint32_t latency = off - fTsfOffset;
    return latency < 1000000 ? now - latency : now;
}

uint16_t AirportItlwmAWDLInterface::
presenceMode() const
{
    // The peer manager steps AWSeqNum by its presence mode per channel slot and
    // tells us that through the channel sequence step count.
    if (fChanSeqValid && fChanSeq[kChanSeqStep] < 16)
        return fChanSeq[kChanSeqStep] + 1;
    if (fPresenceMode >= 1 && fPresenceMode <= 16)
        return fPresenceMode;
    return 4;
}

uint64_t AirportItlwmAWDLInterface::
slotUs() const
{
    uint32_t period = fSyncParams.availability_window_period;
    if (period < 4 || period > 64)
        period = 16;
    return (uint64_t)period * presenceMode() * 1024;
}

bool AirportItlwmAWDLInterface::
isSyncMaster() const
{
    static const ether_addr zero = {};
    return !memcmp(&fSyncMaster, &zero, sizeof(zero)) || !memcmp(&fSyncMaster, &fMac, sizeof(fMac));
}

ieee80211com *AirportItlwmAWDLInterface::
halIc() const
{
    return (fController && fController->fHalService) ?
        fController->fHalService->get80211Controller() : nullptr;
}

void AirportItlwmAWDLInterface::
startAWTimeline()
{
    if (fAWRunning || !fAWTimer || !awTimelineAllowed())
        return;
    uint64_t now = hostUs();
    uint16_t pm = presenceMode();
    // Free run until a master is programmed; any non-zero start works, the
    // peer manager only flags a sequence number stuck at 0.
    fRefUs = now;
    fRefSeq = (uint16_t)(((now / 1000) & 0x3fff) / pm * pm + pm);
    fLastValid = false;
    fInAW = false;
    fAWRunning = true;
    // Pull model: the interface's output thread starts stopped and calls outputStart() only
    // once started (IONetworkInterface::startOutputThread). B6408479 never started it:
    // the stack queued 41 packets on awdl0 and none ever left. Starting again is harmless.
    if (fController && fController->fAWDLBsdIf && fController->fAWDLBsdIf->fPullModel) {
        IOReturn sr = fController->fAWDLBsdIf->startOutputThread();
        fOutputStarts++;
        fOutputStartRet = sr;
    }
    publishAWDLAddress();
    AWDL_LOG("seq %u slot %llu us", fRefSeq, slotUs());
    scheduleNextAW(now);
    if (awDataAllowed() && awListenAllowed())
        scheduleAlignedListen(now);
    publishSyncState();
}

void AirportItlwmAWDLInterface::
stopAWTimeline()
{
    if (!fAWRunning)
        return;
    fAWRunning = false;
    if (fAWTimer)
        fAWTimer->cancelTimeout();
    fListenAlignedActive = false;
    fPeerValid = false;
    fPeerChanValid = false;
    if (ieee80211com *ic = halIc())
        ic->ic_awdl_want_data = 0;
    if (fListenTimer)
        fListenTimer->cancelTimeout();
    fTxPhase = 0;
    if (fTxTimer)
        fTxTimer->cancelTimeout();
    // Nothing can leave without the timeline; do not send stale frames later.
    while (fDataQCount) {
        mbuf_freem(fDataQ[fDataQHead]);
        fDataQ[fDataQHead] = nullptr;
        fDataQHead = (fDataQHead + 1) % kDataQueueLen;
        fDataQCount--;
    }
    resumeOutput();     // pull model: the stack's queue must not stay parked on an empty one
    if (fInAW) {
        AWDLAWEndEvent end = {};
        end.endUsBE = OSSwapHostToBigInt32((uint32_t)hostUs());
        postMessage(kAWDLMsgAWEnd, &end, sizeof(end), true);
    }
    fInAW = false;
    AWDL_LOG("starts %llu syncs %llu", fAWStarts, fSyncUpdates);
    publishSyncState();
}

void AirportItlwmAWDLInterface::
teardownAW()
{
    fAWRunning = false;
    fListenAlignedActive = false;
    if (ieee80211com *ic = halIc())
        ic->ic_awdl_want_data = 0;
    if (fListenTimer) {
        fListenTimer->cancelTimeout();
        if (fListenTimer->getWorkLoop())
            fListenTimer->getWorkLoop()->removeEventSource(fListenTimer);
        OSSafeReleaseNULL(fListenTimer);
    }
    fTxPhase = 0;
    while (fDataQCount) {
        mbuf_freem(fDataQ[fDataQHead]);
        fDataQ[fDataQHead] = nullptr;
        fDataQHead = (fDataQHead + 1) % kDataQueueLen;
        fDataQCount--;
    }
    if (fTxTimer) {
        fTxTimer->cancelTimeout();
        if (fTxTimer->getWorkLoop())
            fTxTimer->getWorkLoop()->removeEventSource(fTxTimer);
        OSSafeReleaseNULL(fTxTimer);
    }
    if (!fAWTimer)
        return;
    fAWTimer->cancelTimeout();
    if (fAWTimer->getWorkLoop())
        fAWTimer->getWorkLoop()->removeEventSource(fAWTimer);
    OSSafeReleaseNULL(fAWTimer);
}

void AirportItlwmAWDLInterface::
scheduleNextAW(uint64_t nowUs)
{
    if (!fAWRunning || !fAWTimer)
        return;
    int64_t slot = (int64_t)slotUs();
    int64_t since = (int64_t)nowUs - fRefUs + kTimerEarlyUs;
    int64_t k = since >= 0 ? since / slot + 1 : -((-since) / slot);
    int64_t when = fRefUs + k * slot;
    uint64_t abs;
    nanoseconds_to_absolutetime((uint64_t)when * 1000, &abs);
    fAWTimer->cancelTimeout();
    fAWTimer->wakeAtTime(abs);
}

void AirportItlwmAWDLInterface::
awTimerFired(OSObject *owner, IOTimerEventSource *)
{
    AirportItlwmAWDLInterface *that = OSDynamicCast(AirportItlwmAWDLInterface, owner);
    if (!that || !that->fAWRunning)
        return;
    that->postAWEvents();
    that->scheduleNextAW(hostUs());
}

void AirportItlwmAWDLInterface::
postAWEvents()
{
    uint64_t now = hostUs();
    int64_t slot = (int64_t)slotUs();
    uint16_t pm = presenceMode();
    int64_t since = (int64_t)now - fRefUs + kTimerEarlyUs;
    int64_t k = since >= 0 ? since / slot : -((-since + slot - 1) / slot);
    int64_t start = fRefUs + k * slot;
    uint16_t seq = (uint16_t)(fRefSeq + k * pm);
    // A re-sync can map this boundary to the slot we already reported.
    if (fLastValid && seq == fLastSeq)
        return;

    reportDataStats();

    // The peer manager expects end then start (it clears its "in AW" and
    // back-to-back flags on the end event).
    if (fInAW) {
        AWDLAWEndEvent end = {};
        end.endUsBE = OSSwapHostToBigInt32((uint32_t)start);
        postMessage(kAWDLMsgAWEnd, &end, sizeof(end), true);
        fInAW = false;
    }

    // Channel 0 in the sequence is an idle slot. The firmware sends no AW start
    // there: sanityCheckAWStart counts the skipped slots and only complains
    // about skipped slots that have a channel.
    unsigned count = fChanSeqValid ? fChanSeq[kChanSeqCount] : 0;
    const apple80211_channel *slotChan = count ?
        (const apple80211_channel *)(fChanSeq + kChanSeqEntries) + (seq / pm) % count : nullptr;
    if (slotChan && !slotChan->channel) {
        fLastValid = true;
        fLastSeq = seq;
        return;
    }

    AWDLAWStartEvent ev = {};
    ev.seq = seq;
    ev.role = isSyncMaster() ? 1 : 2;
    const ether_addr &master = isSyncMaster() ? fMac : fSyncMaster;
    memcpy(ev.master, &master, sizeof(ev.master));
    memcpy(ev.topMaster, &master, sizeof(ev.topMaster));
    ev.startUsBE = OSSwapHostToBigInt32((uint32_t)start);
    ev.chan.version = APPLE80211_VERSION;
    if (slotChan) {
        ev.chan.channel = slotChan->channel;
        ev.chan.flags = slotChan->flags;
    }
    if (!ev.chan.channel) {
        ev.chan.channel = 6;
        ev.chan.flags = APPLE80211_C_FLAG_2GHZ | APPLE80211_C_FLAG_20MHZ;
    }
    postMessage(kAWDLMsgAWStart, &ev, sizeof(ev), true);
    maybeListen(ev.chan.channel, seq);

    fInAW = true;
    fLastValid = true;
    fLastSeq = seq;
    fLastStartUs = start;
    fAWStarts++;
    if (fAWStarts <= 4 || (fAWStarts & 0x3f) == 0)
        publishSyncState();
}

// Follow the master's timeline from its Synchronization Parameters TLV, as
// OWL does (seemoo-lab/owl src/sync.c, rx.c): the next extended AW (presence
// mode AWs) starts tx_down_counter TU after the frame, and its sequence number
// is next_aw_seq rounded down to the presence mode, plus one presence mode
// (same arithmetic as IO80211AWDLPeer::calculatePeerTimeDiff).
void AirportItlwmAWDLInterface::
syncFromFrame(const uint8_t *af, size_t len, const uint8_t *src, uint64_t rxUs)
{
    if (!fAWRunning || isSyncMaster() || memcmp(src, &fSyncMaster, sizeof(fSyncMaster)))
        return;
    size_t off = kAWDLFixedHeaderLen;
    while (off + 3 <= len) {
        uint8_t type = af[off];
        uint16_t tlen = af[off + 1] | (af[off + 2] << 8);
        const uint8_t *v = af + off + 3;
        if (off + 3 + tlen > len)
            return;
        if (type == kAWDLSyncParamsTLV) {
            if (tlen < 31)
                return;
            uint16_t down = v[1] | (v[2] << 8);
            uint16_t tlvPm = v[27];
            uint16_t nextSeq = v[29] | (v[30] << 8);
            uint16_t pm = presenceMode();
            if (tlvPm && tlvPm != pm)
                setProperty("AWDLMasterPresenceMode", tlvPm, 32);
            // Fixed header bytes 8..11 / 12..15 are phy_tx / target_tx (us,
            // little-endian). Their difference may be the delay between building
            // the frame (tx_down_counter) and sending it [inferred]; recorded
            // for now, not applied.
            uint32_t phyTx = af[8] | (af[9] << 8) | (af[10] << 16) | ((uint32_t)af[11] << 24);
            uint32_t targetTx = af[12] | (af[13] << 8) | (af[14] << 16) | ((uint32_t)af[15] << 24);
            int64_t txDelay = (int32_t)(phyTx - targetTx);
            if (txDelay < 0 || txDelay > 100000)
                txDelay = 0;
            int64_t boundary = (int64_t)rxUs + (int64_t)down * 1024;
            uint16_t seq = (uint16_t)(nextSeq / pm * pm + pm);

            // Error of our timeline at that boundary.
            int64_t slot = (int64_t)slotUs();
            int64_t d = boundary - fRefUs;
            int64_t k = d >= 0 ? (d + slot / 2) / slot : -((-d + slot / 2) / slot);
            int64_t err = d - k * slot;
            int16_t seqErr = (int16_t)(seq - (uint16_t)(fRefSeq + k * pm));
            fLastSyncErrUs = err;
            setProperty("AWDLSyncTxDelayUs", (uint64_t)txDelay, 32);

            // The peer manager tolerates +/-10 ms between AW starts. Ignore a
            // single large outlier (rx latency spike); re-anchor only when the
            // error persists, the sequence differs or we are not locked yet.
            const int64_t kOutlierUs = 6000;
            bool large = err > kOutlierUs || err < -kOutlierUs;
            if (fSyncUpdates && !seqErr && large && ++fSyncOutliers < 3) {
                setProperty("AWDLSyncOutliers", fSyncOutliersTotal + 1, 64);
                fSyncOutliersTotal++;
                return;
            }
            fSyncOutliers = 0;

            fRefUs = boundary;
            fRefSeq = seq;
            fSyncUpdates++;
            if (fSyncUpdates <= 4 || (fSyncUpdates & 0x3f) == 0 || seqErr) {
                setProperty("AWDLSyncSeqErr", (uint64_t)(int64_t)seqErr, 32);
                publishSyncState();
            }
            scheduleNextAW(hostUs());
            return;
        }
        off += 3 + tlen;
    }
}

// 2b: the station stays on the AP channel. When that is not the channel of
// this AW slot, ask the firmware for a short off-channel visit (iwx listen
// scan) so AWDL frames sent in the AW can be heard. Only every
// kListenEveryNthSlot-th such slot, to keep the station link usable.
// Defaults; both can be changed at run time (setProperties). A new listen is
// refused while one is pending or running, which bounds the duty cycle.
enum : unsigned {
    // Longest single dwell. 160 TU cut the six-slot run of the Data sequence (393 ms) into
    // two dwells with a ~30 ms hole and lost its last slot; with 420 TU one dwell covers the
    // run: iPhone -> Mac 148 -> 443 kB/s (2026-11-13). Run time: awdlctl dwell <TU>.
    kListenDwellTU = 420,
    kListenEveryNthSlot = 1,
    kListenDwellMinTU = 16,
    kListenDwellMaxTU = 1000,
};

IOReturn AirportItlwmAWDLInterface::
setProperties(OSObject *properties)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    // Only while the experimental listen is on, so a stock -itlawdl boot
    // exposes no new writable state.
    if (!d || !awListenAllowed())
        return IO80211AWDLProtocol::setProperties(properties);
    // The TX/listen knobs below change radio behaviour: only an administrator
    // (awdlctl run with sudo) may set them.
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    bool handled = false;
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("AWDLListenDwellTU"))) {
        uint32_t v = n->unsigned32BitValue();
        if (v < kListenDwellMinTU || v > kListenDwellMaxTU)
            return kIOReturnBadArgument;
        fListenDwellTU = v;
        handled = true;
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("AWDLListenEveryNth"))) {
        uint32_t v = n->unsigned32BitValue();
        if (v < 1 || v > 64)
            return kIOReturnBadArgument;
        fListenEveryNth = v;
        handled = true;
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("AWDLListenAlign"))) {
        // Called on a user thread; the timers and peer state belong to the work loop.
        uintptr_t align = n->unsigned32BitValue() != 0;
        handled = true;
        if (_fCommandGate)
            _fCommandGate->runAction([](OSObject *, void *self, void *value, void *, void *) -> IOReturn {
                auto that = static_cast<AirportItlwmAWDLInterface *>(self);
                that->fListenAlign = (uint32_t)(uintptr_t)value;
                if (!that->fListenAlign) {
                    that->fListenAlignedActive = false;
                    if (that->fListenTimer)
                        that->fListenTimer->cancelTimeout();
                } else if (!that->fListenAlignedActive) {
                    that->scheduleAlignedListen(hostUs());
                }
                return kIOReturnSuccess;
            }, this, (void *)align);
    }
    // Run-time A/B of the TX switches (same meaning as the boot-args).
    struct TxKnobs { int pump, home; int64_t lead; int64_t dwellx, pumpus; int follow; } knobs = { -1, -1, -1, -1, -1, -1 };
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject("AWDLFollowPeerOn")))
        knobs.follow = b->isTrue();
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject("AWDLTxPumpOn")))
        knobs.pump = b->isTrue();
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject("AWDLHomeTxOn")))
        knobs.home = b->isTrue();
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("AWDLListenLeadUs"))) {
        uint32_t v = n->unsigned32BitValue();
        if (v < 1000 || v > 60000)
            return kIOReturnBadArgument;
        knobs.lead = v;
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("AWDLDwellExtraTU"))) {
        uint32_t v = n->unsigned32BitValue();
        if (v > 200)
            return kIOReturnBadArgument;
        knobs.dwellx = v;
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("AWDLPumpUs"))) {
        uint32_t v = n->unsigned32BitValue();
        if (v < 250 || v > 10000)
            return kIOReturnBadArgument;
        knobs.pumpus = v;
    }
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject("AWDLDataStatsOn"))) {
        fDataStats = b->isTrue();
        handled = true;
    }
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject("AWDLIdleDwellOn"))) {
        fIdleDwell = b->isTrue();
        handled = true;
    }
    if (OSBoolean *b = OSDynamicCast(OSBoolean, d->getObject("AWDLAwGateOn"))) {
        fAwGate = b->isTrue();          // a plain flag read on the work loop
        handled = true;
    }
    if (knobs.pump >= 0 || knobs.home >= 0 || knobs.lead >= 0 || knobs.dwellx >= 0 || knobs.pumpus >= 0 ||
        knobs.follow >= 0) {
        handled = true;
        if (_fCommandGate)
            _fCommandGate->runAction([](OSObject *, void *self, void *arg, void *, void *) -> IOReturn {
                auto that = static_cast<AirportItlwmAWDLInterface *>(self);
                auto k = static_cast<const TxKnobs *>(arg);
                if (k->pump >= 0) {
                    that->fTxPump = k->pump;
                    if (!that->fTxPump && that->fTxPhase == 3)
                        that->fTxPhase = 0;
                }
                if (k->home >= 0)
                    that->fHomeTx = k->home;
                if (k->lead >= 0)
                    that->fListenLeadUs = k->lead;
                if (k->dwellx >= 0)
                    that->fDwellExtraTU = (uint32_t)k->dwellx;
                if (k->pumpus >= 0)
                    that->fPumpUs = (uint32_t)k->pumpus;
                if (k->follow >= 0) {
                    that->fFollowPeer = k->follow;
                    that->fFollowStreak = 0;
                }
                that->publishTxDiag();
                return kIOReturnSuccess;
            }, this, &knobs);
    }
    if (!handled)
        return IO80211AWDLProtocol::setProperties(properties);
    setProperty("AWDLListenDwellTU", fListenDwellTU, 32);
    setProperty("AWDLListenEveryNth", fListenEveryNth, 32);
    setProperty("AWDLListenAlign", fListenAlign, 32);
    // Counters are published in batches; this gives callers an exact snapshot.
    publishListenState();
    AWDL_LOG("listen dwell %u TU every %u align %u", fListenDwellTU, fListenEveryNth, fListenAlign);
    return kIOReturnSuccess;
}

void AirportItlwmAWDLInterface::
publishListenState()
{
    {
        // "mac frames maxGapMs over6s ageMs" for each transmitter heard recently.
        char g[512];
        size_t o = 0;
        uint64_t now = hostUs();
        for (unsigned i = 0; i < kRxGapSlots && o < sizeof(g) - 64; i++) {
            const RxGap &e = fRxGap[i];
            if (!e.lastUs || now - e.lastUs > 120000000)
                continue;
            o += snprintf(g + o, sizeof(g) - o, "%02x%02x %u %llu %u %llu; ", e.mac[4], e.mac[5], e.count,
                          e.maxGapUs / 1000, e.over6s, (now - e.lastUs) / 1000);
        }
        g[o] = 0;
        setProperty("AWDLRxGap", g);
    }

    setProperty("AWDLRxChan6", fRxAFChan6, 64);
    setProperty("AWDLRxChan44", fRxAFChan44, 64);
    setProperty("AWDLRxChan149", fRxAFChan149, 64);
    setProperty("AWDLRxChanOther", fRxAFChanOther, 64);
    setProperty("AWDLListenStarted", fListenStarted, 64);
    setProperty("AWDLListenBusy", fListenBusy, 64);
    setProperty("AWDLListenAligned", fListenAligned, 64);
    setProperty("AWDLListenNoSlot", fListenNoSlot, 64);
    setProperty("AWDLListenAlignedActive", fListenAlignedActive);
    publishPeerTl();
    setProperty("AWDLPeerUpdates", fPeerUpdates, 64);
    setProperty("AWDLPeerPredHit", fPeerPredHit, 64);
    setProperty("AWDLPeerPredMiss", fPeerPredMiss, 64);
    setProperty("AWDLPeerErrUs", (uint64_t)(fPeerLastErrUs < 0 ? -fPeerLastErrUs : fPeerLastErrUs), 32);
    if (fPeerChanValid) {
        char seq[16 * 4 + 1] = {};
        for (unsigned i = 0, n = 0; i < 16; i++)
            n += snprintf(seq + n, sizeof(seq) - n, " %u", fPeerChan[i]);
        setProperty("AWDLPeerChanSeq", seq);
    }
    ieee80211com *ic = halIc();
    if (!ic)
        return;
    setProperty("AWDLListenDone", ic->ic_awdl_listen_done, 32);
    setProperty("AWDLListenRxAll", ic->ic_awdl_listen_rx_all, 32);
    setProperty("AWDLListenRxBeacon", ic->ic_awdl_listen_rx_beacon, 32);
    setProperty("AWDLListenRxAction", ic->ic_awdl_listen_rx_action, 32);
    setProperty("AWDLRocSent", ic->ic_awdl_roc_sent, 32);
    setProperty("AWDLRocRespErr", ic->ic_awdl_roc_resp_err, 32);
    setProperty("AWDLRocStarted", ic->ic_awdl_roc_started, 32);
    setProperty("AWDLRocEnded", ic->ic_awdl_roc_ended, 32);
    setProperty("AWDLRocNotScheduled", ic->ic_awdl_roc_not_sched, 32);
    setProperty("AWDLRocStartFail", ic->ic_awdl_roc_start_fail, 32);
    setProperty("AWDLRocEndFail", ic->ic_awdl_roc_end_fail, 32);
    setProperty("AWDLRocEndFailShort", ic->ic_awdl_roc_end_fail_short, 32);
    setProperty("AWDLRocEndFailLastUs", ic->ic_awdl_roc_end_fail_last_us, 32);
    setProperty("AWDLRocChanChanges", ic->ic_awdl_roc_chan_changes, 32);
    setProperty("AWDLRocBandChanges", ic->ic_awdl_roc_band_changes, 32);
    char per[96];
    snprintf(per, sizeof(per), "6:%llu/%llu 44:%llu/%llu 149:%llu/%llu 161:%llu/%llu",
             fListenOnChan[0], fRxAFChan6, fListenOnChan[1], fRxAFChan44,
             fListenOnChan[2], fRxAFChan149, fListenOnChan[3], fRxAFChan161);
    setProperty("AWDLListenByChan", per);
    if (awTxAllowed())
        publishTxState();
    if (awDataAllowed())
        publishDataState();
    setProperty("AWDLRocStaErr", ic->ic_awdl_roc_sta_err, 32);
    setProperty("AWDLRocStaStage", ic->ic_awdl_roc_sta_stage, 32);
    setProperty("AWDLRocStaCode", ic->ic_awdl_roc_sta_code, 32);
    setProperty("AWDLRocQueue", ic->ic_awdl_roc_qid, 32);
    setProperty("AWDLRocLastUs", ic->ic_awdl_roc_last_us, 32);
    setProperty("AWDLRocMinUs", ic->ic_awdl_roc_min_us, 32);
    setProperty("AWDLRocMaxUs", ic->ic_awdl_roc_max_us, 32);
    setProperty("AWDLRocDelayUs", ic->ic_awdl_roc_delay_us, 32);
    setProperty("AWDLRocRxTarget", ic->ic_awdl_roc_rx_target, 32);
    setProperty("AWDLRocRxHome", ic->ic_awdl_roc_rx_home, 32);
    setProperty("AWDLRocRxOther", ic->ic_awdl_roc_rx_other, 32);
    setProperty("AWDLRocRxLastChannel", ic->ic_awdl_roc_rx_last_chan, 32);
}

void AirportItlwmAWDLInterface::
maybeListen(uint32_t slotChannel, uint16_t seq)
{
    if (!awListenAllowed() || !fController || !slotChannel)
        return;
    ieee80211com *ic = halIc();
    if (!ic || !ic->ic_awdl_listen || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss)
        return;
    unsigned home = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
    if (unsigned forced = awListenChannelOverride())
        slotChannel = forced;
    if (home == slotChannel) {
        fListenSkippedHome++;
        return;
    }
    // The peer's timeline is known: the listen timer picks the moments instead.
    if (fListenAlignedActive)
        return;
    if (++fListenSlot % fListenEveryNth)
        return;
    startListen(slotChannel, fListenDwellTU, home, false);
}

void AirportItlwmAWDLInterface::
startListen(unsigned chan, uint32_t dwellTU, unsigned home, bool aligned)
{
    ieee80211com *ic = halIc();
    if (!ic || !ic->ic_awdl_listen)
        return;
    int err = ic->ic_awdl_listen(ic, (uint8_t)chan, (uint16_t)dwellTU);
    if (err) {
        fListenBusy++;
    } else {
        fListenStarted++;
        if (aligned)
            fListenAligned++;
        int idx = chan == 6 ? 0 : chan == 44 ? 1 : chan == 149 ? 2 : chan == 161 ? 3 : -1;
        if (idx >= 0)
            fListenOnChan[idx]++;
    }
    if (((fListenStarted + fListenBusy) & 0x1f) == 1) {
        publishListenState();
        setProperty("AWDLListenHomeChannel", home, 32);
        setProperty("AWDLListenChannel", chan, 32);
        setProperty("AWDLListenLastErr", err, 32);
    }
}

// A listen goes out this long before the slot: the firmware took about 4.4 ms
// from the command to its START notification.
static const int64_t kListenLeadDefaultUs = 6000;   // itlawdllead= overrides (fListenLeadUs)
// Without a frame from the peer for this long, go back to listening blindly.
static const uint64_t kPeerStaleUs = 30000000;
static const uint64_t kPeerReplaceUs = 10000000;
// The TX gate trusts the tracked peer's predicted channel only this long after
// its last frame (IO80211Family itself drops a peer after ~6 s without one).
static const uint64_t kPeerGateStaleUs = 10000000;
// AW gate (awGateOpen): usable part of the availability window and how long after the
// last unicast frame from the peer it still counts as being in data mode.
static const uint64_t kAwGateHeadUs = 1500;
static const uint64_t kAwGateTailUs = 5000;
static const uint64_t kAwGateDataModeUs = 300000;
// Idle dwells (dwellTUFor): with no unicast traffic for kIdleAfterUs the listen dwell only
// covers the availability window of the slot, not the whole 64-TU slot.
static const uint64_t kIdleAfterUs = 3000000;
static const uint64_t kIdleTailUs = 6000;
static const int64_t kIdleLeadMaxUs = 12000;
// Peer timeline within this of ours counts as aligned (the peer manager
// tolerates about 10 ms between AW starts).
static const int64_t kFollowTolUs = 6000;

static int64_t floorDiv(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// Channel sequence as in a TLV 18 value (seemoo-lab/owl rx.c): count - 1,
// encoding, duplicate count, step count, fill channel (2), then the entries.
bool AirportItlwmAWDLInterface::
parsePeerChanSeq(const uint8_t *v, size_t len)
{
    if (len < 6 || v[0] + 1 != 16)
        return false;
    size_t size = v[1] == 0 ? 1 : (v[1] == 1 || v[1] == 3) ? 2 : 0;
    if (!size || len < 6 + 16 * size)
        return false;
    // Legacy keeps the channel in the second byte, op class in the first.
    size_t at = v[1] == 1 ? 1 : 0;
    for (unsigned i = 0; i < 16; i++)
        fPeerChan[i] = v[6 + i * size + at];
    fPeerChanValid = true;
    return true;
}

unsigned AirportItlwmAWDLInterface::
peerChannelAt(uint64_t us) const
{
    if (!fPeerValid || !fPeerChanValid || !fPeerSlotUs || !fPeerPm)
        return 0;
    int64_t k = floorDiv((int64_t)us - fPeerRefUs, fPeerSlotUs);
    uint16_t seq = (uint16_t)(fPeerRefSeq + k * fPeerPm);
    return fPeerChan[(seq / fPeerPm) % 16];
}

// Keep the timeline of the peer we hear, whoever our sync master is: when we
// are master ourselves syncFromFrame learns nothing about it.
void AirportItlwmAWDLInterface::
trackPeer(const uint8_t *af, size_t len, const uint8_t *src, uint64_t rxUs, unsigned rxChan, int rssiDbm)
{
    if (!fAWRunning || !awListenAllowed())
        return;
    peerTlLearn(af, len, src, rxUs, rssiDbm);
    if (fPeerValid && memcmp(src, &fPeerMac, sizeof(fPeerMac))) {
        // One peer is tracked. It has to be the one we exchange unicast data
        // with (the AirDrop peer), not whichever neighbour was heard first:
        // the listens in our off slots and the TX gate both follow it.
        bool dataRecent = fDataPeerUs && (int64_t)(rxUs - fDataPeerUs) < (int64_t)kPeerReplaceUs;
        bool srcIsData = dataRecent && !memcmp(src, &fDataPeerMac, sizeof(fDataPeerMac));
        bool trackedIsData = dataRecent && !memcmp(&fPeerMac, &fDataPeerMac, sizeof(fDataPeerMac)) &&
            (int64_t)(rxUs - fPeerSeenUs) < (int64_t)kPeerStaleUs;
        PeerTl *se = peerTlFind(src, false, rxUs);
        PeerTl *te = peerTlFind((const uint8_t *)&fPeerMac, false, rxUs);
        // A peer inside the bubble (cached by the family, the data peer, or simply strong)
        // replaces one that is not: the first device heard is often a neighbour's.
        bool srcCachedOnly = se && entryTrusted(*se, rxUs) && !(te && entryTrusted(*te, rxUs)) && !trackedIsData;
        if (!srcIsData && !srcCachedOnly &&
            (trackedIsData || (int64_t)(rxUs - fPeerSeenUs) < (int64_t)kPeerReplaceUs))
            return;
        if (srcIsData)
            fPeerSwitchData++;
        else if (srcCachedOnly)
            fPeerSwitchCached++;
        fPeerValid = false;
        fPeerChanValid = false;
        fFollowStreak = 0;
    }
    // Checks the slot-to-channel mapping, which is inferred for a peer.
    if (unsigned predicted = peerChannelAt(rxUs)) {
        if (predicted == rxChan)
            fPeerPredHit++;
        else
            fPeerPredMiss++;
    }
    size_t off = kAWDLFixedHeaderLen;
    bool sync = false;
    while (off + 3 <= len) {
        uint8_t type = af[off];
        uint16_t tlen = af[off + 1] | (af[off + 2] << 8);
        const uint8_t *v = af + off + 3;
        if (off + 3 + tlen > len)
            break;
        if (type == kAWDLSyncParamsTLV && tlen >= 31) {
            uint16_t down = v[1] | (v[2] << 8);
            uint16_t period = v[5] | (v[6] << 8);
            uint16_t pm = v[27];
            uint16_t nextSeq = v[29] | (v[30] << 8);
            if (period >= 4 && period <= 64 && pm >= 1 && pm <= 16) {
                int64_t boundary = (int64_t)rxUs + (int64_t)down * 1024;
                uint32_t slot = (uint32_t)period * pm * 1024;
                if (fPeerValid && fPeerSlotUs == slot) {
                    int64_t d = boundary - fPeerRefUs;
                    fPeerLastErrUs = d - floorDiv(d + slot / 2, slot) * slot;
                }
                fPeerRefUs = boundary;
                fPeerRefSeq = (uint16_t)(nextSeq / pm * pm + pm);
                fPeerPm = pm;
                fPeerSlotUs = slot;
                memcpy(&fPeerMaster, v + 21, sizeof(fPeerMaster));
                sync = true;
            }
            if (tlen > 33)
                parsePeerChanSeq(v + 33, tlen - 33);
        } else if (type == kAWDLChanSeqTLV) {
            parsePeerChanSeq(v, tlen);
        }
        off += 3 + tlen;
    }
    if (!sync)
        return;
    memcpy(&fPeerMac, src, sizeof(fPeerMac));
    fPeerValid = true;
    fPeerSeenUs = rxUs;
    fPeerUpdates++;
    // Is the peer on our timeline? A peer only sends to us in slots where its
    // channel equals the one we advertise for that slot, so with a different
    // slot numbering (two masters) a sparse sequence leaves no common slot.
    if (fPeerSlotUs == slotUs() && fPeerPm == presenceMode()) {
        int64_t slot = (int64_t)fPeerSlotUs;
        int64_t d = fPeerRefUs - fRefUs;
        int64_t k = floorDiv(d + slot / 2, slot);
        uint16_t ownSeq = (uint16_t)(fRefSeq + k * fPeerPm);
        fPeerOwnErrUs = d - k * slot;
        fPeerSlotOff = (uint16_t)(((fPeerRefSeq / fPeerPm) % 16 + 16 - (ownSeq / fPeerPm) % 16) % 16);
        bool mis = fPeerSlotOff != 0 || fPeerOwnErrUs > kFollowTolUs || fPeerOwnErrUs < -kFollowTolUs;
        if (mis) {
            fPeerMisalignedCnt++;
            fFollowStreak++;
        } else {
            fPeerAlignedCnt++;
            fFollowStreak = 0;
        }
        // -itlawdlfollow: we think we are the master but the peer keeps its own
        // timeline (it did not elect us). Adopt it, exactly as syncFromFrame
        // does for a real master; three frames in a row so one late frame
        // changes nothing. Not when the peer names us as its master: then it
        // is following us and moving our timeline would only make it chase.
        bool peerFollowsUs = !memcmp(&fPeerMaster, &fMac, sizeof(fMac));
        if (mis && peerFollowsUs)
            fPeerFollowsUsMis++;
        bool trusted = peerTrusted(src, rxUs);
        if (fFollowPeer && isSyncMaster() && mis && !peerFollowsUs && fFollowStreak >= 3 && !trusted)
            fFollowUntrusted++;
        if (fFollowPeer && isSyncMaster() && mis && !peerFollowsUs && fFollowStreak >= 3 && trusted) {
            fRefUs = fPeerRefUs;
            fRefSeq = fPeerRefSeq;
            fFollowAdopts++;
            fFollowStreak = 0;
            fPeerOwnErrUs = 0;
            fPeerSlotOff = 0;
            scheduleNextAW(hostUs());
            fListenAlignedActive = false;       // re-plan the dwells below
            publishTxDiag();
        }
    }
    if (!fListenAlignedActive)
        scheduleAlignedListen(hostUs());
}

#pragma mark - per-peer timelines

AirportItlwmAWDLInterface::PeerTl *AirportItlwmAWDLInterface::
peerTlFind(const uint8_t *mac, bool create, uint64_t now)
{
    PeerTl *freeSlot = nullptr, *oldest = nullptr;
    for (unsigned i = 0; i < kPeerTlMax; i++) {
        PeerTl &e = fPeerTl[i];
        if (!e.used) {
            if (!freeSlot)
                freeSlot = &e;
            continue;
        }
        if (!memcmp(&e.mac, mac, sizeof(e.mac)))
            return &e;
        if (!e.cached && (!oldest || e.seenUs < oldest->seenUs))
            oldest = &e;
    }
    if (!create)
        return nullptr;
    PeerTl *e = freeSlot;
    if (!e) {
        // Full: reuse the entry heard longest ago, but never one still in use.
        if (!oldest || (int64_t)(now - oldest->seenUs) < (int64_t)kPeerGateStaleUs)
            return nullptr;
        e = oldest;
        fPeerTlEvict++;
    }
    bzero(e, sizeof(*e));
    e->used = true;
    memcpy(&e->mac, mac, sizeof(e->mac));
    return e;
}

// Same TLVs as trackPeer (sync parameters 4, channel sequence 18), kept per source.
void AirportItlwmAWDLInterface::
peerTlLearn(const uint8_t *af, size_t len, const uint8_t *src, uint64_t rxUs, int rssiDbm)
{
    if (!src || (src[0] & 1) || !memcmp(src, &fMac, sizeof(fMac)))
        return;
    bool sync = false, haveChan = false;
    int64_t refUs = 0;
    uint16_t refSeq = 0, pmv = 0;
    uint32_t slot = 0;
    uint8_t chan[16];
    uint8_t master[6] = {};
    auto parseChan = [&](const uint8_t *v, size_t l) {
        if (l < 6 || v[0] + 1 != 16)
            return;
        size_t size = v[1] == 0 ? 1 : (v[1] == 1 || v[1] == 3) ? 2 : 0;
        if (!size || l < 6 + 16 * size)
            return;
        size_t at = v[1] == 1 ? 1 : 0;
        for (unsigned i = 0; i < 16; i++)
            chan[i] = v[6 + i * size + at];
        haveChan = true;
    };
    size_t off = kAWDLFixedHeaderLen;
    while (off + 3 <= len) {
        uint8_t type = af[off];
        uint16_t tlen = af[off + 1] | (af[off + 2] << 8);
        const uint8_t *v = af + off + 3;
        if (off + 3 + tlen > len)
            break;
        if (type == kAWDLSyncParamsTLV && tlen >= 31) {
            uint16_t down = v[1] | (v[2] << 8);
            uint16_t period = v[5] | (v[6] << 8);
            uint16_t pm = v[27];
            uint16_t nextSeq = v[29] | (v[30] << 8);
            if (period >= 4 && period <= 64 && pm >= 1 && pm <= 16) {
                refUs = (int64_t)rxUs + (int64_t)down * 1024;
                slot = (uint32_t)period * pm * 1024;
                refSeq = (uint16_t)(nextSeq / pm * pm + pm);
                pmv = pm;
                memcpy(master, v + 21, sizeof(master));
                sync = true;
            }
            if (tlen > 33)
                parseChan(v + 33, tlen - 33);
        } else if (type == kAWDLChanSeqTLV) {
            parseChan(v, tlen);
        }
        off += 3 + tlen;
    }
    if (!sync)
        return;
    PeerTl *e = peerTlFind(src, true, rxUs);
    if (!e)
        return;
    e->seenUs = rxUs;
    e->refUs = refUs;
    e->refSeq = refSeq;
    e->pm = pmv;
    e->slotUs = slot;
    if (haveChan) {
        memcpy(e->chan, chan, sizeof(e->chan));
        e->chanValid = true;
    }
    memcpy(&e->master, master, sizeof(e->master));
    if (rssiDbm < 0 && rssiDbm > -110)
        e->rssi = (int8_t)(e->rssi ? (3 * e->rssi + rssiDbm) / 4 : rssiDbm);
    // Is this peer on our timeline (same slot numbering, boundaries within kFollowTolUs)?
    if (slot == slotUs() && pmv == presenceMode()) {
        int64_t sl = (int64_t)slot;
        int64_t d = refUs - fRefUs;
        int64_t k = floorDiv(d + sl / 2, sl);
        int64_t err = d - k * sl;
        uint16_t ownSeq = (uint16_t)(fRefSeq + k * pmv);
        unsigned off = ((refSeq / pmv) % 16 + 16 - (ownSeq / pmv) % 16) % 16;
        if (off != 0 || err > kFollowTolUs || err < -kFollowTolUs)
            e->misaligned++;
        else
            e->aligned++;
    } else {
        e->misaligned++;
    }
}

// A peer that matters to us: inside the "bubble". IO80211Family only caches a peer while
// data flows (6-7 s at a time, measured 2026-11-14), so the cache alone leaves the peer
// untrusted during discovery, which is when it matters. Signal strength is the family's own
// criterion ("Not Routable ... outside bubble" at -70 dBm and below, the phone on the desk
// at -33..-45), so a strong, recently heard peer counts too.
bool AirportItlwmAWDLInterface::
entryTrusted(const PeerTl &e, uint64_t now) const
{
    if (!e.used)
        return false;
    if (e.cached)
        return true;
    if (fDataPeerUs && (int64_t)(now - fDataPeerUs) < (int64_t)kPeerReplaceUs &&
        !memcmp(&e.mac, &fDataPeerMac, sizeof(fDataPeerMac)))
        return true;
    return e.rssi && e.rssi >= fTrustRssi && (int64_t)(now - e.seenUs) < (int64_t)kPeerGateStaleUs;
}

bool AirportItlwmAWDLInterface::
peerTrusted(const uint8_t *mac, uint64_t now)
{
    if (PeerTl *e = peerTlFind(mac, false, now))
        return entryTrusted(*e, now);
    return fDataPeerUs && (int64_t)(now - fDataPeerUs) < (int64_t)kPeerReplaceUs &&
        !memcmp(mac, &fDataPeerMac, sizeof(fDataPeerMac));
}

void AirportItlwmAWDLInterface::
notePeerDataRx(const uint8_t *mac, uint64_t us)
{
    if (PeerTl *e = peerTlFind(mac, false, us))
        e->dataRxUs = us;
}

bool AirportItlwmAWDLInterface::
dwellRunning()
{
    ieee80211com *ic = halIc();
    uint8_t ch = 0;
    int st = (ic && ic->ic_awdl_dwell_state) ? ic->ic_awdl_dwell_state(ic, &ch) : 0;
    return st == 2 && ch == fTxChan;
}

// May a frame for dst leave now? Unicast: decided on dst's own timeline (is it on the
// channel of the running dwell, and inside its availability window). A destination we
// have no fresh timeline for counts as reachable, as an unknown peer always did.
// Group-addressed: unchanged, on the tracked peer (peerGateChannel / awGateOpen).
bool AirportItlwmAWDLInterface::
destGateOpen(const uint8_t *dst, uint64_t now)
{
    if (dst[0] & 1) {
        // Group-addressed (mDNS, ND): the peers that matter are the ones IO80211Family keeps
        // in its peer cache (in range, routable). Send while at least one of them is on the
        // dwell channel and inside its availability window (or in data mode). Before, this
        // followed the single tracked peer, which could be a neighbour's device outside the
        // bubble: the iPhone then never got the Mac's mDNS answers (2026-11-14, D9B251D7).
        bool any = false;
        for (unsigned i = 0; i < kPeerTlMax; i++) {
            PeerTl &e = fPeerTl[i];
            if (!e.used || !entryTrusted(e, now) || !e.slotUs || !e.pm ||
                (int64_t)(now - e.seenUs) > (int64_t)kPeerGateStaleUs)
                continue;
            any = true;
            int64_t slot = (int64_t)e.slotUs;
            int64_t k = floorDiv((int64_t)now - e.refUs, slot);
            if (fTxPump && e.chanValid) {
                unsigned pc = e.chan[((uint16_t)(e.refSeq + k * e.pm) / e.pm) % 16];
                if (pc && pc != fTxChan)
                    continue;
            }
            if (!fAwGate || (e.dataRxUs && (int64_t)(now - e.dataRxUs) < (int64_t)kAwGateDataModeUs)) {
                fGateMcCached++;
                return true;
            }
            int64_t aw = slot / e.pm;
            int64_t phase = (int64_t)now - e.refUs - k * slot;
            if (phase >= (int64_t)kAwGateHeadUs && phase < aw - (int64_t)kAwGateTailUs) {
                fGateMcCached++;
                return true;
            }
        }
        if (any)
            return false;
        // No cached peer with a fresh timeline: as before, on the tracked peer.
        fGateMcTracked++;
        if (fTxPump) {
            unsigned pc = peerGateChannel(now);
            if (pc && pc != fTxChan)
                return false;
        }
        return awGateOpen(now);
    }
    PeerTl *e = peerTlFind(dst, false, now);
    if (!e || !e->slotUs || !e->pm || (int64_t)(now - e->seenUs) > (int64_t)kPeerGateStaleUs) {
        fGateUnknownDst++;
        return true;
    }
    int64_t slot = (int64_t)e->slotUs;
    int64_t k = floorDiv((int64_t)now - e->refUs, slot);
    if (fTxPump && e->chanValid) {
        uint16_t seq = (uint16_t)(e->refSeq + k * e->pm);
        unsigned pc = e->chan[(seq / e->pm) % 16];
        if (pc && pc != fTxChan) {
            fGateDstAway++;
            return false;
        }
    }
    if (!fAwGate)
        return true;
    // Traffic in progress with this peer: it keeps its extension windows open.
    if (e->dataRxUs && (int64_t)(now - e->dataRxUs) < (int64_t)kAwGateDataModeUs)
        return true;
    int64_t aw = slot / e->pm;
    int64_t phase = (int64_t)now - e->refUs - k * slot;
    if (phase >= (int64_t)kAwGateHeadUs && phase < aw - (int64_t)kAwGateTailUs)
        return true;
    fGateDstAw++;
    return false;
}

void AirportItlwmAWDLInterface::
publishPeerTl()
{
    char buf[kPeerTlMax * 72 + 1] = {};
    uint64_t now = hostUs();
    size_t n = 0;
    unsigned count = 0;
    for (unsigned i = 0; i < kPeerTlMax && n + 72 < sizeof(buf); i++) {
        PeerTl &e = fPeerTl[i];
        if (!e.used || (int64_t)(now - e.seenUs) > (int64_t)kPeerStaleUs)
            continue;
        unsigned pc = 0;
        if (e.chanValid && e.slotUs && e.pm) {
            int64_t k = floorDiv((int64_t)now - e.refUs, (int64_t)e.slotUs);
            pc = e.chan[((uint16_t)(e.refSeq + k * e.pm) / e.pm) % 16];
        }
        // mac(last 3)[* cached | + trusted] age channel rssi m=master(last 2) a=aligned/misaligned
        n += snprintf(buf + n, sizeof(buf) - n, "%s%02x:%02x:%02x%s %llums ch%u %ddBm m=%02x:%02x a=%u/%u",
                      count ? ", " : "", e.mac.octet[3], e.mac.octet[4], e.mac.octet[5],
                      e.cached ? "*" : entryTrusted(e, now) ? "+" : "",
                      (unsigned long long)((now - e.seenUs) / 1000), pc, (int)e.rssi,
                      e.master.octet[4], e.master.octet[5], e.aligned, e.misaligned);
        count++;
    }
    setProperty("AWDLPeers", buf);
    setProperty("AWDLPeerCount", count, 32);
    char g[240];
    snprintf(g, sizeof(g), "skips %llu unknown %llu away %llu aw %llu evict %llu mc cached %llu tracked %llu "
             "switchcached %llu followuntrusted %llu",
             fGateSkips, fGateUnknownDst, fGateDstAway, fGateDstAw, fPeerTlEvict, fGateMcCached, fGateMcTracked,
             fPeerSwitchCached, fFollowUntrusted);
    setProperty("AWDLDestGate", g);
}

// Channel of the tracked peer for the TX gate. A peer that was not heard for
// kPeerGateStaleUs counts as unknown (0): its extrapolated timeline is no
// longer evidence, and holding every queued frame (all destinations share the
// queue) on it stalled the queue for tens of seconds.
unsigned AirportItlwmAWDLInterface::
peerGateChannel(uint64_t us)
{
    if (!fPeerValid)
        return 0;
    if ((int64_t)(us - fPeerSeenUs) > (int64_t)kPeerGateStaleUs) {
        fPeerGateStale++;
        return 0;
    }
    return peerChannelAt(us);
}

void AirportItlwmAWDLInterface::
noteDataPeer(const uint8_t *mac)
{
    if (!mac || (mac[0] & 1))
        return;
    memcpy(&fDataPeerMac, mac, sizeof(fDataPeerMac));
    fDataPeerUs = hostUs();
}

// Arm the listen timer for the next run of peer slots we should visit.
uint8_t AirportItlwmAWDLInterface::
ownChannelAt(uint16_t seq) const
{
    unsigned count = fChanSeqValid ? fChanSeq[kChanSeqCount] : 0;
    if (!count)
        return 0;
    uint16_t pm = presenceMode();
    const apple80211_channel *c = (const apple80211_channel *)(fChanSeq + kChanSeqEntries) + (seq / pm) % count;
    return (uint8_t)c->channel;
}

// Next slot of our own timeline whose channel is not 0 (0 = not on AWDL, the
// firmware stays on the infra channel and posts no AW start) and is not the
// station channel. Dwell for the run of equal-channel slots.
bool AirportItlwmAWDLInterface::
scheduleOwnListen(uint64_t afterUs)
{
    if (!fListenTimer || !fAWRunning || !fChanSeqValid || !fController)
        return false;
    ieee80211com *ic = halIc();
    if (!ic || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss)
        return false;
    unsigned home = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
    uint64_t now = hostUs();
    // Data frames are only accepted while a peer is around (see ic_awdl_want_data);
    // the P2P MAC picks the change up before the next dwell.
    ic->ic_awdl_want_data = fPeerValid && now - fPeerSeenUs <= kPeerStaleUs;
    uint64_t from = afterUs > now ? afterUs : now;
    int64_t slot = (int64_t)slotUs();
    uint16_t pm = presenceMode();
    const int64_t lead = listenLeadUs(now);
    int64_t k = floorDiv((int64_t)from + lead - fRefUs, slot) + 1;
    // The peer we track (the phone during AirDrop) must be heard at least every
    // few seconds or IO80211Family drops it from NDP (lastAF ~6 s). When our own
    // slot keeps us home (or off AWDL) but the peer's slot at that time is on a
    // non-home channel, listen there instead (on unless -itlawdlnopeerlisten).
    bool usePeer = awPeerListenAllowed() && fPeerValid && fPeerChanValid && fPeerSlotUs &&
        fPeerPm && now - fPeerSeenUs <= kPeerStaleUs;
    for (int i = 0; i < 32; i++, k++) {
        uint16_t seq = (uint16_t)(fRefSeq + k * pm);
        uint8_t chan = ownChannelAt(seq);
        bool fromPeer = false;
        // -itlawdlhometx: a slot on the station channel is a usable slot too (the
        // peer is there as well); the P2P MAC dwells on it like on any other.
        bool homeOk = fHomeTx && chan && chan == home;
        if ((!chan || (chan == home && !homeOk)) && usePeer) {
            unsigned pc = peerChannelAt((uint64_t)(fRefUs + k * slot + slot / 2));
            if (pc && (pc != home || fHomeTx)) {
                chan = (uint8_t)pc;
                fromPeer = true;
            }
        }
        if (!chan || (chan == home && !fHomeTx))
            continue;
        unsigned run = 1;
        if (!fromPeer) {
            while (run < 16 && ownChannelAt((uint16_t)(seq + run * pm)) == chan)
                run++;
        }
        if (fromPeer)
            fListenPeerSlots++;
        uint32_t dwell = dwellTUFor(now, run, slot, pm);
        if (dwell > fListenDwellTU)
            dwell = fListenDwellTU;
        dwell += fDwellExtraTU;             // itlawdldwellx=, 0 by default
        fListenNextChan = chan;
        fListenNextDwellTU = dwell;
        fListenNextLeadUs = lead;
        uint64_t abs;
        nanoseconds_to_absolutetime((uint64_t)(fRefUs + k * slot - lead) * 1000, &abs);
        fListenTimer->cancelTimeout();
        fListenTimer->wakeAtTime(abs);
        fListenAlignedActive = true;
        return true;
    }
    fListenNoSlot++;
    return false;
}

void AirportItlwmAWDLInterface::
scheduleAlignedListen(uint64_t afterUs)
{
    fListenAlignedActive = false;
    if (awDataAllowed()) {
        scheduleOwnListen(afterUs);
        return;
    }
    if (!fListenTimer || !fListenAlign || !fAWRunning || !fPeerValid || !fPeerChanValid || !fController)
        return;
    uint64_t now = hostUs();
    if (now - fPeerSeenUs > kPeerStaleUs)
        return;
    ieee80211com *ic = halIc();
    if (!ic || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss)
        return;
    unsigned home = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
    unsigned forced = awListenChannelOverride();
    int64_t slot = fPeerSlotUs;
    uint64_t from = afterUs > now ? afterUs : now;
    const int64_t lead = listenLeadUs(now);
    int64_t k = floorDiv((int64_t)from + lead - fPeerRefUs, slot) + 1;
    for (int i = 0; i < 32; i++, k++) {
        uint16_t seq = (uint16_t)(fPeerRefSeq + k * fPeerPm);
        unsigned chan = fPeerChan[(seq / fPeerPm) % 16];
        if (!chan || chan == home || (forced && chan != forced))
            continue;
        // Stay for the whole run of slots on this channel, up to the dwell limit.
        unsigned run = 1;
        while (run < 16 && fPeerChan[((seq / fPeerPm) + run) % 16] == chan)
            run++;
        uint32_t dwell = dwellTUFor(now, run, slot, fPeerPm);
        if (dwell > fListenDwellTU)
            dwell = fListenDwellTU;
        fListenNextChan = (uint8_t)chan;
        fListenNextDwellTU = dwell;
        fListenNextLeadUs = lead;
        uint64_t abs;
        nanoseconds_to_absolutetime((uint64_t)(fPeerRefUs + k * slot - lead) * 1000, &abs);
        fListenTimer->cancelTimeout();
        fListenTimer->wakeAtTime(abs);
        fListenAlignedActive = true;
        return;
    }
    fListenNoSlot++;
}

void AirportItlwmAWDLInterface::
listenTimerFired(OSObject *owner, IOTimerEventSource *)
{
    AirportItlwmAWDLInterface *that = OSDynamicCast(AirportItlwmAWDLInterface, owner);
    if (!that || !that->fListenAlignedActive || !that->fAWRunning || !that->fController)
        return;
    uint64_t now = hostUs();
    uint32_t dwell = that->fListenNextDwellTU;
    ieee80211com *ic = that->halIc();
    if (ic && ic->ic_awdl_listen && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss) {
        unsigned home = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
        bool homeOk = that->fHomeTx && awDataAllowed();
        if ((home != that->fListenNextChan || homeOk) && !(++that->fListenSlot % that->fListenEveryNth)) {
            uint64_t before = that->fListenStarted;
            that->startListen(that->fListenNextChan, dwell, home, true);
            if (that->fListenStarted != before) {
                if (awDataAllowed())
                    that->fListenOwn++;
                that->fTxReqUs = now;
                that->scheduleTx(that->fListenNextChan, now + that->fListenNextLeadUs);
            }
        }
    }
    that->scheduleAlignedListen(now + (uint64_t)dwell * 1024);
}

#pragma mark - stage 3 step 1: transmit (host side of the firmware PSF / MI engine)

// The peer manager hands down TLVs only (see updateSyncFrameTemplate). Broadcom firmware
// adds the 802.11 header, the AWDL fixed header, the synchronization
// parameters (TLV 4) and the channel sequence (TLV 18). We do the same and
// send during an aligned listen dwell, on the P2P station's queue, in the
// peer's AW: PSF 1 ms and MI 4 ms after the peer's slot boundary.
static const uint8_t kAWDLBssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
static const int64_t kTxPSFAtUs = 1000;
static const int64_t kTxMIAtUs = 4000;
enum : uint8_t { kAWDLSubtypePSF = 0, kAWDLSubtypeMI = 3 };

void AirportItlwmAWDLInterface::
scheduleTx(uint8_t chan, uint64_t slotStartUs)
{
    if (!awTxAllowed() || !fTxTimer || !fAWRunning)
        return;
    // Diagnostics: if the previous dwell's START was not seen yet, look now.
    noteRocStart();
    // AF TX mode 3 ("awdl_aftxmode suppress" in IO80211AWDLPeerManager::setAFTXMode) is set by
    // the family in the Data and Infra Priority schedule states. It silences the periodic
    // sync frame (PSF) only: the original port skipped the whole TX window here, so in those
    // states no master indication and no data left the Mac (26.7.2, 2026-11-12: Infra Priority
    // with 256 frames queued, AWDLDataTx 0, the Mac never showed up on the iPhone).
    if (fAfTxMode == 3 && !isSyncMaster())
        fTxSkipMode++;              // PSF suppressed in this window (never while sync master)
    if (fTxPhase == 3 && !fTxPumpSawRun)
        fPumpNoRun++;               // previous pump window never saw its dwell run
    fTxChan = chan;
    fTxSlotUs = slotStartUs;
    fTxStartNoted = false;
    uint64_t abs;
    if (fTxPump) {
        // Pump until the dwell is over: requested length plus a margin for a
        // late START (the firmware has been seen to start up to ~80 ms late).
        fTxPhase = 3;
        fTxPumpPsfDone = fTxPumpMiDone = fTxPumpSawRun = false;
        fTxPumpEndUs = slotStartUs + (uint64_t)fListenNextDwellTU * 1024 + 100000;
        fPumpWindows++;
        nanoseconds_to_absolutetime((slotStartUs + kTxPSFAtUs) * 1000, &abs);
        fTxTimer->cancelTimeout();
        fTxTimer->wakeAtTime(abs);
        return;
    }
    fTxPhase = 1;
    nanoseconds_to_absolutetime((slotStartUs + kTxPSFAtUs) * 1000, &abs);
    fTxTimer->cancelTimeout();
    fTxTimer->wakeAtTime(abs);
}

// Record when the firmware actually started the dwell that was planned for
// fTxSlotUs (histogram AWDLRocStartVsSlot). Cheap; called from the TX paths.
void AirportItlwmAWDLInterface::
noteRocStart()
{
    if (fTxStartNoted || !fController || !fTxReqUs)
        return;
    ieee80211com *ic = halIc();
    if (!ic || ic->ic_awdl_roc_start_us <= fTxReqUs)
        return;
    fTxStartNoted = true;
    int64_t off = (int64_t)ic->ic_awdl_roc_start_us - (int64_t)fTxSlotUs;
    // ms buckets: <0 (early) <2 <5 <10 <20 <40 <80 >=80
    static const int64_t lim[7] = { 0, 2000, 5000, 10000, 20000, 40000, 80000 };
    int i = 0;
    while (i < 7 && off >= lim[i])
        i++;
    fRocStartHist[i]++;
}

// Length and lead of the next listen dwell.
//
// Every dwell takes the radio off the station channel. Staying for the whole slot plus the
// lead (65 + 30 ms, three times per 1.05 s supercycle, two of them back to back) left the
// infrastructure link dead for 250-490 ms at a time: ping to the gateway averaged ~100 ms
// (max 450-740 ms) against 6 ms with awdl0 down (2026-11-12, 25G332, AP on 100/80).
// A real idle AWDL device only has to be on the social channel for the availability window
// (presence mode 4 = one AW in four); the extension windows matter only while unicast
// traffic flows. So: idle -> AW + a short lead, traffic within kIdleAfterUs -> as before.
bool AirportItlwmAWDLInterface::
dwellIdle(uint64_t now) const
{
    if (!fIdleDwell)
        return false;
    if (fDataQCount)
        return false;
    if (fDataRxUcUs && (int64_t)(now - fDataRxUcUs) < (int64_t)kIdleAfterUs)
        return false;
    if (fDataTxUcUs && (int64_t)(now - fDataTxUcUs) < (int64_t)kIdleAfterUs)
        return false;
    return true;
}

int64_t AirportItlwmAWDLInterface::
listenLeadUs(uint64_t now) const
{
    return dwellIdle(now) && fListenLeadUs > kIdleLeadMaxUs ? kIdleLeadMaxUs : fListenLeadUs;
}

uint32_t AirportItlwmAWDLInterface::
dwellTUFor(uint64_t now, unsigned run, int64_t slot, uint16_t pm)
{
    if (dwellIdle(now) && pm) {
        fListenIdle++;
        return (uint32_t)((slot / pm + listenLeadUs(now) + (int64_t)kIdleTailUs) / 1024);
    }
    fListenFull++;
    return (uint32_t)((run * slot + fListenLeadUs) / 1024);
}

// Data-path packet counts for the peer manager.
//
// IO80211AWDLPeerManager::monitorAWDLDataMode decides whether AWDL carries traffic from two
// counters in the peer manager (+0x7b0 tx, +0x79c rx: "In AW: Tx:%d Rx:%d"). A firmware
// driver feeds them through IO80211SkywalkInterface::updateInterfaceDataStats (vtable +0xe28,
// -> IO80211PeerManager::updateLocalCounters, which copies stats+0x08 and stats+0x18).
// awdl0's data goes through our own IOEthernetInterface, so the counters stayed at 0, the
// peer manager never saw a transfer (HasActAirDrop 0, indatamode 0) and kept the "Low Power"
// schedule of 3 slots in 16: AirDrop ran at ~400 KB/s. Report the real counts once per slot.
// The structure is read up to +0x680 (four latency bin blocks of 0x140 from +0x180); the
// fields we do not set are zero.
// IO80211VirtualInterface::cachePeer(ether_addr *, unsigned int *result). A native driver asks
// for this before unicast to a peer: the family puts the peer in its "firmware" peer cache
// (IO80211P2PDataPathManager::addPeerToFWCache -> our setPEER_CACHE_CONTROL) and counts it as
// cached. awdl0's data does not go through the family's TX path here, so nobody ever asked:
// "peers:3, cached:0" during an AirDrop. With no cached peer the family applies the idle rule
// (isInfraRealtimePacketThresholdAllowed: any 50 infra packets = Real Time), keeps the
// schedule in Infra Priority (3 slots in 16) and the transfer crawls (~60 KB/s, 2026-11-13).
// Off with -itlawdlnopeercache.
extern "C" bool _ZN23IO80211VirtualInterface9cachePeerEP10ether_addrPj(
    IO80211VirtualInterface *, ether_addr *, unsigned int *);
// cachePeer refuses ("Data is disabled on the interface") while either flag of the interface
// is set: suspended (+0x54: quiet mode, cleared by the family when AWDL resumes) or
// peer-manager data disabled (+0x55). IO80211AWDLPeerManager::reset() sets the second one and
// nothing in the family clears it again; awdl0 lives across resets here, so once AWDL is
// running again the driver clears it, as for a freshly created interface.
extern "C" bool _ZN23IO80211VirtualInterface20isInterfaceSuspendedEv(IO80211VirtualInterface *);
extern "C" bool _ZN23IO80211VirtualInterface26getPeerManagerDataDisabledEv(IO80211VirtualInterface *);
extern "C" void _ZN23IO80211VirtualInterface26setPeerManagerDataDisabledEb(IO80211VirtualInterface *, bool);

void AirportItlwmAWDLInterface::
cacheDataPeer()
{
#if !defined(AIRPORT_AWDL_ABI_TAHOE)
    // Sequoia: the vtable slot, message format or stats layout used here were verified
    // on Tahoe 25G332 only (IO80211VirtualInterface::findPeer is at 0xcc0 on 15.0.1, not
    // 0xcd0). Off until checked against a 15.x kernel collection.
    return;
#endif
    static int allowed = -1;
    if (allowed < 0) {
        int v;
        allowed = PE_parse_boot_argn("-itlawdlnopeercache", &v, sizeof(v)) ? 0 : 1;
    }
    if (allowed != 1 || !fDataPeerUs)
        return;
    uint64_t now = hostUs();
    if ((int64_t)(now - fDataPeerUs) > 2000000)
        return;                             // no unicast data with a peer right now
    if (!memcmp(&fCachedPeerMac, &fDataPeerMac, sizeof(fCachedPeerMac)) &&
        (int64_t)(now - fCachedPeerUs) < 5000000)
        return;                             // asked for this peer a moment ago
    // Third condition of cachePeer: IO80211SkywalkInterface::isInterfaceEnabled(). A native
    // driver's virtual interface is enabled through IO80211VirtualInterface::setInterfaceEnable
    // when the system brings it up; awdl0 is our own IOEthernetInterface, so the family never
    // saw it enabled (3DE74834: suspended 0, dataoff 0, still "Data is disabled").
    // -itlawdlnoifenable keeps the old state.
    static int ifEnable = -1;
    if (ifEnable < 0) {
        int v;
        ifEnable = PE_parse_boot_argn("-itlawdlnoifenable", &v, sizeof(v)) ? 0 : 1;
    }
    bool wasEnabled = isInterfaceEnabled();
    if (!wasEnabled && ifEnable == 1 && fAWRunning) {
        setInterfaceEnable(true);
        fPeerCacheIfEnabled++;
    }
    bool suspended = _ZN23IO80211VirtualInterface20isInterfaceSuspendedEv(this);
    bool dataOff = _ZN23IO80211VirtualInterface26getPeerManagerDataDisabledEv(this);
    if (dataOff && fAWRunning && !suspended) {
        _ZN23IO80211VirtualInterface26setPeerManagerDataDisabledEb(this, false);
        fPeerCacheDataOn++;
    }
    unsigned int result = 0;
    ether_addr mac = fDataPeerMac;
    bool ok = _ZN23IO80211VirtualInterface9cachePeerEP10ether_addrPj(this, &mac, &result);
    fCachedPeerMac = fDataPeerMac;
    fCachedPeerUs = now;
    fPeerCacheReq++;
    char g[192];
    snprintf(g, sizeof(g), "req %llu last %s ok %d result %u ifenabled %d (set %llu) suspended %d dataoff %d cleared %llu completed %llu stamps %llu",
             fPeerCacheReq, ether_sprintf((uint8_t *)&mac), ok ? 1 : 0, result, wasEnabled ? 1 : 0,
             fPeerCacheIfEnabled, suspended ? 1 : 0, dataOff ? 1 : 0, fPeerCacheDataOn, fPeerCacheCompleted, fPeerActivityStamps);
    setProperty("AWDLPeerCacheReq", g);
}

// The family drops a cached peer "Due to Inactivity" when four per-peer time stamps are all
// older than its threshold (IO80211AWDLPeer::monitorCachedPeerActivity): last packet queued
// for the peer, last unicast received from it, and two more it keeps itself. Its own TX and
// RX paths set them; awdl0's data bypasses those, so a peer was uncached ~3.7 s after every
// add, in the middle of a transfer (7FBBB54D, 2026-11-13: Mac -> iPhone ran at ~700 kB/s for
// 13 s and stopped dead at the uncache). Stamp the peer from our own unicast activity.
extern "C" void _ZN11IO80211Peer18setLastQueuePacketEy(void *peer, unsigned long long);
extern "C" void _ZN11IO80211Peer28setLastRxUnicastLinkActivityEy(void *peer, unsigned long long);
enum : size_t { kIfFindPeer = 0xcd0 };      // IO80211VirtualInterface::findPeer(ether_addr&), 25G332

void AirportItlwmAWDLInterface::
stampDataPeerActivity()
{
#if !defined(AIRPORT_AWDL_ABI_TAHOE)
    // Sequoia: the vtable slot, message format or stats layout used here were verified
    // on Tahoe 25G332 only (IO80211VirtualInterface::findPeer is at 0xcc0 on 15.0.1, not
    // 0xcd0). Off until checked against a 15.x kernel collection.
    return;
#endif
    if (!fDataPeerUs)
        return;
    uint64_t now = hostUs();
    const int64_t recent = 1000000;
    bool tx = fDataTxUcUs && (int64_t)(now - fDataTxUcUs) < recent;
    bool rx = fDataRxUcUs && (int64_t)(now - fDataRxUcUs) < recent;
    if ((!tx && !rx) || (int64_t)(now - fDataPeerUs) > recent)
        return;
    ether_addr mac = fDataPeerMac;
    void *peer = vcall<void *>(this, kIfFindPeer, &mac);
    if (!peer)
        return;
    uint64_t abs = mach_absolute_time();
    if (tx)
        _ZN11IO80211Peer18setLastQueuePacketEy(peer, abs);
    if (rx)
        _ZN11IO80211Peer28setLastRxUnicastLinkActivityEy(peer, abs);
    fPeerActivityStamps++;
}

void AirportItlwmAWDLInterface::
reportDataStats()
{
    completePeerCacheOps();
    stampDataPeerActivity();
    cacheDataPeer();
#if !defined(AIRPORT_AWDL_ABI_TAHOE)
    return;     // updateInterfaceDataStats slot (0xe28) and stats layout: Tahoe 25G332 only
#endif
    if (!fDataStats || (fDataTx == fStatsTx && fDataRx == fStatsRx))
        return;
    static uint8_t stats[0x800];            // work loop only
    bzero(stats, sizeof(stats));
    uint32_t tx = (uint32_t)fDataTx, rx = (uint32_t)fDataRx;
    memcpy(stats + 0x08, &tx, sizeof(tx));
    memcpy(stats + 0x18, &rx, sizeof(rx));
    // The family's own per-packet paths (IO80211PeerManager::skywalkLogTxPacket /
    // skywalkInputPacket) also count every packet in two totals, which
    // IO80211PeerManager::updateLocalCounters takes from +0x30 (tx) and +0xd8 (rx).
    memcpy(stats + 0x30, &tx, sizeof(tx));
    memcpy(stats + 0xd8, &rx, sizeof(rx));
    IOReturn ret = vcall<IOReturn>(this, kIfUpdateInterfaceDataStats, stats);
    fStatsTx = fDataTx;
    fStatsRx = fDataRx;
    fStatsReports++;
    fStatsLastRet = ret;
}

// Availability-window gate. Every 64-TU slot of the AWDL sequence starts with one
// availability window (the sync period, 16 TU) in which a peer on that channel must
// listen; the rest of the slot is extension windows, which an idle peer skips and only
// a peer with traffic in progress stays awake for. Frames handed to the firmware
// "as soon as the dwell runs" therefore reach an idle iPhone only by luck: on
// 2026-11-12 (25G332) it asked 11 times in 104 s for the Mac's SRV record, each answer
// left within 5 ms, and none was received, so the Mac took ~2 min to show up in the
// share sheet. Apple's own driver sends group-addressed frames inside the AW.
// Returns false when the frame at the head of the queue should wait for the next AW.
bool AirportItlwmAWDLInterface::
awGateOpen(uint64_t now)
{
    if (!fAwGate || !fPeerValid || !fPeerSlotUs || !fPeerPm)
        return true;
    if ((int64_t)(now - fPeerSeenUs) > (int64_t)kPeerGateStaleUs)
        return true;                    // timeline no longer evidence
    // Traffic in progress: the peer keeps its extension windows open (data mode) and
    // holding frames would only add latency to ACKs.
    if (fDataRxUcUs && (int64_t)(now - fDataRxUcUs) < (int64_t)kAwGateDataModeUs)
        return true;
    int64_t slot = (int64_t)fPeerSlotUs;
    int64_t aw = slot / fPeerPm;
    int64_t phase = (int64_t)now - fPeerRefUs;
    phase -= floorDiv(phase, slot) * slot;
    // Guard both edges: the peer timeline is known to a few ms (PeerErrUs), and the
    // frame still has to get through the firmware queue.
    return phase >= (int64_t)kAwGateHeadUs && phase < aw - (int64_t)kAwGateTailUs;
}

// Whether the firmware dwell runs on fTxChan right now and the peer is on that
// channel too (unknown peer timeline counts as yes, as in the original flush).
bool AirportItlwmAWDLInterface::
pumpCanSend(uint8_t *dwellChan, int *dwellState)
{
    ieee80211com *ic = halIc();
    uint8_t ch = 0;
    int st = (ic && ic->ic_awdl_dwell_state) ? ic->ic_awdl_dwell_state(ic, &ch) : 0;
    if (dwellChan)
        *dwellChan = ch;
    if (dwellState)
        *dwellState = st;
    if (st != 2 || ch != fTxChan)
        return false;
    unsigned pc = peerGateChannel(hostUs());
    if (pc && pc != fTxChan) {
        if (fDataQCount) {
            fGateHeldTicks++;
            if (fDataQMc[fDataQHead])
                fGateHeldMcTicks++;
        }
        fPumpPeerAway++;
        return false;
    }
    return true;
}

// -itlawdltxpump: instead of one PSF at +1 ms and one MI + data flush at +4 ms
// (which miss whenever the firmware starts the dwell later), poll every
// fPumpUs while the dwell window lasts and send as soon as it really runs.
void AirportItlwmAWDLInterface::
pumpTick()
{
    uint64_t now = hostUs();
    fPumpTicks++;
    noteRocStart();
    if (now > fTxPumpEndUs) {
        if (!fTxPumpSawRun)
            fPumpNoRun++;
        fTxPhase = 0;
        return;
    }
    uint8_t ch = 0;
    int st = 0;
    // Action frames keep following the tracked peer (pumpCanSend); queued data is
    // released per destination in flushData, so only the dwell itself is required.
    bool afOk = pumpCanSend(&ch, &st);
    bool dwellOk = st == 2 && ch == fTxChan;
    if (dwellOk) {
        if (afOk) {
            bool late = !fTxPumpSawRun && now > fTxSlotUs + kTxMIAtUs;
            fTxPumpSawRun = true;
            if (!fTxPumpPsfDone) {
                // AF TX mode 3 suppresses the PSF, but never while we are the sync master: the
            // peers that elected us need it to stay synchronised. Without it a real Mac
            // dropped us within seconds (2026-11-14: MacBook saw the Mac 127 ms after the
            // family went from mode 3 to 0, and never while master in mode 3).
            bool psfSuppressed = fAfTxMode == 3 && !isSyncMaster();
            if (fAfTxMode == 3 && !psfSuppressed && fSyncTmplLen)
                fTxPsfMasterMode3++;
            fTxPumpPsfDone = !fSyncTmplLen || psfSuppressed || sendAWDLFrame(false);
                if (fTxPumpPsfDone && late)
                    fPumpLateAF++;
            }
            if (fTxPumpPsfDone && !fTxPumpMiDone)
                fTxPumpMiDone = !fMITmplLen || sendAWDLFrame(true);
        }
        if (awDataAllowed())
            flushData();
    } else if (fTxPumpSawRun && st == 0) {
        // The dwell is over: nothing more can leave until the next one.
        fTxPhase = 0;
        if (((fTxPSF + fTxMI + fTxRefused) & 0x1f) <= 1)
            publishTxState();
        return;
    }
    uint64_t abs;
    nanoseconds_to_absolutetime((now + fPumpUs) * 1000, &abs);
    fTxTimer->wakeAtTime(abs);
}

void AirportItlwmAWDLInterface::
txTimerFired(OSObject *owner, IOTimerEventSource *)
{
    AirportItlwmAWDLInterface *that = OSDynamicCast(AirportItlwmAWDLInterface, owner);
    if (!that || !that->fTxPhase || !that->fAWRunning)
        return;
    if (that->fTxPhase == 3) {
        that->pumpTick();
        return;
    }
    if (that->fTxPhase == 1) {
        if (that->fSyncTmplLen && (that->fAfTxMode != 3 || that->isSyncMaster()))
            that->sendAWDLFrame(false);
        that->fTxPhase = 2;
        uint64_t abs;
        nanoseconds_to_absolutetime((that->fTxSlotUs + kTxMIAtUs) * 1000, &abs);
        that->fTxTimer->wakeAtTime(abs);
        return;
    }
    that->fTxPhase = 0;
    if (that->fMITmplLen)
        that->sendAWDLFrame(true);
    if (awDataAllowed()) {
        unsigned pc = that->peerGateChannel(hostUs());
        if (!pc || pc == that->fTxChan)
            that->flushData();
        else if (that->fDataQCount)
            that->fDataTxNotCommon++;
    }
    that->noteRocStart();
    if (((that->fTxPSF + that->fTxMI + that->fTxRefused) & 0x1f) <= 1)
        that->publishTxState();
}

// Sync parameters TLV as parsed by IO80211AWDLPeer::parseAwdlSyncParamsTlv
// (same layout as OWL's awdl_sync_params_tlv), times from our AW timeline.
size_t AirportItlwmAWDLInterface::
buildAWDLFrame(uint8_t *buf, size_t cap, bool mi, uint64_t nowUs)
{
    const uint8_t *tmpl = mi ? fMITmpl : fSyncTmpl;
    size_t tmplLen = mi ? fMITmplLen : fSyncTmplLen;
    // Channel entries are 2 bytes (op class encoding): the receiver's
    // IO80211AWDLPeerManager::isValidChanSeqTlv wants at least 2 * count + 2
    // bytes and drops the whole frame otherwise (iPhone: "bad chan seq tlv",
    // parse error reason 1, with the 1-byte encoding).
    const size_t syncLen = 3 + 33 + 6 + 32 + 2;
    const size_t seqLen = 3 + 6 + 32 + 3;
    size_t need = sizeof(struct ieee80211_frame) + kAWDLFixedHeaderLen + syncLen + seqLen + tmplLen;
    if (need > cap)
        return 0;
    memset(buf, 0, need);

    struct ieee80211_frame *wh = (struct ieee80211_frame *)buf;
    wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_ACTION;
    memset(wh->i_addr1, 0xff, 6);
    memcpy(wh->i_addr2, &fMac, 6);
    memcpy(wh->i_addr3, kAWDLBssid, 6);
    uint16_t sc = (uint16_t)(fTxSeq++ << 4);
    wh->i_seq[0] = sc & 0xff;
    wh->i_seq[1] = sc >> 8;

    uint8_t *af = buf + sizeof(*wh);
    af[0] = 127;
    af[1] = 0x00; af[2] = 0x17; af[3] = 0xf2;
    af[4] = 8;
    af[5] = 0x10;           // version 1.0; IO80211 rejects 0 (actionFrameHasCorrectOUI)
    af[6] = mi ? kAWDLSubtypeMI : kAWDLSubtypePSF;
    af[7] = 0;
    uint32_t t = (uint32_t)nowUs;
    memcpy(af + 8, &t, 4);  // phy_tx
    memcpy(af + 12, &t, 4); // target_tx

    // Our timeline: the extended AW (slot) we are in and the next boundary.
    uint16_t pm = presenceMode();
    uint32_t period = fSyncParams.availability_window_period;
    if (period < 4 || period > 64)
        period = 16;
    int64_t slot = (int64_t)slotUs();
    int64_t k = floorDiv((int64_t)nowUs - fRefUs, slot);
    int64_t start = fRefUs + k * slot;
    uint16_t seq = (uint16_t)(fRefSeq + k * pm);
    uint16_t curAW = (uint16_t)(seq + ((int64_t)nowUs - start) / ((int64_t)period * 1024));
    uint16_t down = (uint16_t)((start + slot - (int64_t)nowUs + 1023) / 1024);
    unsigned count = fChanSeqValid ? fChanSeq[kChanSeqCount] : 0;
    auto chanAt = [&](uint16_t s) -> uint8_t {
        if (!count)
            return 6;
        const apple80211_channel *c = (const apple80211_channel *)(fChanSeq + kChanSeqEntries) + (s / pm) % count;
        return (uint8_t)c->channel;
    };
    const ether_addr &master = isSyncMaster() ? fMac : fSyncMaster;
    uint8_t masterChan = fMasterChannel ? (uint8_t)fMasterChannel : 6;

    uint8_t *p = af + kAWDLFixedHeaderLen;
    auto put16 = [](uint8_t *q, uint16_t v) { q[0] = v & 0xff; q[1] = v >> 8; };
    // Global operating class of a 20 MHz channel (IEEE 802.11 Annex E).
    auto opClass = [](uint8_t c) -> uint8_t {
        if (!c) return 0;
        if (c <= 13) return 81;
        if (c <= 48) return 115;
        if (c <= 64) return 118;
        if (c <= 144) return 121;
        return 124;
    };
    auto chanSeq = [&](uint8_t *q) {
        q[0] = 15;          // count - 1
        q[1] = 3;           // op class encoding: channel, op class
        // Header bytes as AppleBCMWLANProximityInterface::buildAWDLChanSequence
        // takes them from the system's sequence: step +7, duplicate +8,
        // fill +9 (255 = 0xffff, repeat the current channel).
        q[2] = fChanSeqValid ? fChanSeq[8] : 0;
        q[3] = fChanSeqValid ? fChanSeq[kChanSeqStep] : (uint8_t)(pm - 1);
        uint8_t fill = fChanSeqValid ? fChanSeq[9] : 255;
        put16(q + 4, fill == 255 ? 0xffff : (uint16_t)(fill | (opClass(fill) << 8)));
        for (unsigned i = 0; i < 16; i++) {
            uint8_t c = count ? chanAt((uint16_t)(i * pm)) : 6;
            q[6 + 2 * i] = c;
            q[7 + 2 * i] = opClass(c);
        }
    };
    p[0] = kAWDLSyncParamsTLV;
    put16(p + 1, (uint16_t)(syncLen - 3));
    uint8_t *v = p + 3;
    v[0] = chanAt((uint16_t)(seq + pm));    // next AW channel
    put16(v + 1, down);                     // tx_down_counter
    v[3] = masterChan;
    v[4] = 0;                               // guard time
    put16(v + 5, (uint16_t)period);         // aw_period
    put16(v + 7, (uint16_t)(fSyncParams.synchronization_frame_period ?: 110));
    put16(v + 9, 0x1800);                   // flags, as OWL
    put16(v + 11, (uint16_t)period);        // aw_ext_length (16, else "Zero AWExt")
    put16(v + 13, (uint16_t)period);        // aw_com_length
    int64_t rem = (int64_t)period - (int64_t)((nowUs - start) / 1024);
    put16(v + 15, (uint16_t)(rem > 0 ? rem : 0));
    v[17] = v[18] = v[19] = v[20] = (uint8_t)(pm - 1);
    memcpy(v + 21, &master, 6);
    v[27] = (uint8_t)pm;
    put16(v + 29, curAW);                   // next_aw_seq
    put16(v + 31, curAW);                   // ap_alignment
    chanSeq(v + 33);
    p += syncLen;

    p[0] = kAWDLChanSeqTLV;
    put16(p + 1, (uint16_t)(seqLen - 3));
    chanSeq(p + 3);
    p += seqLen;

    memcpy(p, tmpl, tmplLen);
    return need;
}

bool AirportItlwmAWDLInterface::
sendAWDLFrame(bool mi)
{
    ieee80211com *ic = halIc();
    if (!ic || !ic->ic_awdl_tx)
        return false;
    static uint8_t frame[2048];             // work loop only
    size_t len = buildAWDLFrame(frame, sizeof(frame), mi, hostUs());
    if (!len) {
        fTxLastErr = 0x100;
        fTxRefused++;
        return false;
    }
    int err = ic->ic_awdl_tx(ic, fTxChan, frame, len);
    if (err) {
        fTxRefused++;
        fTxLastErr = err;
        return false;
    }
    fTxLastLen = (uint32_t)len;
    if (mi)
        fTxMI++;
    else
        fTxPSF++;
    return true;
}

void AirportItlwmAWDLInterface::
enqueueData(mbuf_t m)
{
    if (!m)
        return;
    if (fDataQCount >= kDataQueueLen) {
        fDataTxDropFull++;
        mbuf_freem(m);
        return;
    }
    unsigned slot = (fDataQHead + fDataQCount) % kDataQueueLen;
    fDataQ[slot] = m;
    fDataQUs[slot] = hostUs();
    fDataQMc[slot] = 0;
    {
        uint8_t dst[6];
        if (mbuf_pkthdr_len(m) >= 14 && mbuf_copydata(m, 0, sizeof(dst), dst) == 0) {
            noteDataPeer(dst);
            if (dst[0] & 1) {
                fDataQMc[slot] = 1;
                fMcQueued++;
            } else {
                fDataTxUcUs = fDataQUs[slot];
            }
        }
    }
    fDataQCount++;
    fDataTxQueued++;
    // -itlawdltxpump: a frame queued while the dwell runs (typically a TCP ACK
    // or reply to what the peer just sent in this slot) leaves now instead of
    // waiting for the next dwell's flush point.
    if (fTxPump && fTxPhase == 3 && dwellRunning()) {
        fPumpImmediate++;
        flushData();
    }
}

void AirportItlwmAWDLInterface::
noteDataSent(unsigned idx)
{
    static_assert(sizeof(fDataQUs) / sizeof(fDataQUs[0]) == kDataQueueLen, "fDataQUs follows fDataQ");
    uint64_t lat = hostUs() - fDataQUs[idx];
    if (lat > fDataLatMaxUs)
        fDataLatMaxUs = lat;
    // ms buckets: <5 <20 <50 <100 <200 <500 <1000 >=1000
    static const uint64_t lim[7] = { 5000, 20000, 50000, 100000, 200000, 500000, 1000000 };
    int i = 0;
    while (i < 7 && lat >= lim[i])
        i++;
    fDataLatHist[i]++;
    if (fDataQMc[idx]) {
        fMcLatHist[i]++;
        fMcTx++;
    }
}

// A queued frame older than this is dropped at flush time (125 s old mDNS
// queries were seen leaving when AWDL came back up).
static const uint64_t kDataMaxAgeUs = 10000000;

// Ethernet -> 802.11 data (no QoS) + LLC/SNAP 00:17:f2 0x0800 + AWDL data
// header (0x0403 LE, sequence LE, pad, ethertype BE) + payload, as OWL's
// awdl_init_full_data_frame. Sent on the P2P queue while dwelling on the
// peer's channel; frames that cannot go out yet stay queued.
void AirportItlwmAWDLInterface::
flushData()
{
    ieee80211com *ic = halIc();
    if (!ic || !ic->ic_awdl_tx)
        return;
    static uint8_t frame[2048];             // work loop only
    // One pass over the queue in order. A frame whose destination is not reachable right
    // now stays queued (compacted towards the head, order kept) and does not block frames
    // for other destinations behind it. Once a destination was held in this pass, later
    // frames for it are held too, so the order per destination never changes.
    uint8_t held[8][6];
    unsigned nheld = 0;
    bool holdAllUc = false, mcDecided = false, mcOpen = true, stop = false, anyHeld = false;
    unsigned count = fDataQCount, w = 0;
    const size_t hdr = sizeof(struct ieee80211_frame), llc = 8, ad = 8;
    for (unsigned i = 0; i < count; i++) {
        unsigned idx = (fDataQHead + i) % kDataQueueLen;
        mbuf_t m = fDataQ[idx];
        size_t plen = mbuf_pkthdr_len(m);
        uint64_t nowUs = hostUs();
        bool keep = stop;
        uint8_t eh[14] = {};
        bool haveEh = plen >= 14 && mbuf_copydata(m, 0, sizeof(eh), eh) == 0;
        if (!keep && nowUs - fDataQUs[idx] > kDataMaxAgeUs) {
            // Queued while no dwell could carry it (AWDL idle, peer gone): the
            // stack has long retransmitted or given up, do not send it now.
            fDataTxDropStale++;
        } else if (!keep && (!haveEh || hdr + llc + ad + plen - 14 > sizeof(frame))) {
            fDataTxErr++;
        } else if (!keep) {
            bool open;
            if (eh[0] & 1) {
                if (!mcDecided) {
                    mcOpen = destGateOpen(eh, nowUs);
                    mcDecided = true;
                }
                open = mcOpen;
            } else {
                open = !holdAllUc;
                for (unsigned j = 0; open && j < nheld; j++)
                    if (!memcmp(held[j], eh, 6))
                        open = false;
                if (open && !destGateOpen(eh, nowUs)) {
                    open = false;
                    if (nheld < 8)
                        memcpy(held[nheld++], eh, 6);
                    else
                        holdAllUc = true;
                }
            }
            if (!open) {
                keep = true;
                anyHeld = true;
            } else {
                if (anyHeld)
                    fGateSkips++;
                if (fAwGate)
                    fAwGateSent++;
                memset(frame, 0, hdr + llc + ad);
                struct ieee80211_frame *wh = (struct ieee80211_frame *)frame;
                wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_DATA | IEEE80211_FC0_SUBTYPE_DATA;
                memcpy(wh->i_addr1, eh, 6);
                memcpy(wh->i_addr2, &fMac, 6);
                memcpy(wh->i_addr3, kAWDLBssid, 6);
                uint16_t sc = (uint16_t)(fTxSeq++ << 4);
                wh->i_seq[0] = sc & 0xff;
                wh->i_seq[1] = sc >> 8;
                uint8_t *l = frame + hdr;
                l[0] = 0xaa; l[1] = 0xaa; l[2] = 0x03;
                l[3] = 0x00; l[4] = 0x17; l[5] = 0xf2;
                l[6] = 0x08; l[7] = 0x00;
                uint8_t *a = l + llc;
                a[0] = 0x03; a[1] = 0x04;
                a[2] = fDataSeq & 0xff; a[3] = fDataSeq >> 8;
                a[6] = eh[12]; a[7] = eh[13];
                mbuf_copydata(m, 14, plen - 14, a + ad);
                int err = ic->ic_awdl_tx(ic, fTxChan, frame, hdr + llc + ad + plen - 14);
                if (err == EAGAIN) {
                    fDataTxDeferred++;
                    fTxSeq--;               // not sent: keep it and everything behind it
                    keep = true;
                    stop = true;
                } else if (err) {
                    fDataTxErr++;
                } else {
                    fDataTx++;
                    fDataSeq++;
                    noteDataSent(idx);
                    // if_output counts the bytes; packets come from the driver stats.
                    if (fController && fController->fAWDLBsdIf && fController->fAWDLBsdIf->getIfnet())
                        ifnet_stat_increment_out(fController->fAWDLBsdIf->getIfnet(), 1, 0, 0);
                }
            }
        }
        if (keep) {
            unsigned to = (fDataQHead + w) % kDataQueueLen;
            if (to != idx) {
                fDataQ[to] = fDataQ[idx];
                fDataQUs[to] = fDataQUs[idx];
                fDataQMc[to] = fDataQMc[idx];
                fDataQ[idx] = nullptr;
            }
            w++;
        } else {
            mbuf_freem(m);
            fDataQ[idx] = nullptr;
        }
    }
    fDataQCount = w;
    if (anyHeld)
        fAwGateHeld++;
    resumeOutput();
}

// Pull model: once our queue has room again, let the stack call outputStart().
void AirportItlwmAWDLInterface::
resumeOutput()
{
    if (!fOutputStalled || fDataQCount > kDataQueuePull / 2)
        return;
    fOutputStalled = false;
    fOutputResumes++;
    if (fController && fController->fAWDLBsdIf)
        fController->fAWDLBsdIf->signalOutputThread();
}

void AirportItlwmAWDLInterface::
publishDataState()
{
    setProperty("AWDLDataRx", fDataRx, 64);
    setProperty("AWDLDataRxBad", fDataRxBad, 64);
    setProperty("AWDLDataRxAmsdu", fDataRxAmsdu, 64);
    setProperty("AWDLDataRxNull", fDataRxNull, 64);
    {
        char g[96];
        snprintf(g, sizeof(g), "on %d held %llu sent %llu", fAwGate ? 1 : 0, fAwGateHeld, fAwGateSent);
        setProperty("AWDLTxAwGate", g);
        snprintf(g, sizeof(g), "on %d idle %llu full %llu", fIdleDwell ? 1 : 0, fListenIdle, fListenFull);
        setProperty("AWDLListenIdleDwell", g);
        snprintf(g, sizeof(g), "on %d reports %llu last 0x%x", fDataStats ? 1 : 0, fStatsReports, (unsigned)fStatsLastRet);
        setProperty("AWDLDataStats", g);
    }
    setProperty("AWDLDataRxAmsduSub", fDataRxAmsduSub, 64);
    setProperty("AWDLDataRxHtc", fDataRxHtc, 64);
    setProperty("AWDLDataRxFrag", fDataRxFrag, 64);
    if (fDataRxBadHeadLen) {
        char hex[sizeof(fDataRxBadHead) * 2 + 16];
        int o = snprintf(hex, sizeof(hex), "%u:", fDataRxBadLen);
        for (uint8_t i = 0; i < fDataRxBadHeadLen && o + 2 < (int)sizeof(hex); i++)
            o += snprintf(hex + o, sizeof(hex) - o, "%02x", fDataRxBadHead[i]);
        setProperty("AWDLDataRxBadLast", hex);
    }
    setProperty("AWDLDataRxNoBuf", fDataRxNoBuf, 64);
    setProperty("AWDLDataRxLastLen", fDataRxLastLen, 32);
    setProperty("AWDLDataRxLastType", fDataRxLastType, 32);
    setProperty("AWDLDataTx", fDataTx, 64);
    setProperty("AWDLDataTxQueued", fDataTxQueued, 64);
    setProperty("AWDLDataTxDropFull", fDataTxDropFull, 64);
    setProperty("AWDLDataTxDeferred", fDataTxDeferred, 64);
    setProperty("AWDLDataTxErr", fDataTxErr, 64);
    setProperty("AWDLDataTxPending", fDataQCount, 32);
    {
        char g[96];
        snprintf(g, sizeof(g), "%s starts %llu (0x%x) stalls %llu resumes %llu",
                 fController && fController->fAWDLBsdIf && fController->fAWDLBsdIf->fPullModel ? "pull" : "push",
                 fOutputStarts, (unsigned)fOutputStartRet, fOutputStalls, fOutputResumes);
        setProperty("AWDLOutputModel", g);
    }
    setProperty("AWDLDataTxNotCommon", fDataTxNotCommon, 64);
    setProperty("AWDLListenOwn", fListenOwn, 64);
    setProperty("AWDLListenPeerSlots", fListenPeerSlots, 64);
}

// Always-on TX timing diagnostics (strings, published with the TX state).
void AirportItlwmAWDLInterface::
publishTxDiag()
{
    char s[256];
    snprintf(s, sizeof(s), "pump %d every %u us, hometx %d, lead %lld us, dwell +%u TU, follow %d",
             fTxPump, fPumpUs, fHomeTx, fListenLeadUs, fDwellExtraTU, fFollowPeer);
    setProperty("AWDLTxModes", s);
    {
        // Who we follow, who the tracked peer follows, and whether the two
        // timelines agree (slotoff = peer sequence index - ours, err = boundary).
        char peer[24] = "none", pm[24] = "-";
        if (fPeerValid) {
            snprintf(peer, sizeof(peer), "%s", ether_sprintf(fPeerMac.octet));
            snprintf(pm, sizeof(pm), "%s", ether_sprintf(fPeerMaster.octet));
        }
        uint64_t now = hostUs();
        char ps[448];
        snprintf(ps, sizeof(ps), "role %s peer %s age %llu ms peermaster %s slotoff %u err %lld us aligned %llu "
                 "misaligned %llu followsusmis %llu adopts %llu gatestale %llu dataswitch %llu",
                 isSyncMaster() ? "master" : "slave", peer,
                 fPeerValid ? (now - fPeerSeenUs) / 1000 : 0ULL, pm, fPeerSlotOff, fPeerOwnErrUs,
                 fPeerAlignedCnt, fPeerMisalignedCnt, fPeerFollowsUsMis, fFollowAdopts, fPeerGateStale,
                 fPeerSwitchData);
        setProperty("AWDLPeerSync", ps);
    }
    snprintf(s, sizeof(s), "rx %llu queued %llu sent %llu held %llu heldmc %llu dropstale %llu lat <5:%u <20:%u <50:%u "
             "<100:%u <200:%u <500:%u <1000:%u >=1000:%u",
             fDataRxMc, fMcQueued, fMcTx, fGateHeldTicks, fGateHeldMcTicks, fDataTxDropStale,
             fMcLatHist[0], fMcLatHist[1], fMcLatHist[2], fMcLatHist[3],
             fMcLatHist[4], fMcLatHist[5], fMcLatHist[6], fMcLatHist[7]);
    setProperty("AWDLDataMcast", s);
    snprintf(s, sizeof(s), "<0:%u <2:%u <5:%u <10:%u <20:%u <40:%u <80:%u >=80:%u",
             fRocStartHist[0], fRocStartHist[1], fRocStartHist[2], fRocStartHist[3],
             fRocStartHist[4], fRocStartHist[5], fRocStartHist[6], fRocStartHist[7]);
    setProperty("AWDLRocStartVsSlotMs", s);
    snprintf(s, sizeof(s), "<5:%u <20:%u <50:%u <100:%u <200:%u <500:%u <1000:%u >=1000:%u",
             fDataLatHist[0], fDataLatHist[1], fDataLatHist[2], fDataLatHist[3],
             fDataLatHist[4], fDataLatHist[5], fDataLatHist[6], fDataLatHist[7]);
    setProperty("AWDLDataLatencyMs", s);
    setProperty("AWDLDataLatencyMaxMs", fDataLatMaxUs / 1000, 32);
    snprintf(s, sizeof(s), "windows %llu ticks %llu norun %llu peeraway %llu immediate %llu lateAF %llu",
             fPumpWindows, fPumpTicks, fPumpNoRun, fPumpPeerAway, fPumpImmediate, fPumpLateAF);
    setProperty("AWDLTxPump", s);
    ieee80211com *ic = halIc();
    if (!ic)
        return;
    const uint32_t *h = ic->ic_awdl_roc_delay_hist;
    snprintf(s, sizeof(s), "<2:%u <5:%u <10:%u <20:%u <40:%u <80:%u <160:%u >=160:%u",
             h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    setProperty("AWDLRocDelayHistMs", s);
    snprintf(s, sizeof(s), "af req %u run %u otherchan %u idle %u | data req %u run %u otherchan %u idle %u",
             ic->ic_awdl_tx_state_hist[0][0], ic->ic_awdl_tx_state_hist[0][1],
             ic->ic_awdl_tx_state_hist[0][2], ic->ic_awdl_tx_state_hist[0][3],
             ic->ic_awdl_tx_state_hist[1][0], ic->ic_awdl_tx_state_hist[1][1],
             ic->ic_awdl_tx_state_hist[1][2], ic->ic_awdl_tx_state_hist[1][3]);
    setProperty("AWDLTxVsDwell", s);
    snprintf(s, sizeof(s), "2g %u/%u unii1 %u/%u 5g-mid %u/%u unii3 %u/%u",
             ic->ic_awdl_tx_ok_band[0], ic->ic_awdl_tx_fail_band[0],
             ic->ic_awdl_tx_ok_band[1], ic->ic_awdl_tx_fail_band[1],
             ic->ic_awdl_tx_ok_band[2], ic->ic_awdl_tx_fail_band[2],
             ic->ic_awdl_tx_ok_band[3], ic->ic_awdl_tx_fail_band[3]);
    setProperty("AWDLTxOkFailByBand", s);
    snprintf(s, sizeof(s), "%u Mbps step %u (up %u down %u) peer %s caps 0x%x", ic->ic_awdl_tx_rate_mbps,
             ic->ic_awdl_tx_rate_idx, ic->ic_awdl_tx_rate_up, ic->ic_awdl_tx_rate_down,
             ether_sprintf(ic->ic_awdl_peer_mac), ic->ic_awdl_peer_caps);
    setProperty("AWDLDataRate", s);
}

void AirportItlwmAWDLInterface::
publishTxState()
{
    publishTxDiag();
    setProperty("AWDLTxPSF", fTxPSF, 64);
    setProperty("AWDLTxPsfMasterMode3", fTxPsfMasterMode3, 64);
    setProperty("AWDLTxMI", fTxMI, 64);
    setProperty("AWDLTxRefused", fTxRefused, 64);
    setProperty("AWDLTxSkipMode", fTxSkipMode, 64);
    setProperty("AWDLTxLastErr", fTxLastErr, 32);
    setProperty("AWDLTxLastLen", fTxLastLen, 32);
    ieee80211com *ic = halIc();
    if (!ic)
        return;
    setProperty("AWDLTxFwSent", ic->ic_awdl_tx_sent, 32);
    setProperty("AWDLTxFwBusy", ic->ic_awdl_tx_busy, 32);
    setProperty("AWDLTxFwErr", ic->ic_awdl_tx_err, 32);
    setProperty("AWDLTxFwOk", ic->ic_awdl_tx_ok, 32);
    setProperty("AWDLTxFwFail", ic->ic_awdl_tx_fail, 32);
    setProperty("AWDLTxFwLastStatus", ic->ic_awdl_tx_last_status, 32);
    setProperty("AWDLRxUcAction", ic->ic_awdl_rx_uc_action, 32);
    setProperty("AWDLRxUcData", ic->ic_awdl_rx_uc_data, 32);
    setProperty("AWDLRxUcOther", ic->ic_awdl_rx_uc_other, 32);
    setProperty("AWDLRxUcLastChannel", ic->ic_awdl_rx_uc_last_chan, 32);
    setProperty("AWDLFwUcMgmt", ic->ic_awdl_fw_uc_mgmt, 32);
    setProperty("AWDLFwUcData", ic->ic_awdl_fw_uc_data, 32);
    setProperty("AWDLFwUcBadCrc", ic->ic_awdl_fw_uc_badcrc, 32);
    setProperty("AWDLFwUcDropDecrypt", ic->ic_awdl_fw_uc_drop_decrypt, 32);
    setProperty("AWDLFwUcLastChannel", ic->ic_awdl_fw_uc_last_chan, 32);
    setProperty("AWDLFwUcLastFc", ic->ic_awdl_fw_uc_last_fc, 32);
    setProperty("AWDLFwForeignData", ic->ic_awdl_fw_foreign_data, 32);
    setProperty("AWDLPromisc", ic->ic_awdl_want_data ? true : false);
    setProperty("AWDLPromiscChanges", ic->ic_awdl_promisc_changes, 32);
    // Which net80211 drop path feeds the station's input errors.
    char st[256];
    snprintf(st, sizeof(st), "ccmp_dec %u unenc %u wepfail %u wrongdir %u wrongbss %u tooshort %u notassoc %u dup %u decap %u ctl %u",
             ic->ic_stats.is_ccmp_dec_errs, ic->ic_stats.is_rx_unencrypted, ic->ic_stats.is_rx_wepfail,
             ic->ic_stats.is_rx_wrongdir, ic->ic_stats.is_rx_wrongbss, ic->ic_stats.is_rx_tooshort,
             ic->ic_stats.is_rx_notassoc, ic->ic_stats.is_rx_dup, ic->ic_stats.is_rx_decap, ic->ic_stats.is_rx_ctl);
    setProperty("NetRxDropStats", st);
    setProperty("AWDLP2PAddrChanges", ic->ic_awdl_p2p_addr_changes, 32);
    if (ic->ic_awdl_rx_uc_action + ic->ic_awdl_rx_uc_data + ic->ic_awdl_rx_uc_other) {
        char src[24];
        snprintf(src, sizeof(src), "%s", ether_sprintf(ic->ic_awdl_rx_uc_last_src));
        setProperty("AWDLRxUcLastSrc", src);
    }
    char me[24];
    snprintf(me, sizeof(me), "%s", ether_sprintf(ic->ic_awdl_addr));
    setProperty("AWDLRadioAddr", me);
    if (ic->ic_awdl_bssid_valid) {
        char bs[24];
        snprintf(bs, sizeof(bs), "%s", ether_sprintf(ic->ic_awdl_bssid));
        setProperty("AWDLRadioBssid", bs);
    }
}

void AirportItlwmAWDLInterface::
publishSyncState()
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%s", ether_sprintf(fSyncMaster.octet));
    setProperty("AWDLSyncMaster", buf);
    setProperty("AWDLSyncMode", fSyncMode, 32);
    setProperty("AWDLAWRunning", fAWRunning);
    setProperty("AWDLAWStarts", fAWStarts, 64);
    setProperty("AWDLAWSeq", fLastSeq, 32);
    setProperty("AWDLSyncUpdates", fSyncUpdates, 64);
    setProperty("AWDLSyncErrUs", (uint64_t)fLastSyncErrUs, 32);
    setProperty("AWDLPresenceMode", presenceMode(), 32);
}

#pragma mark - creation, mirrors AppleBCMWLANCore::createProxInterface

IOReturn AirportItlwmSkywalkInterface::
setVIRTUAL_IF_CREATE(apple80211_virt_if_create_data *data)
{
    AirportItlwm *controller = OSDynamicCast(AirportItlwm, getController());
    if (!controller || !data)
        return kIOReturnBadArgument;
    return controller->createAWDLInterface(data);
}

IOReturn AirportItlwm::
createAWDLInterface(apple80211_virt_if_create_data *data)
{
    XYLog("AWDL %s: role %u enabled %d\n", __FUNCTION__, data->role, fAWDLEnabled);
    if (!fAWDLEnabled || data->role != kAirportItlwmRoleAWDL)
        return kIOReturnUnsupported;
    if (fAWDLIf) {
        strlcpy((char *)data->bsd_name, fAWDLIf->getBSDName() ?: "awdl0", sizeof(data->bsd_name));
        return kIOReturnSuccess;
    }

    ether_addr mac;
    memcpy(mac.octet, data->mac, sizeof(mac.octet));
    AirportItlwmAWDLInterface *awdl = AirportItlwmAWDLInterface::withController(this, &mac);
    if (!awdl)
        return kIOReturnNoMemory;
    if (!awdl->attach(this)) {
        XYLog("AWDL %s: attach failed\n", __FUNCTION__);
        awdl->release();
        return kIOReturnError;
    }

    // Same registration sequence the station interface uses in start().
    IOSkywalkEthernetInterface::RegistrationInfo info;
    memset(&info, 0, sizeof(info));
    if (!awdl->initRegistrationInfo(&info, 1, sizeof(info))) {
        XYLog("AWDL %s: initRegistrationInfo failed\n", __FUNCTION__);
        awdl->detach(this);
        awdl->release();
        return kIOReturnError;
    }
    awdl->mExpansionData->fRegistrationInfo = (IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(IOSkywalkNetworkInterface::RegistrationInfo));
    awdl->mExpansionData2->fRegistrationInfo = (IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(IOSkywalkEthernetInterface::RegistrationInfo));
    if (!awdl->mExpansionData->fRegistrationInfo || !awdl->mExpansionData2->fRegistrationInfo) {
        awdl->detach(this);
        awdl->release();
        return kIOReturnNoMemory;
    }
    memcpy(awdl->mExpansionData->fRegistrationInfo, &info, sizeof(info));
    memcpy(awdl->mExpansionData2->fRegistrationInfo, &info, sizeof(info));
    awdl->deferBSDAttach(true);
    if (!awdl->start(this)) {
        XYLog("AWDL %s: start failed\n", __FUNCTION__);
        awdl->detach(this);
        awdl->release();
        return kIOReturnError;
    }
    fAWDLIf = awdl;
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    // Optional on 24H32 (-itlawdlsetif): record awdl0 as the controller's AWDL
    // interface, as Unopcoooo's Sequoia path does. Plain setter, see IO80211ControllerV2.h.
    if (awSetIfAllowed()) {
        setAwdlInterface(awdl);
        fAWDLSetIf = true;
        XYLog("AWDL %s: setAwdlInterface\n", __FUNCTION__);
    }
#endif

    // The BSD ifnet comes from a dedicated IOEthernetInterface, as for the station.
    IONetworkInterface *bsd = nullptr;
    fAWDLMac = mac;
    fCreatingAWDL = true;
    bool ok = IONetworkController::attachInterface(&bsd, false);
    fCreatingAWDL = false;
    if (!ok || !bsd) {
        XYLog("AWDL %s: BSD interface attach failed\n", __FUNCTION__);
        destroyAWDLInterface();
        return kIOReturnError;
    }
    fAWDLBsdIf = OSDynamicCast(AirportItlwmAWDLEthernetInterface, bsd);
    // On a real Mac awdl0 is only a Skywalk interface, never an IONetworkInterface, so
    // SystemConfiguration does not list it as a network port. Ours is an IOEthernetInterface,
    // which configd would turn into a "Wi-Fi 2" service (DHCP on awdl0, and the service cannot
    // be removed). "HiddenInterface" is the IORegistry key configd skips in
    // __SCNetworkInterfaceCopyAll_IONetworkInterface (configd SCNetworkInterface.c,
    // kSCNetworkInterfaceHiddenInterfaceKey). Set on this object only: configd also searches
    // parents, so putting it on the controller would hide en0 as well.
    bsd->setProperty("HiddenInterface", kOSBooleanTrue);
    bsd->registerService();

    // The unit is assigned asynchronously by the network stack; awdl0 is the only
    // awdl interface on this machine, so report the name it will get.
    strlcpy((char *)data->bsd_name, "awdl0", sizeof(data->bsd_name));
    XYLog("AWDL %s: created %s\n", __FUNCTION__, data->bsd_name);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setAWDLLinkGated(OSObject *owner, void *arg0, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, owner);
    if (!that || !that->fAWDLIf)
        return kIOReturnNotReady;
    // Same call as AppleBCMWLANProximityInterface::bringupLink.
    that->fAWDLIf->setLinkState(arg0 ? kIO80211NetworkLinkUp : kIO80211NetworkLinkDown, 1);
    // The IOKit path that normally sets IFF_RUNNING is bypassed in performCommand.
    // Without it every send on awdl0 fails with ENETDOWN before reaching the driver.
    if (awDataAllowed() && that->fAWDLBsdIf && that->fAWDLBsdIf->getIfnet())
        ifnet_set_flags(that->fAWDLBsdIf->getIfnet(), arg0 ? IFF_RUNNING : 0, IFF_RUNNING);
    return kIOReturnSuccess;
}

void AirportItlwm::
destroyAWDLInterface()
{
    // Unpublish both objects and stop the AWDL timers inside the command gate
    // first: the RX handlers (awdlActionFrameHandler / awdlDataFrameHandler),
    // the timers and the gated half of outputAWDL all run on the work loop and
    // must not see an interface that is being torn down on this thread.
    AirportItlwmAWDLInterface *awdl = nullptr;
    AirportItlwmAWDLEthernetInterface *bsd = nullptr;
    struct Out { AirportItlwmAWDLInterface **awdl; AirportItlwmAWDLEthernetInterface **bsd; } out = { &awdl, &bsd };
    IOCommandGate::Action unpublish = [](OSObject *, void *self, void *arg, void *, void *) -> IOReturn {
        AirportItlwm *that = static_cast<AirportItlwm *>(self);
        Out *o = static_cast<Out *>(arg);
        *o->awdl = that->fAWDLIf;
        *o->bsd = that->fAWDLBsdIf;
        that->fAWDLIf = nullptr;
        that->fAWDLBsdIf = nullptr;
        if (*o->bsd)
            (*o->bsd)->setAWDLOwner(nullptr);
        if (*o->awdl)
            (*o->awdl)->teardownAW();
        return kIOReturnSuccess;
    };
    if (_fCommandGate)
        _fCommandGate->runAction(unpublish, this, &out);
    else
        unpublish(nullptr, this, &out, nullptr, nullptr);
    if (bsd) {
        XYLog("AWDL %s: dropped %llu packets\n", __FUNCTION__, bsd->droppedPackets());
        detachInterface(bsd, true);
        bsd->release();
    }
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    if (fAWDLSetIf) {
        setAwdlInterface(nullptr);
        fAWDLSetIf = false;
    }
#endif
    if (awdl) {
        awdl->stop(this);
        awdl->detach(this);
        awdl->release();
    }
}

// Log a setter that is accepted but not acted on yet.
#define AWDL_SET_STUB(NAME, TYPE)                       \
IOReturn AirportItlwmAWDLInterface::set##NAME(TYPE *d)  \
{                                                       \
    fCallCount++;                                       \
    AWDL_LOG("%p", d);                                  \
    return kIOReturnSuccess;                            \
}

// Getter without a known layout: tell the caller we have nothing.
#define AWDL_GET_STUB(NAME, TYPE)                       \
IOReturn AirportItlwmAWDLInterface::get##NAME(TYPE *d)  \
{                                                       \
    AWDL_LOG("unsupported");                            \
    return kIOReturnUnsupported;                        \
}

AirportItlwmAWDLInterface *AirportItlwmAWDLInterface::
withController(AirportItlwm *controller, ether_addr *mac)
{
    AirportItlwmAWDLInterface *ifp = new AirportItlwmAWDLInterface;
    if (!ifp)
        return nullptr;
    if (!ifp->init(controller, mac, kAirportItlwmRoleAWDL, "awdl")) {
        ifp->release();
        return nullptr;
    }
    ifp->fController = controller;
    // Which listen/TX backend the HAL runs in this boot (the coex backend leaves the
    // P2P listen callbacks unset, so maybeListen/startListen/sendAWDLFrame stay idle).
    ifp->setProperty("AWDLBackend", airportAWDLBackendName());
    // Runs on the controller work loop, like the rx path and the ioctls, so the
    // timeline state needs no extra locking.
    if (!awTimelineAllowed())
        return ifp;
    ifp->fAWTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityHigh,
                                                         ifp, &AirportItlwmAWDLInterface::awTimerFired);
    if (!ifp->fAWTimer || !controller->getWorkLoop() ||
        controller->getWorkLoop()->addEventSource(ifp->fAWTimer) != kIOReturnSuccess) {
        AWDL_LOG("AW timer setup failed");
        OSSafeReleaseNULL(ifp->fAWTimer);
    }
    if (awListenAllowed()) {
        ifp->fListenTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityHigh,
                                                                 ifp, &AirportItlwmAWDLInterface::listenTimerFired);
        if (!ifp->fListenTimer || !controller->getWorkLoop() ||
            controller->getWorkLoop()->addEventSource(ifp->fListenTimer) != kIOReturnSuccess) {
            AWDL_LOG("listen timer setup failed");
            OSSafeReleaseNULL(ifp->fListenTimer);
        }
    }
    if (awListenAllowed() && awTxAllowed()) {
        ifp->fTxTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityHigh,
                                                             ifp, &AirportItlwmAWDLInterface::txTimerFired);
        if (!ifp->fTxTimer || !controller->getWorkLoop() ||
            controller->getWorkLoop()->addEventSource(ifp->fTxTimer) != kIOReturnSuccess) {
            AWDL_LOG("tx timer setup failed");
            OSSafeReleaseNULL(ifp->fTxTimer);
        }
    }
    return ifp;
}

bool AirportItlwmAWDLInterface::
init(IO80211Controller *controller, ether_addr *mac, unsigned int role, char const *prefix)
{
    if (!IO80211AWDLProtocol::init(controller, mac, role, prefix)) {
        AWDL_LOG("super::init failed");
        return false;
    }
    fController = nullptr;
    memcpy(&fMac, mac, sizeof(fMac));
    memset(&fSyncParams, 0, sizeof(fSyncParams));
    fSyncEnabled = false;
    fSyncState = 0;
    fCallCount = 0;
    fRxAF = 0;
    fRxAFDropped = 0;
    fRxAFNoInject = 0;
    fAWTimer = nullptr;
    fAWRunning = false;
    fRefUs = 0;
    fRefSeq = 0;
    fLastValid = false;
    fLastStartUs = 0;
    fLastSeq = 0;
    fAWStarts = 0;
    fSyncUpdates = 0;
    fLastSyncErrUs = 0;
    memset(&fSyncMaster, 0, sizeof(fSyncMaster));
    fSyncMode = 0;
    fPresenceMode = 0;
    memset(fChanSeq, 0, sizeof(fChanSeq));
    fChanSeqValid = false;
    fInAW = false;
    fTsfOffset = 0;
    fTsfValid = false;
    fSyncOutliers = 0;
    fSyncOutliersTotal = 0;
    fListenSlot = 0;
    fListenStarted = 0;
    fListenBusy = 0;
    fListenSkippedHome = 0;
    fListenDwellTU = kListenDwellTU;
    fListenEveryNth = kListenEveryNthSlot;
    fListenTimer = nullptr;
    fListenAlign = 1;
    fListenAlignedActive = false;
    fListenNextChan = 0;
    fListenNextDwellTU = 0;
    fListenAligned = 0;
    fListenNoSlot = 0;
    fPeerValid = false;
    fPeerSeenUs = 0;
    fPeerRefUs = 0;
    fPeerRefSeq = 0;
    fPeerPm = 0;
    fPeerSlotUs = 0;
    fPeerChanValid = false;
    fPeerUpdates = 0;
    fPeerPredHit = 0;
    fPeerPredMiss = 0;
    fPeerLastErrUs = 0;
    fRxAFChan6 = 0;
    fRxAFChan44 = 0;
    fRxAFChan149 = 0;
    fRxAFChanOther = 0;
    fRxAFChan161 = 0;
    memset(fListenOnChan, 0, sizeof(fListenOnChan));
    fSyncTmplLen = 0;
    fSyncTmplSets = 0;
    fMITmplLen = 0;
    fOOBSets = 0;
    fOOBAutoSets = 0;
    fTxPsfMasterMode3 = 0;
    fAfTxMode = 0;
    fTxTimer = nullptr;
    fTxChan = 0;
    fTxPhase = 0;
    fTxSlotUs = 0;
    fTxSeq = 0;
    fMasterChannel = 0;
    fSecondaryMasterChannel = 0;
    fElectionParamsSet = false;
    memset(fElectionParams, 0, sizeof(fElectionParams));
    fTxPSF = 0;
    fTxMI = 0;
    fTxRefused = 0;
    fTxSkipMode = 0;
    fTxLastErr = 0;
    fTxLastLen = 0;
    fDataRx = 0;
    fDataRxBad = 0;
    fDataRxAmsdu = 0;
    fDataRxNull = fDataRxHtc = fDataRxFrag = fDataRxAmsduSub = 0;
    fAwGateHeld = fAwGateSent = 0;
    fDataRxUcUs = 0;
    fDataTxUcUs = 0;
    bzero(fPeerTl, sizeof(fPeerTl));
    fPeerTlEvict = fGateSkips = fGateUnknownDst = fGateDstAway = fGateDstAw = 0;
    fGateMcCached = fGateMcTracked = fPeerSwitchCached = fFollowUntrusted = 0;
    fListenIdle = fListenFull = 0;
    fStatsTx = fStatsRx = fStatsReports = 0;
    fStatsLastRet = 0;
    fListenNextLeadUs = 0;
    fDataRxBadLen = 0;
    fDataRxBadHeadLen = 0;
    fDataRxNoBuf = 0;
    fDataRxLastLen = 0;
    fDataRxLastType = 0;
    memset(fDataQ, 0, sizeof(fDataQ));
    fDataQHead = 0;
    fDataQCount = 0;
    fDataSeq = 0;
    fDataTx = 0;
    fDataTxQueued = 0;
    fDataTxDropFull = 0;
    fDataTxDeferred = 0;
    fDataTxErr = 0;
    fListenOwn = 0;
    fListenPeerSlots = 0;
    memset(fRxGap, 0, sizeof(fRxGap));
    fBssidSets = 0;
    fPeerCacheOps = 0;
    fDataTxNotCommon = 0;
    // TX behaviour switches: all default to the previous behaviour.
    {
        int v = 0;
        fTxPump = PE_parse_boot_argn("-itlawdltxpump", &v, sizeof(v));
        fHomeTx = PE_parse_boot_argn("-itlawdlhometx", &v, sizeof(v));
        uint32_t u = 0;
        fPumpUs = PE_parse_boot_argn("itlawdlpumpus", &u, sizeof(u)) && u >= 250 && u <= 10000 ? u : 1000;
        u = 0;
        fListenLeadUs = PE_parse_boot_argn("itlawdllead", &u, sizeof(u)) && u >= 1000 && u <= 60000 ?
            (int64_t)u : kListenLeadDefaultUs;
        u = 0;
        fDwellExtraTU = PE_parse_boot_argn("itlawdldwellx", &u, sizeof(u)) && u <= 200 ? u : 0;
    }
    {
        int v = 0;
        fFollowPeer = PE_parse_boot_argn("-itlawdlfollow", &v, sizeof(v));
        // On by default; -itlawdlnoawgate restores "send as soon as the dwell runs".
        fAwGate = !PE_parse_boot_argn("-itlawdlnoawgate", &v, sizeof(v));
        fTrustRssi = -60;
        {
            uint32_t mag = 0;
            if (PE_parse_boot_argn("itlawdltrustrssi", &mag, sizeof(mag)) && mag >= 30 && mag <= 95)
                fTrustRssi = -(int)mag;
        }
        // Off by default (opt in with -itlawdlidledwell or "awdlctl idledwell on"): it halves
        // the gateway latency while AWDL idles, but with the dwell reduced to the AW the
        // firmware's late dwell starts (10-40 ms late in ~20 % of dwells) and the peer
        // timeline error (up to ~9 ms) make us miss most of what an idle peer sends:
        // discovery went back to minutes (2026-11-12, build 54617AA1).
        fIdleDwell = PE_parse_boot_argn("-itlawdlidledwell", &v, sizeof(v));
        // On by default; -itlawdlnostats keeps the peer manager blind to our traffic.
        fDataStats = !PE_parse_boot_argn("-itlawdlnostats", &v, sizeof(v));
    }
    memset(&fPeerMaster, 0, sizeof(fPeerMaster));
    memset(&fPeerMac, 0, sizeof(fPeerMac));
    fPeerOwnErrUs = 0;
    fPeerSlotOff = 0;
    fFollowStreak = 0;
    fPeerAlignedCnt = fPeerMisalignedCnt = fFollowAdopts = fPeerFollowsUsMis = 0;
    fPeerGateStale = fPeerSwitchData = 0;
    memset(fDataQMc, 0, sizeof(fDataQMc));
    fMcQueued = fMcTx = 0;
    memset(fMcLatHist, 0, sizeof(fMcLatHist));
    fGateHeldTicks = fGateHeldMcTicks = fDataTxDropStale = 0;
    fDataRxMc = 0;
    memset(&fDataPeerMac, 0, sizeof(fDataPeerMac));
    fDataPeerUs = 0;
    memset(&fCachedPeerMac, 0, sizeof(fCachedPeerMac));
    fCachedPeerUs = 0;
    fPeerCacheReq = 0;
    fPeerCacheDataOn = 0;
    fPeerCacheIfEnabled = 0;
    fPeerCacheDoneHead = fPeerCacheDoneCount = 0;
    fPeerCacheCompleted = 0;
    fPeerActivityStamps = 0;
    fTxReqUs = 0;
    fTxPumpEndUs = 0;
    fTxPumpPsfDone = fTxPumpMiDone = fTxPumpSawRun = false;
    fTxStartNoted = true;
    fPumpTicks = fPumpWindows = fPumpNoRun = fPumpPeerAway = fPumpImmediate = fPumpLateAF = 0;
    memset(fRocStartHist, 0, sizeof(fRocStartHist));
    memset(fDataLatHist, 0, sizeof(fDataLatHist));
    fDataLatMaxUs = 0;
    memset(fDataQUs, 0, sizeof(fDataQUs));
    AWDL_LOG("role %u prefix %s mac %s", role, prefix, ether_sprintf(mac->octet));
    AWDL_LOG("tx pump %d (%u us) home tx %d lead %lld us dwell +%u TU follow %d", fTxPump, fPumpUs, fHomeTx,
             fListenLeadUs, fDwellExtraTU, fFollowPeer);
    return true;
}

void AirportItlwmAWDLInterface::
free()
{
    AWDL_LOG("calls %llu", fCallCount);
    teardownAW();
    IO80211AWDLProtocol::free();
}

IOReturn AirportItlwmAWDLInterface::
setMacAddress(ether_addr &mac)
{
    memcpy(&fMac, &mac, sizeof(fMac));
    AWDL_LOG("%s", ether_sprintf(mac.octet));
    publishAWDLAddress();
    return kIOReturnSuccess;
}

// Give the radio awdl0's address (P2P device MAC, unicast rx accounting).
void AirportItlwmAWDLInterface::
publishAWDLAddress()
{
    if (!awTxAllowed() || !fController)
        return;
    ieee80211com *ic = halIc();
    if (!ic)
        return;
    if (ic->ic_awdl_addr_gen && !memcmp(ic->ic_awdl_addr, &fMac, sizeof(fMac)))
        return;
    memcpy(ic->ic_awdl_addr, &fMac, sizeof(fMac));
    ic->ic_awdl_addr_gen++;
}

#pragma mark - getters with cached state

IOReturn AirportItlwmAWDLInterface::
getSSID(apple80211_ssid_data *d)
{
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getCHANNEL(apple80211_channel_data *d)
{
    // Social channel 6 until channel sequencing is implemented.
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->channel.version = APPLE80211_VERSION;
    d->channel.channel = 6;
    d->channel.flags = APPLE80211_C_FLAG_2GHZ | APPLE80211_C_FLAG_20MHZ;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getBSSID(apple80211_bssid_data *d)
{
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    memcpy(&d->bssid, &fMac, sizeof(fMac));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getSTATE(apple80211_state_data *d)
{
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->state = APPLE80211_S_INIT;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *d)
{
    memcpy(d, &fSyncParams, sizeof(*d));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *d)
{
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->enabled = fSyncEnabled;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNC_STATE(apple80211_awdl_sync_state *d)
{
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->state = fSyncState;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size *d)
{
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->max_peers = 32;
    return kIOReturnSuccess;
}

#pragma mark - setters with cached state

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *d)
{
    fCallCount++;
    memcpy(&fSyncParams, d, sizeof(fSyncParams));
    AWDL_LOG("aw_len %u aw_period %u ext_len %u sync_frame_period %u",
             d->availability_window_length, d->availability_window_period,
             d->extension_length, d->synchronization_frame_period);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *d)
{
    fCallCount++;
    fSyncEnabled = d->enabled != 0;
    AWDL_LOG("enabled %u", d->enabled);
    if (!awTimelineAllowed())
        return kIOReturnSuccess;
    setProperty("AWDLSyncEnabled", fSyncEnabled);
    if (fSyncEnabled)
        startAWTimeline();
    else
        stopAWTimeline();
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_STATE(apple80211_awdl_sync_state *d)
{
    fCallCount++;
    // IO80211AWDLPeerManager::programChipWithNewMaster: +4 mode, +8 the master
    // to follow (our own address when we are master), +16 op mode, +20 flags.
    const uint8_t *raw = (const uint8_t *)d;
    fSyncState = d->state;
    AWDL_LOG("state %u", d->state);
    if (!awTimelineAllowed())
        return kIOReturnSuccess;
    memcpy(&fSyncMaster, raw + 8, sizeof(fSyncMaster));
    fSyncMode = *(const uint32_t *)(raw + 16);
    AWDL_LOG("state %u master %s mode %u flags 0x%x", d->state, ether_sprintf(fSyncMaster.octet),
             fSyncMode, raw[20]);
    publishSyncState();
    return kIOReturnSuccess;
}

// +4 BSSID (Broadcom stores it at ivars +608 and creates its firmware AWDL
// interface with it, AppleBCMWLANProximityInterface::setAWDL_BSSID); all zero
// tears the interface down. +10: a second address, logged only.
IOReturn AirportItlwmAWDLInterface::
setAWDL_BSSID(apple80211_awdl_bssid *d)
{
    fCallCount++;
    AWDL_LOG("bssid %s", ether_sprintf(d->bssid));
    if (!awTimelineAllowed())
        return kIOReturnSuccess;
    char b[48];
    snprintf(b, sizeof(b), "%s", ether_sprintf(d->bssid));
    size_t n = strlen(b);
    snprintf(b + n, sizeof(b) - n, " %s", ether_sprintf(d->unk_mac));
    setProperty("AWDLBssid", b);
    fBssidSets++;
    setProperty("AWDLBssidSets", fBssidSets, 64);
    if (!awDataAllowed() || !fController)
        return kIOReturnSuccess;
    ieee80211com *ic = halIc();
    if (!ic)
        return kIOReturnSuccess;
    static const uint8_t zero[6] = {};
    bool valid = memcmp(d->bssid, zero, 6) != 0;
    if (valid == (ic->ic_awdl_bssid_valid != 0) && (!valid || !memcmp(ic->ic_awdl_bssid, d->bssid, 6)))
        return kIOReturnSuccess;
    memcpy(ic->ic_awdl_bssid, d->bssid, 6);
    ic->ic_awdl_bssid_valid = valid;
    ic->ic_awdl_addr_gen++;     // the P2P MAC is re-programmed before the next dwell
    return kIOReturnSuccess;
}

// Broadcom: awdl_peer_op puts the peer (address, HT/VHT/HE caps, its channel
// sequence) into the firmware peer cache before unicast. Layout as read by
// AppleBCMWLANProximityInterface::setPEER_CACHE_CONTROL: +4 opcode, +8 MAC,
// +16 HT IE (45, 26, ...), +44 ampdu, +45 VHT present, +46 VHT IE, +60 HE,
// +68.. channel sequence. Recorded only for now.
IOReturn AirportItlwmAWDLInterface::
setPEER_CACHE_CONTROL(apple80211_peer_cache_control *d)
{
    fCallCount++;
    if (!awTimelineAllowed() || !d)
        return kIOReturnSuccess;
    const uint8_t *r = (const uint8_t *)d;
    uint32_t op = *(const uint32_t *)(r + 4);
    fPeerCacheOps++;
    char s[96];
    snprintf(s, sizeof(s), "op %u %s ht %u/%u vht %u he %u", op, ether_sprintf((uint8_t *)r + 8),
             r[16], r[17], r[45], r[60]);
    setProperty("AWDLPeerCacheLast", s);
    setProperty("AWDLPeerCacheOps", fPeerCacheOps, 64);
    // Rates for unicast data to this peer (iwx_awdl_tx_frame): what it announced in the
    // HT element (+16: id 45, length 26, capabilities, A-MPDU parameters, MCS set from +21)
    // and the VHT element (+45 present, +46: id, length, capabilities, Rx MCS map at +52).
    if (op == 0 || op == 6) {
        ieee80211com *pic = halIc();
        if (pic) {
            uint8_t caps = 0;
            if (r[16] == 45 && r[17] >= 26) {
                caps |= IEEE80211_AWDL_PEER_HT;
                if (r[22])
                    caps |= IEEE80211_AWDL_PEER_HT2;
                if (r[45]) {
                    uint16_t map = (uint16_t)(r[52] | (r[53] << 8));
                    caps |= IEEE80211_AWDL_PEER_VHT;
                    if (((map >> 2) & 3) != 3) {
                        caps |= IEEE80211_AWDL_PEER_VHT2;
                        if (((map >> 2) & 3) >= 1)
                            caps |= IEEE80211_AWDL_PEER_VHT_M8;
                    }
                }
            }
            memcpy(pic->ic_awdl_peer_mac, r + 8, IEEE80211_ADDR_LEN);
            pic->ic_awdl_peer_caps = caps;
        }
    }
    if (op == 0 || op == 6 || op == 2) {
        uint64_t now = hostUs();
        if (PeerTl *e = peerTlFind(r + 8, op != 2, now))
            e->cached = op != 2;
    }
    // The family counts the peer as cached (or free again) only when the driver reports the
    // operation done; see completePeerCacheOps(). Queue it for the next slot tick.
    if ((op == 0 || op == 6 || op == 2) && fPeerCacheDoneCount < kPeerCacheDoneMax) {
        unsigned slot = (fPeerCacheDoneHead + fPeerCacheDoneCount) % kPeerCacheDoneMax;
        fPeerCacheDone[slot].type = op == 2 ? 3 : 1;
        memcpy(fPeerCacheDone[slot].mac, r + 8, 6);
        memcpy(&fPeerCacheDone[slot].tail, r + 14, 2);
        fPeerCacheDoneCount++;
    }
    return kIOReturnSuccess;
}

// Completion of setPEER_CACHE_CONTROL, as AppleBCMWLANCore::completePeerCacheControl posts
// it when the firmware has done the operation: message 0x33, 0x1d0 bytes, +4 = 1 (peer added,
// opcodes 0 and 6) or 3 (peer deleted, opcode 2), +8 the peer address, +0xe copied from the
// request. Without it the family's cache count only grew: after eight peers "No space to cache
// peer ... CPC 8 max 8", cached:0(8), and AWDL went back to the idle rule (B3072F85/DD071138,
// 2026-11-13: Mac -> iPhone stuck in "waiting").
void AirportItlwmAWDLInterface::
completePeerCacheOps()
{
#if !defined(AIRPORT_AWDL_ABI_TAHOE)
    // Sequoia: the vtable slot, message format or stats layout used here were verified
    // on Tahoe 25G332 only (IO80211VirtualInterface::findPeer is at 0xcc0 on 15.0.1, not
    // 0xcd0). Off until checked against a 15.x kernel collection.
    return;
#endif
    static uint8_t msg[0x1d0];              // work loop only
    while (fPeerCacheDoneCount) {
        const PeerCacheDone &d = fPeerCacheDone[fPeerCacheDoneHead];
        bzero(msg, sizeof(msg));
        memcpy(msg + 4, &d.type, 4);
        memcpy(msg + 8, d.mac, 6);
        memcpy(msg + 14, &d.tail, 2);
        postMessage(0x33, msg, sizeof(msg), true);
        fPeerCacheDoneHead = (fPeerCacheDoneHead + 1) % kPeerCacheDoneMax;
        fPeerCacheDoneCount--;
        fPeerCacheCompleted++;
    }
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *d)
{
    fCallCount++;
    fPresenceMode = d->mode;
    AWDL_LOG("mode %u", d->mode);
    return kIOReturnSuccess;
}

#pragma mark - stage 3: what the system asks us to transmit (recorded only)

// Payload pointers come from IO80211AWDLPeerManager's own buffers
// (updateSyncFrameTemplate: 512 bytes on its stack; buildMasterIndicationTemplate:
// 0x4b0 bytes in its ivars). Only copy from kernel addresses.
static bool awdlKernelPtr(const void *p)
{
    return (uintptr_t)p >= 0xffffff8000000000ULL;
}

// "type:len type:len ..." of a TLV buffer, to check which TLVs the system
// supplies and which the firmware is expected to add.
static void awdlTLVList(const uint8_t *b, size_t len, char *out, size_t outLen)
{
    size_t off = 0, n = 0;
    out[0] = 0;
    while (off + 3 <= len && n + 12 < outLen) {
        uint16_t tlen = b[off + 1] | (b[off + 2] << 8);
        n += snprintf(out + n, outLen - n, "%s%u:%u", n ? " " : "", b[off], tlen);
        off += 3 + tlen;
    }
    if (off != len && n + 12 < outLen)
        snprintf(out + n, outLen - n, " !%zu/%zu", off, len);
}

static void awdlHex(const uint8_t *b, size_t len, char *out, size_t outLen)
{
    size_t n = 0;
    out[0] = 0;
    for (size_t i = 0; i < len && n + 3 < outLen; i++)
        n += snprintf(out + n, outLen - n, "%02x", b[i]);
}

// ioctl 151, from IO80211AWDLPeerManager::updateSyncFrameTemplate: +4 length,
// +8 pointer. TLVs only (service params, election v2, data path state, ...);
// Broadcom hands it to the firmware ("awdl_payload"), which adds the AWDL
// header, the sync parameters and the channel sequence when it sends a PSF.
IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *d)
{
    fCallCount++;
    const uint8_t *raw = (const uint8_t *)d;
    uint32_t len = *(const uint32_t *)(raw + 4);
    const uint8_t *p = *(const uint8_t * const *)(raw + 8);
    AWDL_LOG("version %u len %u", d->version, len);
    if (!awTimelineAllowed() || !len || len > sizeof(fSyncTmpl) || !awdlKernelPtr(p))
        return kIOReturnSuccess;
    memcpy(fSyncTmpl, p, len);
    fSyncTmplLen = (uint16_t)len;
    fSyncTmplSets++;
    char tlvs[160];
    awdlTLVList(fSyncTmpl, len, tlvs, sizeof(tlvs));
    setProperty("AWDLTxSyncTmplLen", len, 32);
    setProperty("AWDLTxSyncTmplSets", fSyncTmplSets, 64);
    setProperty("AWDLTxSyncTmplTLVs", tlvs);
    return kIOReturnSuccess;
}

// ioctl 149 (send now; also IO80211AWDLPeerManager::sendUnicastMI) and 225
// (auto, from updatePrimaryPayloadMI). Layout from those builders and from
// AppleBCMWLANProximityInterface::setAWDL_OOB_(AUTO_)REQUEST:
// +4 destination MAC (ff.. for broadcast MI), +12 tx offset in the AW (us,
// auto), +16 MI bitmap (auto), +20 u16 (6, or 14 for UMI on the dwell
// channel), +22 6 bytes from PeerManager ivars (inferred: our address or the
// AWDL BSSID), +40 payload length, +48 payload pointer. The payload is TLVs
// (election v2, data path state, AirDrop, arpa, service params/responses).
void AirportItlwmAWDLInterface::
recordOOB(const void *req, bool autoReq)
{
    const uint8_t *raw = (const uint8_t *)req;
    uint16_t len = *(const uint16_t *)(raw + 40);
    const uint8_t *p = *(const uint8_t * const *)(raw + 48);
    char hdr[48 * 2 + 1];
    awdlHex(raw, 48, hdr, sizeof(hdr));
    setProperty(autoReq ? "AWDLTxOOBAutoHdr" : "AWDLTxOOBHdr", hdr);
    if (autoReq) {
        fOOBAutoSets++;
        setProperty("AWDLTxOOBAutoSets", fOOBAutoSets, 64);
        setProperty("AWDLTxMIBitmap", *(const uint32_t *)(raw + 16), 32);
        setProperty("AWDLTxMIOffsetUs", *(const uint32_t *)(raw + 12), 32);
    } else {
        fOOBSets++;
        setProperty("AWDLTxOOBSets", fOOBSets, 64);
    }
    if (!len || len > sizeof(fMITmpl) || !awdlKernelPtr(p))
        return;
    char tlvs[200];
    awdlTLVList(p, len, tlvs, sizeof(tlvs));
    setProperty(autoReq ? "AWDLTxOOBAutoTLVs" : "AWDLTxOOBTLVs", tlvs);
    setProperty(autoReq ? "AWDLTxOOBAutoLen" : "AWDLTxOOBLen", len, 32);
    if (autoReq) {
        memcpy(fMITmpl, p, len);
        fMITmplLen = len;
    }
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *d)
{
    fCallCount++;
    if (awTimelineAllowed())
        recordOOB(d, false);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *d)
{
    fCallCount++;
    if (awTimelineAllowed())
        recordOOB(d, true);
    return kIOReturnSuccess;
}

// ioctl 208, IO80211AWDLPeerManager::setAFTXMode: +4 mode (3 = suppress, e.g.
// infra on 2.4 GHz; 0 = allow). Broadcom: iovar awdl_aftxmode.
// +4 channel number (AppleBCMWLANProximityInterface::setAWDL_MASTER_CHANNEL).
IOReturn AirportItlwmAWDLInterface::
setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *d)
{
    fCallCount++;
    fMasterChannel = *(const uint32_t *)((const uint8_t *)d + 4);
    AWDL_LOG("channel %u", fMasterChannel);
    if (awTimelineAllowed())
        setProperty("AWDLMasterChannel", fMasterChannel, 32);
    return kIOReturnSuccess;
}

// APPLE80211_IOC_AWDL_ELECTION_PARAMS, 0x38 bytes. Fields used by
// AppleBCMWLANProximityInterface (firmware "election tree"): +0x10 flags (8 bit),
// +0x12 election ID (16), +0x14 distance to the top master (8), +0x16 top master address (6),
// +0x1c top master metric (32), +0x20 self metric (32). The election itself is run by the
// family on this driver, so the values it sets are kept and handed back unchanged.
IOReturn AirportItlwmAWDLInterface::
setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *d)
{
    fCallCount++;
    if (!d)
        return kIOReturnBadArgument;
    memcpy(fElectionParams, d, sizeof(fElectionParams));
    fElectionParamsSet = true;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *d)
{
    if (!d || !fElectionParamsSet)
        return kIOReturnUnsupported;
    memcpy(d, fElectionParams, sizeof(fElectionParams));
    return kIOReturnSuccess;
}

// The family (and wdutil's "Master Channel") reads back what it configured.
IOReturn AirportItlwmAWDLInterface::
getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *d)
{
    if (!d || !fMasterChannel)
        return kIOReturnUnsupported;
    d->version = APPLE80211_VERSION;
    d->master_channel = fMasterChannel;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *d)
{
    fCallCount++;
    fSecondaryMasterChannel = d ? d->secondary_master_channel : 0;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *d)
{
    if (!d || !fSecondaryMasterChannel)
        return kIOReturnUnsupported;
    d->version = APPLE80211_VERSION;
    d->secondary_master_channel = fSecondaryMasterChannel;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *d)
{
    fCallCount++;
    fAfTxMode = *(const uint32_t *)((const uint8_t *)d + 4);
    AWDL_LOG("mode %u", fAfTxMode);
    if (awTimelineAllowed())
        setProperty("AWDLTxAfMode", fAfTxMode, 32);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *d)
{
    fCallCount++;
    if (!awTimelineAllowed())
        return kIOReturnSuccess;
    const uint8_t *raw = (const uint8_t *)d;
    if (raw[kChanSeqCount] < 1 || raw[kChanSeqCount] > kChanSeqMax)
        return kIOReturnBadArgument;
    memcpy(fChanSeq, raw, sizeof(fChanSeq));
    fChanSeqValid = true;
    char seq[kChanSeqMax * 4 + 1] = {};
    for (unsigned i = 0, n = 0; i < raw[kChanSeqCount] && n + 4 < sizeof(seq); i++) {
        const apple80211_channel *c = (const apple80211_channel *)(raw + kChanSeqEntries) + i;
        n += snprintf(seq + n, sizeof(seq) - n, " %u", c->channel);
    }
    AWDL_LOG("count %u enc %u step %u:%s", raw[kChanSeqCount], raw[6], raw[kChanSeqStep], seq);
    setProperty("AWDLChanSeq", seq);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "enc %u step %u dup %u fill %u", raw[6], raw[kChanSeqStep], raw[8], raw[9]);
    setProperty("AWDLChanSeqHdr", hdr);
    if (awDataAllowed() && awListenAllowed() && fAWRunning)
        scheduleAlignedListen(hostUs());
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *d)
{
    if (!fChanSeqValid)
        return kIOReturnUnsupported;
    memcpy(d, fChanSeq, sizeof(fChanSeq));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *d)
{
    if (!awTimelineAllowed())
        return kIOReturnUnsupported;
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->mode = presenceMode();
    return kIOReturnSuccess;
}

#pragma mark - getters without a known layout

AWDL_GET_STUB(AWDL_EXTENSION_STATE_MACHINE_PARAMETERS, apple80211_awdl_extension_state_machine_parameter)
AWDL_GET_STUB(AWDL_ELECTION_ID, apple80211_awdl_election_id)
AWDL_GET_STUB(AWDL_ELECTION_METRIC, apple80211_awdl_election_metric)
AWDL_GET_STUB(AWDL_ELECTION_RSSI_THRESHOLDS, apple80211_awdl_election_rssi_thresholds)
AWDL_GET_STUB(AWDL_DEVICE_CAPABILITIES, apple80211_awdl_device_capabilities)
AWDL_GET_STUB(AWDL_AF_TX_MODE, apple80211_awdl_af_tx_mode)
AWDL_GET_STUB(AWDL_MIN_RATE, apple80211_awdl_min_rate)
AWDL_GET_STUB(AWDL_ENCRYPTION_TYPE, apple80211_encrypt)
AWDL_GET_STUB(AWDL_TXCAL_PERIOD, apple80211_awdl_txcal_period)
AWDL_GET_STUB(AWDL_DEV_STATS, apple80211_awdl_dev_stats)
AWDL_GET_STUB(AWDL_DFSP_CONFIG, apple80211_awdl_dfsp_config)
AWDL_GET_STUB(AWDL_CCA, apple80211_awdl_cca)
AWDL_GET_STUB(AWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES, apple80211_awdl_cap)
AWDL_GET_STUB(LLW_PARAMS, apple80211_llw_params)

#pragma mark - setters accepted but not acted on yet

AWDL_SET_STUB(SSID, apple80211_ssid_data)
AWDL_SET_STUB(CHANNEL, apple80211_channel_data)
AWDL_SET_STUB(AWDL_EXTENSION_STATE_MACHINE_PARAMETERS, apple80211_awdl_extension_state_machine_parameter)
AWDL_SET_STUB(AWDL_ELECTION_ID, apple80211_awdl_election_id)
AWDL_SET_STUB(AWDL_ELECTION_METRIC, apple80211_awdl_election_metric)
AWDL_SET_STUB(AWDL_ELECTION_RSSI_THRESHOLDS, apple80211_awdl_election_rssi_thresholds)
AWDL_SET_STUB(AWDL_PEER_TRAFFIC_REGISTRATION, apple80211_awdl_peer_traffic_registration)
AWDL_SET_STUB(AWDL_MIN_RATE, apple80211_awdl_min_rate)
AWDL_SET_STUB(AWDL_ENCRYPTION_KEYS, apple80211_key)
AWDL_SET_STUB(AWDL_ENCRYPTION_TYPE, apple80211_encrypt)
AWDL_SET_STUB(AWDL_TXCAL_PERIOD, apple80211_awdl_txcal_period)
AWDL_SET_STUB(AWDL_AWDL_ADVERTISERS, awdl_peer_advert_add)
AWDL_SET_STUB(AWDL_DFSP_CONFIG, apple80211_awdl_dfsp_config)
AWDL_SET_STUB(AWDL_DFSP_UCSA_CONFIG, apple80211_awdl_dfsp_ucsa_config)
AWDL_SET_STUB(COMPANION_SKYWALK_IF_CREATE, apple80211_companion_skywalk_if_create)
AWDL_SET_STUB(AWDL_OOB_SEC_PAYLOAD_REQUEST, apple80211_awdl_af_sec_payload_request)
AWDL_SET_STUB(LLW_PARAMS, apple80211_llw_params)
AWDL_SET_STUB(TX_RATE, apple80211_tx_rate)
AWDL_SET_STUB(P2P_COEX_PROTECTION, apple80211_p2p_coex_protection)
AWDL_SET_STUB(PSF_DWELL, apple80211_awdl_psf_dwell_params)
AWDL_SET_STUB(AWDL_CONFIG, apple80211_awdl_config_request)
AWDL_SET_STUB(AWDL_RTG_PEER_STATS_CONFIG, apple80211_rtg_peer_stats_cfg)

#endif /* AIRPORT_AWDL */
