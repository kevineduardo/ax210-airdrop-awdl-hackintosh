#!/bin/sh
# SPDX-License-Identifier: MIT
# Unit tests for the pure helpers in scripts/check-compat.sh (runs anywhere, no hackintosh needed).
set -u
here=$(cd "$(dirname "$0")" && pwd)
CHECK_COMPAT_LIB=1; export CHECK_COMPAT_LIB
# shellcheck source=../../scripts/check-compat.sh
. "$here/../../scripts/check-compat.sh"

failures=0
count=0
eq() { # eq NAME EXPECTED ACTUAL
    count=$((count + 1))
    if [ "$2" != "$3" ]; then
        printf 'FAIL %s\n  expected: [%s]\n  actual:   [%s]\n' "$1" "$2" "$3"
        failures=$((failures + 1))
    fi
}

# kernel_id
eq kernel_id.tahoe "xnu-12377.161.15.701.48~2" \
    "$(kernel_id 'Darwin Kernel Version 25.6.0: Mon Oct 20 2026; root:xnu-12377.161.15.701.48~2/RELEASE_X86_64')"
eq kernel_id.garbage "" "$(kernel_id 'nonsense')"

# macos_status
eq macos.seq.tested tested-sequoia "$(macos_status 15.8.1 24H32 xnu-11417.140.69.712.69~1)"
eq macos.tahoe.tested tested-tahoe "$(macos_status 26.7.2 25G332 xnu-12377.161.15.701.48~2)"
eq macos.build-only build-only "$(macos_status 26.7.2 25G332 xnu-12377.161.15.701.48~9)"
eq macos.seq.other sequoia-other "$(macos_status 15.8.3 24H241 xnu-1)"
eq macos.seq.too-old too-old "$(macos_status 15.1 24B83 xnu-1)"
eq macos.sonoma too-old "$(macos_status 14.8.9 23J631 xnu-1)"
eq macos.tahoe.other tahoe-other "$(macos_status 26.4.1 25E1 xnu-1)"
eq macos.newer newer "$(macos_status 27.0 26A1 xnu-1)"
eq macos.empty too-old "$(macos_status '' '' '')"

# card_kind
eq card.ax210 tested "$(card_kind 8086 2725 028000)"
eq card.ax200 iwx "$(card_kind 8086 2723 028000)"
eq card.ax211 iwx "$(card_kind 8086 51F0 028000)"
eq card.ac9560 intel-other "$(card_kind 8086 9df0 028000)"
eq card.i225 intel-ethernet "$(card_kind 8086 15f3 020000)"
eq card.broadcom other "$(card_kind 14e4 4433 028000)"

# pci_network: an AX210 behind a root port, a Broadcom card, an Intel Ethernet and a non-network device.
fixture='+-o PCI0@0  <class IOPCIDevice, id 0x1, registered>
  | {
  |   "vendor-id" = <22100000>
  |   "class-code" = <00000600>
  | }
  +-o RP01@1c  <class IOPCIDevice, id 0x2, registered>
  | | {
  | |   "vendor-id" = <86800000>
  | |   "class-code" = <00040600>
  | |   "device-id" = <bc7a0000>
  | | }
  | +-o pci8086,2725@0  <class IOPCIDevice, id 0x3, registered>
  |   | {
  |   |   "subsystem-id" = <24000000>
  |   |   "vendor-id" = <86800000>
  |   |   "class-code" = <00800200>
  |   |   "subsystem-vendor-id" = <86800000>
  |   |   "device-id" = <25270000>
  |   |   "IOName" = "pci8086,2725"
  |   | }
  |   +-o AirportItlwm  <class AirportItlwm, id 0x4>
  |       {
  |         "vendor-id" = "Intel"
  |       }
  +-o pci14e4,4433@0  <class IOPCIDevice, id 0x5>
  |   "vendor-id" = <e4140000>
  |   "class-code" = <00800200>
  |   "subsystem-vendor-id" = <6b100000>
  |   "subsystem-id" = <87430000>
  |   "device-id" = <33440000>
  +-o pci8086,15f3@0  <class IOPCIDevice, id 0x6>
      "vendor-id" = <86800000>
      "class-code" = <00000200>
      "device-id" = <f3150000>
      "subsystem-vendor-id" = <86800000>
      "subsystem-id" = <00000000>'
expected='8086 2725 8086 0024 028000
14e4 4433 106b 4387 028000
8086 15f3 8086 0000 020000'
eq pci_network "$expected" "$(printf '%s\n' "$fixture" | pci_network)"
eq pci_network.empty "" "$(printf '' | pci_network)"

