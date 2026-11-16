#!/bin/sh
# SPDX-License-Identifier: MIT
# Test entry point. Every pull request must pass `tests/run.sh` (CI runs it too).
#
#   tests/run.sh            static + unit tests (fast, no build needed)
#   tests/run.sh static     static checks only
#   tests/run.sh unit       unit tests only
#   tests/run.sh kext DIR   post-build checks on every AirportItlwm.kext under DIR
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
# Resolve a DIR argument before changing directory.
if [ "${1:-}" = kext ] && [ -n "${2:-}" ] && [ -d "$2" ]; then kextdir=$(cd "$2" && pwd); else kextdir=${2:-}; fi
cd "$root" || exit 2
fail=0
run() { echo "--- $*"; "$@" || { echo "*** FAILED: $*"; fail=1; }; }
is_macos() { [ "$(uname -s)" = Darwin ]; }

static() {
    run python3 -I tests/static/check_boot_args.py
    run python3 -I tests/static/check_licenses.py
    if command -v shellcheck >/dev/null 2>&1; then
        run shellcheck -S warning tests/run.sh tools/check-kext.sh scripts/collect-diagnostics.sh \
            scripts/check-compat.sh tests/unit/test_check_compat.sh \
            scripts/boot-cycles.sh tools/airdrop/airdrop-measure.sh \
            tools/build/build-airportitlwm-clt.sh tools/build/build-bluetoolfixup-clt.sh
    else
        echo "skip shellcheck (not installed)"
    fi
    if command -v zsh >/dev/null 2>&1; then run zsh -n scripts/monitor-awdl.sh; fi
    # Swift tools: syntax check only (one file at a time: each is a top-level script).
    if command -v swiftc >/dev/null 2>&1 && swiftc --version >/dev/null 2>&1; then
        for f in tools/airdrop/*.swift; do run swiftc -parse "$f"; done
    else
        echo "skip swiftc -parse (swiftc not installed)"
    fi
    run perl -c tools/advsets/verify-advsets.pl
    if is_macos; then
        for p in driver/AirportItlwm/AirportItlwm/WCL/*.plist driver/AirportItlwm/itlwm.xcodeproj/project.pbxproj \
                 driver/BlueToolFixup/BrcmPatchRAM/BlueToolFixup-Info.plist driver/BlueToolFixup/BrcmPatchRAM.xcodeproj/project.pbxproj; do
            run plutil -lint "$p"
        done
    fi
}

unit() {
    run python3 -I -m unittest discover -s tests/unit -p 'test_*.py'
    # AWDL build guard allow-list: the header is freestanding, so this runs on any OS.
    if command -v c++ >/dev/null 2>&1; then
        tmp=$(mktemp -d) || exit 2
        for abi in SEQUOIA TAHOE; do
            run c++ -std=c++17 -Wall -Wextra -Werror -DAIRPORT_AWDL_ABI_$abi \
                -I driver/AirportItlwm/AirportItlwm -o "$tmp/test_awdl_build_guard_$abi" \
                tests/unit/test_awdl_build_guard.cpp
            [ -x "$tmp/test_awdl_build_guard_$abi" ] && run "$tmp/test_awdl_build_guard_$abi"
        done
        # AWDL coex backend: host tests ported from ForbiddenItlwm (GPL-2.0, see tests/unit/awdlcoex).
        for src in tests/unit/awdlcoex/*.cpp; do
            name=$(basename "$src" .cpp)
            run c++ -std=c++17 -Wall -Wextra -Werror -I driver/AirportItlwm/include/AWDLCoex \
                -I driver/AirportItlwm/AirportItlwm -o "$tmp/$name" "$src"
            [ -x "$tmp/$name" ] && run "$tmp/$name"
        done
        rm -rf "$tmp"
    else
        echo "skip AWDL build guard test (no c++ compiler)"
    fi
    run sh tests/unit/test_check_compat.sh
    if is_macos; then
        tmp=$(mktemp -d) || exit 2
        run cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -framework IOKit -framework CoreFoundation \
            -o "$tmp/test_awdlctl" tests/unit/test_awdlctl.c
        [ -x "$tmp/test_awdlctl" ] && run "$tmp/test_awdlctl"
        run make -s -C tools/awdlctl
        rm -rf "$tmp"
    else
        echo "skip C unit tests (need macOS for IOKit headers)"
    fi
}

kext() {
    dir=${kextdir:?usage: tests/run.sh kext <build products dir>}
    found=0
    list=$(mktemp) || exit 2
    find "$dir" -name AirportItlwm.kext -type d > "$list"
    while IFS= read -r k; do
        found=1
        run env NO_KC="${NO_KC:-1}" tools/check-kext.sh "$k"   # CI runner KC is not the target macOS
    done < "$list"
    rm -f "$list"
    [ $found -eq 1 ] || { echo "no AirportItlwm.kext under $dir"; fail=1; }
}

case ${1:-all} in
    all) static; unit ;;
    static) static ;;
    unit) unit ;;
    kext) kext ;;
    *) echo "usage: tests/run.sh [all|static|unit|kext DIR]"; exit 2 ;;
esac
[ $fail -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
