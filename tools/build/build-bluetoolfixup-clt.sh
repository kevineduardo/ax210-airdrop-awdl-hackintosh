#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
# Build BlueToolFixup.kext with the Command Line Tools only (no Xcode).
#
#   tools/build/build-bluetoolfixup-clt.sh
#
# Needs driver/BlueToolFixup/Lilu.kext (Lilu 1.7.2 DEBUG, for its plugin SDK) and
# driver/BlueToolFixup/MacKernelSDK (see CONTRIBUTING.md). The version comes from
# CURRENT_PROJECT_VERSION in BrcmPatchRAM.xcodeproj (tools/build/pbxinfo.py).
# Output: driver/BlueToolFixup/build/clt/BlueToolFixup.kext
#
# Environment: SDK_OVERRIDE (SDK path).
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo/driver/BlueToolFixup"
MK=$PWD/MacKernelSDK
[ -d "$MK/Headers" ] || { echo "driver/BlueToolFixup/MacKernelSDK missing (see CONTRIBUTING.md)" >&2; exit 1; }
[ -d Lilu.kext/Contents/Resources ] || { echo "driver/BlueToolFixup/Lilu.kext missing (Lilu DEBUG build, see CONTRIBUTING.md)" >&2; exit 1; }
VERSION=$(python3 -I "$repo/tools/build/pbxinfo.py" BrcmPatchRAM.xcodeproj BlueToolFixup Release --setting CURRENT_PROJECT_VERSION)
SDK=${SDK_OVERRIDE:-$(xcrun --sdk macosx --show-sdk-path)}
CC=$(xcrun -f clang)
O=build/clt
K=$O/BlueToolFixup.kext
BUNDLE_ID=as.acidanthera.BlueToolFixup
rm -rf "$O"
mkdir -p "$K/Contents/MacOS"

FL=(-target x86_64-apple-macos10.8 -isysroot "$SDK" -arch x86_64 -std=c++1y -stdlib=libc++ -O3 -g
    -fno-common -fstrict-aliasing -fasm-blocks
    -fno-builtin -fno-exceptions -fno-rtti -mkernel -fapple-kext -nostdinc -nostdinc++
    -Wno-stdlibcxx-not-found -Wno-everything
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DLILU_CUSTOM_IOKIT_INIT=1
    -DPRODUCT_NAME=BlueToolFixup "-DMODULE_VERSION=$VERSION"
    -I Lilu.kext/Contents/Resources -isystem "$MK/Headers")

echo "== compile"
for s in Lilu.kext/Contents/Resources/Library/plugin_start.cpp BrcmPatchRAM/BlueToolFixup.cpp; do
    "$CC" -x c++ "${FL[@]}" -c "$s" -o "$O/$(basename "$s").o"
done

echo "== kmod info"
cat > "$O/info.c" <<EOF
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t BlueToolFixup_kern_start(kmod_info_t *, void *);
extern kern_return_t BlueToolFixup_kern_stop(kmod_info_t *, void *);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BUNDLE_ID, "$VERSION", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = BlueToolFixup_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = BlueToolFixup_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
EOF
"$CC" -x c -target x86_64-apple-macos10.8 -isysroot "$SDK" -mkernel -nostdinc -isystem "$MK/Headers" \
    -DKERNEL -O3 -g -c "$O/info.c" -o "$O/info.o"

echo "== link"
RT=$("$CC" -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a
"$CC" -target x86_64-apple-macos10.8 -isysroot "$SDK" "$O"/*.o \
    -Xlinker -kext -nostdlib -L"$SDK/usr/lib" -lkmodc++ "$MK/Library/x86_64/libkmod.a" "$RT" \
    -Xlinker -export_dynamic -Xlinker -no_adhoc_codesign -o "$K/Contents/MacOS/BlueToolFixup"

echo "== Info.plist"
sed -e "s/\$(DEVELOPMENT_LANGUAGE)/en/; s/\$(EXECUTABLE_NAME)/BlueToolFixup/; s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$BUNDLE_ID/" \
    -e "s/\$(PRODUCT_NAME:rfc1034identifier)/BlueToolFixup/g; s/\$(PRODUCT_NAME)/BlueToolFixup/g; s/\$(PRODUCT_BUNDLE_PACKAGE_TYPE)/KEXT/" \
    -e "s/\$(MODULE_VERSION)/$VERSION/g; s/\${MODULE_VERSION}/$VERSION/g; s/\$(CURRENT_PROJECT_VERSION)/$VERSION/g" \
    BrcmPatchRAM/BlueToolFixup-Info.plist > "$K/Contents/Info.plist"
plutil -insert CFBundleSupportedPlatforms -json '["MacOSX"]' "$K/Contents/Info.plist" 2>/dev/null || true
if grep -q '\$(' "$K/Contents/Info.plist"; then
    echo "unresolved variable in Info.plist:" >&2
    grep '\$(' "$K/Contents/Info.plist" >&2
    exit 1
fi
plutil -lint "$K/Contents/Info.plist"
dwarfdump --uuid "$K/Contents/MacOS/BlueToolFixup"
