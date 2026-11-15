#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
# Build AirportItlwm.kext with the Command Line Tools only (no Xcode).
#
#   tools/build/build-airportitlwm-clt.sh [Platform] [Release|Debug]
#
# Platform is the suffix of an AirportItlwm-<Platform> target in
# driver/AirportItlwm/itlwm.xcodeproj (default Tahoe; e.g. Sequoia).
# Sources, preprocessor definitions and the deployment target come from the
# project (tools/build/pbxinfo.py); the compile/link flags are the ones Xcode
# adds for com.apple.product-type.kernel-extension. Every file is rebuilt.
#
# Needs driver/AirportItlwm/MacKernelSDK (see CONTRIBUTING.md).
# Output: driver/AirportItlwm/build/clt/<Configuration>/<Platform>/AirportItlwm.kext (+ .dSYM)
#
# Environment: JOBS (parallel compiles, default: CPU count), SDK_OVERRIDE (SDK path),
# GIT_COMMIT (version suffix), CHECK_IMPORTS=0|1 (import check in tools/check-kext.sh;
# default: only when Platform is the running macOS).
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo/driver/AirportItlwm"
PLATFORM=${1:-Tahoe}
CONF=${2:-Release}
case $CONF in Release|Debug) ;; *) echo "configuration must be Release or Debug" >&2; exit 2 ;; esac
PROJECT_DIR=$PWD
SDK=${SDK_OVERRIDE:-$(xcrun --sdk macosx --show-sdk-path)}
CC=$(xcrun -f clang)
OUT=build/clt/$CONF/$PLATFORM
OBJ=build/clt-obj/$CONF/$PLATFORM
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
[ -d MacKernelSDK/Headers ] || { echo "driver/AirportItlwm/MacKernelSDK missing (see CONTRIBUTING.md)" >&2; exit 1; }

pbx() { python3 -I "$repo/tools/build/pbxinfo.py" itlwm.xcodeproj "AirportItlwm-$PLATFORM" "$CONF" "$@"; }

VERSION=$(pbx --setting MODULE_VERSION)
DEPLOY=$(pbx --setting MACOSX_DEPLOYMENT_TARGET)
PLIST=$(pbx --setting INFOPLIST_FILE)
if [ -z "${GIT_COMMIT:-}" ]; then
    GIT_COMMIT=-awdl-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
    git diff --quiet HEAD -- . 2>/dev/null || GIT_COMMIT=$GIT_COMMIT-dirty
