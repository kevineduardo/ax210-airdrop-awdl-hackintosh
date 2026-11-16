// Added by the ax210-airdrop-awdl-hackintosh project, 2026-11 (AWDL/AirDrop port to
// macOS 15 Sequoia and related fixes). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  AirportItlwmAWDLBuildGuard.hpp
//  AirportItlwm
//
//  Ported from ForbiddenItlwm's AWDLBuildGuard by retropineaple, GPL-2.0.
//
//  Which macOS kernels AWDL (awdl0, AirDrop) may start on. AirportItlwmAWDLInterface
//  subclasses Apple's IO80211AWDLProtocol slot for slot and relies on IO80211Family data
//  layouts, so AWDL only starts on a kernel where that ABI was verified. On any other kernel
//  the driver behaves as if -itlawdl was not set: normal Wi-Fi works and nothing AWDL-related
//  starts. -itlawdlanybuild skips this kernel list (not the vtable check in
//  AirportItlwmAWDLBuildGuard.cpp).
//
//  This part is header-only and freestanding (no kernel or libc headers), so the host unit
//  test tests/unit/test_awdl_build_guard.cpp compiles it as is, once per ABI.
//
//  The match is on the xnu release tag inside the kernel `version` string, for example
//  "Darwin Kernel Version 24.6.0: ...; root:xnu-11417.140.69.712.69~1/RELEASE_X86_64".
//  DEVELOPMENT and other kernel variants never match.
//

#ifndef AirportItlwmAWDLBuildGuard_hpp
#define AirportItlwmAWDLBuildGuard_hpp

#include "AirportItlwmAWDLConfig.h"

#if !defined(AIRPORT_AWDL_ABI_SEQUOIA) && !defined(AIRPORT_AWDL_ABI_TAHOE)
#error "AirportItlwmAWDLBuildGuard.hpp needs AIRPORT_AWDL_ABI_SEQUOIA or AIRPORT_AWDL_ABI_TAHOE"
#endif

namespace AirportAWDLGuard {

struct VerifiedKernel {
    const char *kernel;         // xnu tag, matched as "<kernel>/RELEASE_X86_64"
    const char *const *builds;  // kern.osversion values known to run this kernel, null-terminated
};

#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
// macOS 15.8.1 (24H32): our hardware-tested build. The kernel tag comes from our own
// panic log and `uname -v` on the test machine.
constexpr const char *kSequoiaBuilds[] = {"24H32", nullptr};
constexpr VerifiedKernel kVerifiedKernels[] = {
    {"xnu-11417.140.69.712.69~1", kSequoiaBuilds},
};
#else
// macOS 26.7.2 (25G332) and 26.7.3 (25G342) share this kernel. The 25G332 tag comes from
// ForbiddenItlwm's test data; confirmation with `uname -v` on our 25G332 test machine is
// still pending.
constexpr const char *kTahoeBuilds[] = {"25G332", "25G342", nullptr};
constexpr VerifiedKernel kVerifiedKernels[] = {
    {"xnu-12377.161.15.701.48~2", kTahoeBuilds},
};
#endif

constexpr const char *kReleaseSuffix = "/RELEASE_X86_64";

enum class BuildCheck { Verified, UnknownKernel, UnknownBuild };

inline bool startsWith(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return false;
    return true;
}

inline bool sameText(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// True if `kernelVersion` contains "<tag>/RELEASE_X86_64" with no other character after the
// tag's "~N": a longer tag such as "...~12" does not match "...~1".
inline bool hasReleaseTag(const char *kernelVersion, const char *tag)
{
    if (!kernelVersion || !tag || !*tag)
        return false;
    for (const char *p = kernelVersion; *p; ++p) {
        if (!startsWith(p, tag))
            continue;
        const char *q = p;
        for (const char *t = tag; *t; ++t)
            ++q;
        if (startsWith(q, kReleaseSuffix))
            return true;
    }
    return false;
}

// kernelVersion: the kernel's `version` string (libkern/version.h), always set.
// osBuild: kern.osversion (for example "24H32"). It may be null or empty early in boot;
// then the kernel alone decides. When it is set and the kernel is known, it must be one of
// that kernel's builds.
inline BuildCheck checkBuild(const char *kernelVersion, const char *osBuild)
{
    for (const VerifiedKernel &k : kVerifiedKernels) {
        if (!hasReleaseTag(kernelVersion, k.kernel))
            continue;
        if (!osBuild || !*osBuild)
            return BuildCheck::Verified;
        for (const char *const *b = k.builds; *b; ++b)
            if (sameText(osBuild, *b))
                return BuildCheck::Verified;
        return BuildCheck::UnknownBuild;
    }
    return BuildCheck::UnknownKernel;
}

inline const char *buildCheckName(BuildCheck c)
{
    switch (c) {
        case BuildCheck::Verified: return "verified";
        case BuildCheck::UnknownKernel: return "unknown-kernel";
        case BuildCheck::UnknownBuild: return "unknown-build";
    }
    return "unknown";
}

} // namespace AirportAWDLGuard

// Run time (AirportItlwmAWDLBuildGuard.cpp, kext only). Both are computed once.
// airportAWDLBuildAllowed(): the vtable anchors match and the kernel is verified (or
//   -itlawdlanybuild is set).
// airportAWDLBuildStatus(): one line for the log and the AWDLBuildGuard IORegistry property.
// airportAWDLEnabled(): -itlawdl is set and airportAWDLBuildAllowed(). Every place that used
//   to read -itlawdl directly calls this instead, so a blocked build behaves exactly like a
//   boot without -itlawdl.
// airportAWDLCoexEnabled(): airportAWDLEnabled() and -itlawdlcoex. Selects the coex backend
//   (ForbiddenItlwm's listener engine) instead of the P2P-device listen path for this boot.
// airportAWDLBackendName(): "coex", "p2p", "scan" or "none", for the AWDLBackend property.
bool airportAWDLBuildAllowed();
const char *airportAWDLBuildStatus();
bool airportAWDLEnabled();
bool airportAWDLCoexEnabled();
const char *airportAWDLBackendName();

#endif /* AirportItlwmAWDLBuildGuard_hpp */
