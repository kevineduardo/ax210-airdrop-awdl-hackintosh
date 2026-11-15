// SPDX-License-Identifier: MIT
// Host test for the AWDL build guard allow-list
// (driver/AirportItlwm/AirportItlwm/AirportItlwmAWDLBuildGuard.hpp, GPL-2.0, which is
// ported from ForbiddenItlwm's AWDLBuildGuard by retropineaple). tests/run.sh builds this
// file twice, once with -DAIRPORT_AWDL_ABI_SEQUOIA and once with -DAIRPORT_AWDL_ABI_TAHOE.
#include "AirportItlwmAWDLBuildGuard.hpp"
#include <cstdio>

using namespace AirportAWDLGuard;

static int failures = 0;

static const char *name(BuildCheck c) { return buildCheckName(c); }

static void expect(int line, const char *kernel, const char *build, BuildCheck want)
{
    const BuildCheck got = checkBuild(kernel, build);
    if (got != want) {
        std::printf("FAIL line %d: checkBuild(\"%s\", \"%s\") = %s, want %s\n", line,
                    kernel ? kernel : "(null)", build ? build : "(null)", name(got), name(want));
        ++failures;
    }
}
#define EXPECT(kernel, build, want) expect(__LINE__, kernel, build, BuildCheck::want)

int main()
{
#if defined(AIRPORT_AWDL_ABI_SEQUOIA)
    const char *abi = "Sequoia";
    const char *ok = "Darwin Kernel Version 24.6.0: root:xnu-11417.140.69.712.69~1/RELEASE_X86_64";
    const char *goodBuild = "24H32";
    const char *otherBuild = "24H241";
    const char *dev = "Darwin Kernel Version 24.6.0: root:xnu-11417.140.69.712.69~1/DEVELOPMENT_X86_64";
    const char *unknown = "Darwin Kernel Version 24.6.0: root:xnu-11417.140.69.713.47~2/RELEASE_X86_64";
    const char *longerTag = "Darwin Kernel Version 24.6.0: root:xnu-11417.140.69.712.69~12/RELEASE_X86_64";
    const char *otherOS = "Darwin Kernel Version 25.6.0: root:xnu-12377.161.15.701.48~2/RELEASE_X86_64";
    const char *otherOSBuild = "25G332";
#else
    const char *abi = "Tahoe";
    const char *ok = "Darwin Kernel Version 25.6.0: root:xnu-12377.161.15.701.48~2/RELEASE_X86_64";
    const char *goodBuild = "25G332";
    const char *otherBuild = "25G241";
    const char *dev = "Darwin Kernel Version 25.6.0: root:xnu-12377.161.15.701.48~2/DEVELOPMENT_X86_64";
    const char *unknown = "Darwin Kernel Version 25.6.0: root:xnu-12377.161.15.700.19~2/RELEASE_X86_64";
    const char *longerTag = "Darwin Kernel Version 25.6.0: root:xnu-12377.161.15.701.48~21/RELEASE_X86_64";
    const char *otherOS = "Darwin Kernel Version 24.6.0: root:xnu-11417.140.69.712.69~1/RELEASE_X86_64";
    const char *otherOSBuild = "24H32";
    EXPECT(ok, "25G342", Verified);   // 26.7.3 runs the same kernel
#endif
    // Verified kernel, with and without kern.osversion (empty early in boot).
    EXPECT(ok, goodBuild, Verified);
    EXPECT(ok, nullptr, Verified);
    EXPECT(ok, "", Verified);
    // Known kernel, but a build that is not on its list.
    EXPECT(ok, otherBuild, UnknownBuild);
    EXPECT(ok, otherOSBuild, UnknownBuild);
    EXPECT(ok, "24H3", UnknownBuild);       // prefix of a listed build
    // Development kernel of a verified build.
    EXPECT(dev, goodBuild, UnknownKernel);
    EXPECT(dev, nullptr, UnknownKernel);
    // Unknown kernel of the same macOS version, and a tag that only starts like ours.
    EXPECT(unknown, nullptr, UnknownKernel);
    EXPECT(unknown, goodBuild, UnknownKernel);
    EXPECT(longerTag, nullptr, UnknownKernel);
    // The verified kernel of the other macOS version, which this ABI does not cover.
    EXPECT(otherOS, otherOSBuild, UnknownKernel);
    EXPECT(otherOS, nullptr, UnknownKernel);
    // Tag without the release suffix, and missing strings.
    EXPECT("root:xnu-11417.140.69.712.69~1 root:xnu-12377.161.15.701.48~2", nullptr, UnknownKernel);
    EXPECT(nullptr, goodBuild, UnknownKernel);
    EXPECT(nullptr, nullptr, UnknownKernel);
    EXPECT("", nullptr, UnknownKernel);

    if (failures) {
        std::printf("AWDL build guard (%s): %d failure(s)\n", abi, failures);
        return 1;
    }
    std::printf("AWDL build guard (%s): all checks passed\n", abi);
    return 0;
}