fi
# Read one array element per line (bash 3.2 has no mapfile).
DEFS=()
out=$(pbx --defines --var "MODULE_VERSION=$VERSION" --var "GIT_COMMIT=$GIT_COMMIT")
while IFS= read -r l; do [ -z "$l" ] || DEFS+=("$l"); done <<< "$out"
SOURCES=()
out=$(pbx --sources)
while IFS= read -r l; do [ -z "$l" ] || SOURCES+=("$l"); done <<< "$out"
[ ${#SOURCES[@]} -gt 0 ] || { echo "no sources for AirportItlwm-$PLATFORM" >&2; exit 1; }

if [ "$CONF" = Debug ]; then OPT=-O0; DBGDEF=(-DDEBUG=1); else OPT=-O1; DBGDEF=(); fi

# Firmware blob (the fw_gen target).
if [ ! -f include/FwBinary.cpp ]; then
    echo "== fw_gen"
    export PROJECT_DIR
    sh scripts/fw_gen.sh -P "$PROJECT_DIR/itlwm/firmware/"
fi

# Xcode header map: every project header is reachable by its bare name with "".
IQUOTE=()
while IFS= read -r dir; do
    IQUOTE+=(-iquote "$dir")
done < <(find AirportItlwm itlwm itl80211 include \( -name '*.h' -o -name '*.hpp' -o -name '*.pch' \) -exec dirname {} \; | sort -u)

COMMON=(
    -target "x86_64-apple-macos$DEPLOY" -isysroot "$SDK" -arch x86_64
    -fmessage-length=0 -fdiagnostics-show-note-include-stack -fmacro-backtrace-limit=0
    -Wno-trigraphs -fpascal-strings "$OPT" -fno-common
    -Wno-missing-field-initializers -Wno-missing-prototypes -Wno-return-type -Wno-non-virtual-dtor
    -Wno-overloaded-virtual -Wno-exit-time-destructors -Wno-unused-parameter -Wno-unknown-pragmas
    -Wno-shadow -Wno-four-char-constants -Wno-conversion -Wno-sign-conversion -Wno-newline-eof
    -Wno-deprecated-register -Wno-c++11-narrowing -Wno-incompatible-pointer-types
    -Wno-implicit-function-declaration -Wno-int-conversion -Wno-everything
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT "${DBGDEF[@]+"${DBGDEF[@]}"}" "${DEFS[@]+"${DEFS[@]}"}"
    -fasm-blocks -fstrict-aliasing -g
    -fno-builtin -fno-exceptions -fno-rtti -mkernel -fapple-kext -nostdinc -nostdinc++
    -std=gnu++14 -stdlib=libc++
    -Xclang -sys-header-deps
    "${IQUOTE[@]+"${IQUOTE[@]}"}"
    -isystem itl80211/openbsd -isystem itl80211 -isystem include
    -isystem MacKernelSDK/Headers
)

mkdir -p "$OBJ" "$OUT"
echo "== PCH"
"$CC" -x c++-header "${COMMON[@]}" itlwm/PrivateSPI.pch -o "$OBJ/PrivateSPI.pch.pch"

echo "== compile ${#SOURCES[@]} files ($JOBS jobs)"
OBJS=()
compile_one() {
    local s=$1 o=$2
    "$CC" -x c++ "${COMMON[@]}" -include-pch "$OBJ/PrivateSPI.pch.pch" -c "$s" -o "$o" \
        -MMD -MF "${o%.o}.d" 2> "${o%.o}.log" || { echo "FAIL $s"; cat "${o%.o}.log"; return 1; }
}
for s in "${SOURCES[@]}"; do
    o=$OBJ/$(printf '%s' "$s" | tr '/' '_').o
    OBJS+=("$o")
    rm -f "$o"
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
    compile_one "$s" "$o" &
done
wait
fails=0
for o in "${OBJS[@]}"; do [ -f "$o" ] || fails=$((fails + 1)); done
[ $fails -eq 0 ] || { echo "$fails files failed" >&2; exit 1; }

echo "== kmod info"
BUNDLE_ID=com.zxystd.AirportItlwm
cat > "$OBJ/AirportItlwm_info.c" <<EOF
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BUNDLE_ID, "$VERSION", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
EOF
"$CC" -x c -target "x86_64-apple-macos$DEPLOY" -isysroot "$SDK" -mkernel -nostdinc -isystem MacKernelSDK/Headers \
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT "$OPT" -g \
    -c "$OBJ/AirportItlwm_info.c" -o "$OBJ/AirportItlwm_info.o"

echo "== link"
K=$OUT/AirportItlwm.kext
rm -rf "$K" "$K.dSYM"
mkdir -p "$K/Contents/MacOS"
RT=$("$CC" -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a
"$CC" -target "x86_64-apple-macos$DEPLOY" -isysroot "$SDK" -arch x86_64 \
    "${OBJS[@]}" "$OBJ/AirportItlwm_info.o" \
    -Xlinker -kext -nostdlib -LMacKernelSDK/Library/x86_64 -L"$SDK/usr/lib" \
    -lkmodc++ MacKernelSDK/Library/x86_64/libkmod.a "$RT" \
    -Xlinker -export_dynamic -Xlinker -no_adhoc_codesign \
    -Xlinker -dependency_info -Xlinker "$OBJ/ld_deps.dat" \
    -o "$K/Contents/MacOS/AirportItlwm"

echo "== Info.plist"
sed -e "s/\$(DEVELOPMENT_LANGUAGE)/en/; s/\$(EXECUTABLE_NAME)/AirportItlwm/; s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$BUNDLE_ID/" \
    -e "s/\$(PRODUCT_NAME)/AirportItlwm/; s/\$(PRODUCT_BUNDLE_PACKAGE_TYPE)/KEXT/; s/\$(MODULE_VERSION)/$VERSION/g" \
    "$PLIST" > "$K/Contents/Info.plist"
plutil -insert CFBundleSupportedPlatforms -json '["MacOSX"]' "$K/Contents/Info.plist" 2>/dev/null || true
plutil -insert BuildMachineOSBuild -string "$(sw_vers -buildVersion)" "$K/Contents/Info.plist" 2>/dev/null || true
plutil -insert LSMinimumSystemVersion -string "$DEPLOY" "$K/Contents/Info.plist" 2>/dev/null || true
plutil -lint "$K/Contents/Info.plist"
if grep -q '\$(' "$K/Contents/Info.plist"; then
    echo "unresolved variable in Info.plist:" >&2
    grep '\$(' "$K/Contents/Info.plist" >&2
    exit 1
fi

echo "== dSYM"
dsymutil "$K/Contents/MacOS/AirportItlwm" -o "$K.dSYM" 2>&1 | grep -v "warning: (x86_64)" | head -5 || true
dwarfdump --uuid "$K/Contents/MacOS/AirportItlwm"
ls -la "$K/Contents/MacOS/AirportItlwm"

# The import check only makes sense for a kext built for the running macOS.
if [ -z "${CHECK_IMPORTS:-}" ]; then
    case "$(sw_vers -productVersion | cut -d. -f1):$PLATFORM" in
        26:Tahoe|15:Sequoia) CHECK_IMPORTS=1 ;;
        *) CHECK_IMPORTS=0 ;;
    esac
fi
echo "== check"
if [ "$CHECK_IMPORTS" = 1 ]; then nokc=0; else nokc=1; fi
NO_KC=$nokc "$repo/tools/check-kext.sh" "$K" || { echo "check-kext FAILED: do not install this build" >&2; exit 1; }