# bootargs_missing / bootarg_value
full=$BOOTARGS_COMMON
eq bootargs.full "" "$(bootargs_missing "$full" "$BOOTARGS_COMMON")"
eq bootargs.empty "$BOOTARGS_COMMON" "$(bootargs_missing "" "$BOOTARGS_COMMON")"
eq bootargs.prefix "-itlawdlaw" "$(bootargs_missing "-itlawdl -itlawdlawx" "-itlawdl -itlawdlaw")"
eq bootargs.otherlead "" "$(bootargs_missing "itlawdllead=6000" "itlawdllead=30000")"
eq bootargs.nolead "itlawdllead=30000" "$(bootargs_missing "-itlawdl" "itlawdllead=30000")"
eq bootargs.tahoe "-itlawdlhometx -ibtcompatbeta" \
    "$(bootargs_missing "keepsyms=1 $BOOTARGS_COMMON debug=0x100" "$BOOTARGS_COMMON $BOOTARGS_TAHOE_EXTRA")"
eq bootarg_value.set 6000 "$(bootarg_value "-v itlawdllead=6000 x=1" itlawdllead)"
eq bootarg_value.absent "absent" "$(bootarg_value "-v itlawdlleadx=1" itlawdllead || echo absent)"

# netstack_verdict
eq net.healthy ok "$(netstack_verdict '' 1)"
eq net.broken broken "$(netstack_verdict pf 0)"
eq net.broken.multi broken "$(netstack_verdict 'pf ipfw' 0)"
eq net.off.nopf unknown "$(netstack_verdict '' 0)"
eq net.missing unknown "$(netstack_verdict '?' '?')"

# advsets_verdict (lines as BlueToolFixup.cpp logs them)
eq advsets.all all "$(printf '%s\n' \
    'btlfx: @ [advsets] BT Spec LE advertising sets for third-party controllers enabled' \
    'btlfx: @ [advsets] patched init1 in /usr/sbin/bluetoothd' \
    'btlfx: @ [advsets] all 6 patches applied (table 26.7.2)' | advsets_verdict)"
eq advsets.unverified unverified "$(printf '%s\n' \
    'btlfx: @ [advsets] all 6 patches applied (table 26.7.2, unverified build)' | advsets_verdict)"
eq advsets.perpage perpage "$(printf '%s\n' \
    'btlfx: @ [advsets] selected table 26.4.1 for Darwin 25.4 (no bluetoothd UUID recorded, per-page mode)' \
    'btlfx: @ [advsets] patched a in /usr/sbin/bluetoothd' | advsets_verdict)"
eq advsets.none none "$(printf '%s\n' \
    'btlfx: @ [advsets] /usr/sbin/bluetoothd UUID 00-00 not verified for table 15.8.1, applying none' | advsets_verdict)"
eq advsets.notable notable "$(printf '%s\n' 'btlfx: @ [advsets] no patch table for Darwin 24.1, not patching' | advsets_verdict)"
eq advsets.enabled enabled-only "$(printf '%s\n' \
    'btlfx: @ [advsets] BT Spec LE advertising sets for third-party controllers enabled' | advsets_verdict)"
eq advsets.absent absent "$(printf '' | advsets_verdict)"

# kext_version: kmutil and kextstat line shapes
kmutil_out='  120    0 0xffffff7f9a000000 0x2f0000   0x2f0000   com.zxystd.AirportItlwm (2.4.0) 1A2B3C4D-0000-0000-0000-000000000000 <99 7 6 5 3 1>
  121    3 0xffffff7f9b000000 0x30000    0x30000    as.vit9696.Lilu (1.7.1) 0000 <8 6 5 3 2 1>
  122    0 0xffffff7f9c000000 0x30000    0x30000    as.acidanthera.BlueToolFixup (2.6.9) 0000 <121 8 6 5 3 2 1>'
eq kext.aiw "com.zxystd.AirportItlwm 2.4.0" "$(printf '%s\n' "$kmutil_out" | kext_version AirportItlwm)"
eq kext.lilu "as.vit9696.Lilu 1.7.1" "$(printf '%s\n' "$kmutil_out" | kext_version Lilu)"
eq kext.btfixup "as.acidanthera.BlueToolFixup 2.6.9" "$(printf '%s\n' "$kmutil_out" | kext_version BlueToolFixup)"
eq kext.absent "" "$(printf '%s\n' "$kmutil_out" | kext_version IntelBTPatcher)"
eq kext.nosuffixmatch "" "$(printf '%s\n' '1 0 0x0 0x0 0x0 com.zxystd.itlwm (2.4.0) U <>' | kext_version AirportItlwm)"

if [ $failures -eq 0 ]; then
    echo "ok   check-compat: $count assertions"
    exit 0
fi
echo "FAIL check-compat: $failures of $count assertions failed"
exit 1
