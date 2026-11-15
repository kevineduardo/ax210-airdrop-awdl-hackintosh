#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
# Sanity checks for a built AirportItlwm.kext. Read-only: loads nothing.
#
#   tools/check-kext.sh <AirportItlwm.kext> [kc-syms.txt [baseline-undef.txt]]
#
# kc-syms.txt comes from your own machine, for example:
#   tools/kc/kcsyms.py exports -k com.apple.iokit.IO80211Family -k com.apple.iokit.IOSkywalkFamily \
#       -k com.apple.kernel > kc-syms.txt
#
# 1. Layout fingerprint (ieee80211_var.h): exactly one ieee80211_layout_tag
#    must be defined and none may be undefined. An undefined one means that
#    some object files were compiled against another struct ieee80211com /
#    ieee80211_node / _ifnet than ieee80211.c (stale objects), which is what
#    caused the test-5 panics.
# 2. If the dSYM is next to the kext: every compile unit must agree on the
#    size of the shared structures.
# 3. Import check (optional): every undefined symbol that the baseline kext
#    did not already import must exist in the kernel-collection symbol list.
set -u
kext=${1:?usage: check-kext.sh <kext> [kc-syms.txt [baseline-undef.txt]]}
bin="$kext/Contents/MacOS/AirportItlwm"
[ -f "$bin" ] || { echo "no binary in $kext" >&2; exit 2; }
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
rc=0

xcrun dwarfdump --uuid "$bin"

xcrun nm -u "$bin" | sort -u > "$tmp/undef"
xcrun nm -U "$bin" | awk '{print $NF}' | sort -u > "$tmp/def"
nundef=$(grep -c 'ieee80211_layout_tag' "$tmp/undef")
ndef=$(grep -c 'ieee80211_layout_tag' "$tmp/def")
if [ "$nundef" -ne 0 ] || [ "$ndef" -ne 1 ]; then
    echo "FAIL layout tag: $ndef defined, $nundef undefined (stale objects? do a clean build)"
    grep 'ieee80211_layout_tag' "$tmp/def" "$tmp/undef" | xcrun c++filt
    rc=1
else
    echo "ok   layout tag: $(grep 'ieee80211_layout_tag' "$tmp/def" | xcrun c++filt)"
fi

dsym="$kext.dSYM/Contents/Resources/DWARF/AirportItlwm"
if [ -f "$dsym" ]; then
    xcrun dwarfdump --debug-info "$dsym" | awk '
        /DW_TAG_/ { tag = $2; name = ""; size = ""; decl = 0; next }
        /DW_AT_name/ && name == "" { name = $2; gsub(/[()"]/, "", name) }
        /DW_AT_declaration/ { decl = 1 }
        /DW_AT_byte_size/ && size == "" {
            size = $2; gsub(/[()]/, "", size)
            if ((tag == "DW_TAG_structure_type" || tag == "DW_TAG_class_type") && !decl &&
                name ~ /^(ieee80211com|ieee80211_node|_ifnet|iwx_softc|iwm_softc|iwn_softc|ieee80211_rxinfo|AirportItlwmAWDLInterface|AirportItlwm)$/)
                n[name " " size]++
        }
        END { for (k in n) print k, n[k] }' | sort > "$tmp/sizes"
    bad=$(awk '{c[$1]++} END {for (k in c) if (c[k] > 1) print k}' "$tmp/sizes")
    if [ -n "$bad" ]; then
        echo "FAIL struct sizes differ between compile units: $bad"
        cat "$tmp/sizes"
        rc=1
    else
        echo "ok   struct sizes agree in all compile units:"
        sed 's/^/       /' "$tmp/sizes"
    fi
else
    echo "skip DWARF check (no dSYM at $kext.dSYM)"
fi

if [ $# -ge 2 ]; then
    sort -u "$2" > "$tmp/kc"
    if [ $# -ge 3 ]; then sort -u "$3" > "$tmp/base"; else : > "$tmp/base"; fi
    comm -23 "$tmp/undef" "$tmp/base" | comm -23 - "$tmp/kc" > "$tmp/missing"
    if [ -s "$tmp/missing" ]; then
        echo "FAIL imports not in the kernel collection ($(wc -l < "$tmp/missing" | tr -d ' ')):"
        xcrun c++filt < "$tmp/missing" | sed 's/^/       /'
        rc=1
    else
        echo "ok   imports: $(wc -l < "$tmp/undef" | tr -d ' ') undefined, none missing"
    fi
fi
exit $rc
