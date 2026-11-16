// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL coex backend, -itlawdlcoex).
// See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
// Ported from ForbiddenItlwm (retropineaple), commit a5f1eb9, include/AWDLSameChannelProbe.hpp, GPL-2.0.
// Verbatim apart from this header and quoted includes of the other AWDLCoex headers.
#pragma once
#include <stdint.h>
namespace ItlAWDL {
// An opt-in receive lease for the existing ADDBA probe, never a data-mode policy.
struct SameChannelProbe {
    static constexpr uint64_t maxWindowUS = 250000;
    bool enabled = false, applied = false, restorePending = false;
    uint32_t opens = 0, closes = 0, restoreFailures = 0;
    uint64_t sinceUS = 0, lastUS = 0, maxUS = 0;
    bool wants(unsigned own, unsigned station, bool receiveSession, bool asking,
               bool receiveFilterOff, uint64_t deadline, uint64_t now) const {
        return enabled && own && own == station && receiveSession && asking && !receiveFilterOff &&
            deadline > now && deadline - now <= maxWindowUS;
    }
    void noteApplied(bool live, uint64_t now) {
        if (live == applied) return;
        if (live) { ++opens; sinceUS = now; }
        else {
            ++closes;
            lastUS = now >= sinceUS ? now - sinceUS : 0;
            if (lastUS > maxUS) maxUS = lastUS;
            sinceUS = 0;
        }
        applied = live;
    }
};
}
