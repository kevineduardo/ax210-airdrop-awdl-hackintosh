#!/bin/sh
# SPDX-License-Identifier: MIT
# Read-only compatibility check for AWDL/AirDrop with an Intel Wi-Fi card on a hackintosh.
# Run it before installing (macOS build, card, boot-args) and after (kexts, awdl0, BLE
# advertising sets, user-space networking). It prints OK/WARN/FAIL lines and a verdict.
#
# Usage: scripts/check-compat.sh [--no-log]
#   --no-log   skip reading this boot's log (the BLE advertising-sets check), which can be slow
#
# It changes nothing and does not need sudo. It prints no serial numbers, MAC addresses
# or network names. Exit code: 0 when nothing FAILed, 1 otherwise.
#
# The functions above main() are pure (text in, text out) so tests/unit/test_check_compat.sh
# can source this file with CHECK_COMPAT_LIB=1 and test them without a hackintosh.
set -u

# Builds the AWDL code was tested on: "version build kernel".
TESTED_SEQUOIA="15.8.1 24H32 xnu-11417.140.69.712.69~1"
TESTED_TAHOE="26.7.2 25G332 xnu-12377.161.15.701.48~2"

# Boot arguments we test with (docs/boot-args.md, "Recommended sets").
BOOTARGS_COMMON="-itlawdl -itlawdlaw -itlawdllisten -itlawdlp2p -itlawdltx -itlawdldata -itlawdlpromisc -itlawdltxpump itlawdllead=30000 -itlawdlfollow -btlfxadvsets"
BOOTARGS_TAHOE_EXTRA="-itlawdlhometx -ibtcompatbeta"

# PCI device ids the iwx HAL (where the AWDL code lives) drives; from iwx_devices[] in
# driver/AirportItlwm/itlwm/hal_iwx/ItlIwx.cpp. 2725 (AX210) is the tested one.
IWX_DEVICES="02f0 06f0 34f0 3df0 4df0 43f0 a0f0 2720 2723 2725 2726 7a70 7af0 51f0 51f1 54f0 7f70 2729 7e40"

# ---------------------------------------------------------------- pure helpers

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

# word_in WORD LIST: true when WORD is one of the space-separated words in LIST.
word_in() {
    case " $2 " in *" $1 "*) return 0 ;; esac
    return 1
}

# kernel_id "uname -v output" -> xnu-NNNN...~N
kernel_id() { printf '%s\n' "$1" | sed -n 's/.*root:\(xnu-[^/]*\)\/.*/\1/p'; }

# macos_status VERSION BUILD KERNEL -> one word:
#   tested-sequoia | tested-tahoe   build and kernel match a tested build
#   build-only                      tested build number, different kernel string
#   sequoia-other | tahoe-other     same major version, not tested
#   newer                           newer than Tahoe
#   too-old                         older than 15.2 (no AWDL build)
macos_status() {
    v=$1 b=$2 k=$3
    for t in "$TESTED_SEQUOIA" "$TESTED_TAHOE"; do
        # shellcheck disable=SC2086
        set -- $t
        if [ "$b" = "$2" ]; then
            if [ "$k" = "$3" ]; then
                case $1 in 15.*) echo tested-sequoia ;; *) echo tested-tahoe ;; esac
            else
                echo build-only
            fi
            return
        fi
    done
    major=${v%%.*}
    minor=$(printf '%s' "$v" | cut -d. -f2)
    case $major in ''|*[!0-9]*) major=0 ;; esac
    case $minor in ''|*[!0-9]*) minor=0 ;; esac
    if [ "$major" -eq 15 ] && [ "$minor" -ge 2 ]; then echo sequoia-other
    elif [ "$major" -eq 26 ]; then echo tahoe-other
    elif [ "$major" -gt 26 ]; then echo newer
    else echo too-old
    fi
}

# pci_network: reads `ioreg -r -c IOPCIDevice -l -w0` on stdin and prints one line per
# network-class (0x02xxxx) PCI device: "vendor device subvendor subdevice class" in hex.
# ioreg shows ids as little-endian data, e.g. "vendor-id" = <86800000> is 0x8086.
pci_network() {
    awk '
    function le16(h) { gsub(/[<>]/, "", h); return tolower(substr(h, 3, 2) substr(h, 1, 2)) }
    function le24(h) { gsub(/[<>]/, "", h); return tolower(substr(h, 5, 2) substr(h, 3, 2) substr(h, 1, 2)) }
    function flush() { if (on && cls ~ /^02/) print ven, dev, sven, sdev, cls }
    /\+-o .*<class IOPCIDevice[,>]/ { flush(); on = 1; ven = dev = sven = sdev = cls = ""; next }
    !on { next }
    /"vendor-id" = </           && ven == ""  { ven = le16($NF) }
    /"device-id" = </           && dev == ""  { dev = le16($NF) }
    /"subsystem-vendor-id" = </ && sven == "" { sven = le16($NF) }
    /"subsystem-id" = </        && sdev == "" { sdev = le16($NF) }
    /"class-code" = </          && cls == ""  { cls = le24($NF) }
    END { flush() }'
}

# card_kind VENDOR DEVICE CLASS -> tested | iwx | intel-other | intel-ethernet | other
card_kind() {
    ven=$(lower "$1") dev=$(lower "$2") cls=$(lower "$3")
    if [ "$ven" != 8086 ]; then echo other; return; fi
    case $cls in 0280*) ;; *) echo intel-ethernet; return ;; esac
    if [ "$dev" = 2725 ]; then echo tested
    elif word_in "$dev" "$IWX_DEVICES"; then echo iwx
    else echo intel-other
    fi
}

# bootargs_missing ARGS REQUIRED: prints the REQUIRED words that are not in ARGS.
# A "key=value" word counts as missing only when the key is absent; use bootarg_value
# to compare the value.
bootargs_missing() {
    out=""
    for want in $2; do
        case $want in
            *=*) bootarg_value "$1" "${want%%=*}" >/dev/null || out="$out $want" ;;
            *) word_in "$want" "$1" || out="$out $want" ;;
        esac
    done
    printf '%s\n' "${out# }"
}

# bootarg_value ARGS KEY: prints the value of KEY=value; status 1 when KEY is absent.
bootarg_value() {
    for w in $1; do
        case $w in "$2"=*) printf '%s\n' "${w#*=}"; return 0 ;; esac
    done
    return 1
}

# netstack_verdict FILTER_STATE ENABLE_NETAGENT -> ok | broken | unknown
# Healthy: filter_state empty (or without pf), enable_netagent 1.
# Broken: pf in filter_state and enable_netagent 0 (docs/reference/skywalk-pf-quic.md).
netstack_verdict() {
    case $2 in
        1) echo ok ;;
        0) case " $1 " in *pf*) echo broken ;; *) echo unknown ;; esac ;;
        *) echo unknown ;;
    esac
}

# advsets_verdict: reads this boot's "[advsets]" log lines on stdin ->
#   all | perpage | unverified | none | notable | enabled-only | absent
advsets_verdict() {
    awk '
    /\[advsets\] all [0-9]+ patches applied/ && !/unverified build/ { all = 1 }
    /\[advsets\] all [0-9]+ patches applied.*unverified build/ { unv = 1 }
    /\[advsets\] unverified build:/            { unv = 1 }
    /\[advsets\] patched /                     { patched++ }
    /applying none/                            { none = 1 }
    /\[advsets\] no patch table/               { notable = 1 }
    /\[advsets\] BT Spec LE advertising sets/  { enabled = 1 }
    END {
        if (all) print "all"
        else if (unv) print "unverified"
        else if (none) print "none"
        else if (notable) print "notable"
        else if (patched) print "perpage"
        else if (enabled) print "enabled-only"
        else print "absent"
    }'
}

# kext_version BUNDLE_SUFFIX: reads `kmutil showloaded --list-only` (or kextstat) on stdin and
# prints "bundle.id version" for the first loaded kext whose id ends in BUNDLE_SUFFIX.
kext_version() {
    awk -v suf="$1" '{
        for (i = 1; i < NF; i++) {
            n = length($i); s = length(suf)
            if (n > s && substr($i, n - s + 1) == suf && substr($i, n - s, 1) == "." && $(i + 1) ~ /^\(/) {
                v = $(i + 1); gsub(/[()]/, "", v); print $i, v; exit
            }
        }
    }'
}

# ---------------------------------------------------------------- output

if [ -t 1 ]; then
    c_ok=$(printf '\033[32m'); c_warn=$(printf '\033[33m'); c_fail=$(printf '\033[31m')
    c_bold=$(printf '\033[1m'); c_off=$(printf '\033[0m')
else
    c_ok=; c_warn=; c_fail=; c_bold=; c_off=
fi
n_fail=0
n_warn=0
ok()   { printf '  %sOK  %s %s\n' "$c_ok" "$c_off" "$*"; }
warn() { printf '  %sWARN%s %s\n' "$c_warn" "$c_off" "$*"; n_warn=$((n_warn + 1)); }
fail() { printf '  %sFAIL%s %s\n' "$c_fail" "$c_off" "$*"; n_fail=$((n_fail + 1)); }
info() { printf '       %s\n' "$*"; }
section() { printf '\n%s%s%s\n' "$c_bold" "$*" "$c_off"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------- checks

check_macos() {
    section "1. macOS"
    os_ver=$(sw_vers -productVersion 2>/dev/null)
    os_build=$(sw_vers -buildVersion 2>/dev/null)
    os_kernel=$(kernel_id "$(uname -v)")
    info "macOS ${os_ver:-?} (${os_build:-?}), kernel ${os_kernel:-?}, $(uname -m)"
    if [ "$(uname -m)" != x86_64 ]; then
        fail "This is not an x86_64 machine. The kexts are for Intel/AMD hackintoshes; the checks below will find nothing."
    fi
    case $(macos_status "$os_ver" "$os_build" "$os_kernel") in
        tested-sequoia) ok "Tested build: Sequoia 15.8.1 (24H32). Use the Sequoia kext." ;;
        tested-tahoe)   ok "Tested build: Tahoe 26.7.2 (25G332). Use the Tahoe kext." ;;
        build-only)     warn "Tested build number, but the kernel string differs from the tested one (patched or custom kernel?)." ;;
        sequoia-other)  warn "Sequoia $os_ver is not tested (tested: 15.8.1). First boot with only -itlawdl -itlawdlnoinject; see docs/porting-awdl-to-new-macos-versions.md." ;;
        tahoe-other)    warn "Tahoe $os_ver is not tested (tested: 26.7.2). First boot with only -itlawdl -itlawdlnoinject; see docs/porting-awdl-to-new-macos-versions.md." ;;
        newer)          warn "macOS $os_ver is newer than anything tested (26.7.2). Expect ABI differences; see docs/porting-awdl-to-new-macos-versions.md." ;;
        *)              fail "No AWDL build for macOS ${os_ver:-?}: AWDL needs Sequoia 15.2 or later. Use upstream itlwm for Wi-Fi only." ;;
    esac
    guard=$( { ioreg -r -c AirportItlwm -l -w0; ioreg -r -c AirportItlwmAWDLInterface -l -w0; } 2>/dev/null \
        | grep '"AWDLBuildGuard"' | head -1 | sed 's/^[ |]*//' | cut -c1-200)
    if [ -n "$guard" ]; then info "Driver build guard: $guard"; fi
}

check_card() {
    section "2. Intel Wi-Fi card"
    cards=$(ioreg -r -c IOPCIDevice -l -w0 2>/dev/null | pci_network)
    found=0
    while read -r ven dev sven sdev cls; do
        [ -n "${ven:-}" ] || continue
        kind=$(card_kind "$ven" "$dev" "$cls")
        case $kind in
            other|intel-ethernet) continue ;;
        esac
        found=1
        id="device 0x$dev, subsystem $sven:$sdev"
        case $kind in
            tested) ok "Intel AX210 ($id): the tested card." ;;
            iwx) warn "Intel Wi-Fi 6/6E card driven by the iwx HAL ($id): AWDL code is there but untested on this card." ;;
            *) fail "Intel Wi-Fi card not driven by the iwx HAL ($id): AirportItlwm gives Wi-Fi only, no AWDL." ;;
        esac
    done <<EOF
$cards
EOF
    if [ $found -eq 0 ]; then
        fail "No Intel Wi-Fi card (PCI vendor 0x8086, class 0x0280) found."
        others=$(printf '%s\n' "$cards" | awk 'NF && $1 != "8086" && $5 ~ /^0280/ { printf "%s%s:%s", (n++ ? ", " : ""), $1, $2 }')
        if [ -n "$others" ]; then info "Other Wi-Fi controllers: $others (not covered by this driver)"; fi
    fi
}

check_driver() {
    section "3. Driver"
    kexts=$(kmutil showloaded --list-only 2>/dev/null)
    if [ -z "$kexts" ] && have kextstat; then kexts=$(kextstat -l 2>/dev/null); fi
    aiw=$(printf '%s\n' "$kexts" | kext_version AirportItlwm)
    if [ -z "$aiw" ]; then
        fail "AirportItlwm is not loaded."
        if printf '%s\n' "$kexts" | grep -q '\.itlwm '; then
            info "itlwm.kext is loaded instead: remove it (and HeliPort); AirportItlwm replaces both."
        fi
        return
    fi
    ok "AirportItlwm loaded: $aiw"
    fw=$(system_profiler SPAirPortDataType 2>/dev/null | sed -n 's/^ *Firmware Version: *//p' | head -1)
    if [ -n "$fw" ]; then
        case $fw in
            *-awdl-*) ok "Build from this repo: $fw" ;;
            *) warn "Firmware string has no -awdl- tag ($fw): this does not look like this repo's build." ;;
        esac
    fi
    if ioreg -r -c AirportItlwmAWDLInterface -d1 2>/dev/null | grep -q AirportItlwmAWDLInterface; then
        ok "AWDL interface object present (AirportItlwmAWDLInterface)."
    else
        warn "No AirportItlwmAWDLInterface: AWDL is off (is -itlawdl in boot-args?) or the kext is not this repo's build."
    fi
    flags=$(ifconfig awdl0 2>/dev/null | sed -n '1s/.*flags=[0-9a-fA-F]*<\([^>]*\)>.*/\1/p')
    if [ -z "$flags" ]; then
        fail "awdl0 does not exist."
    elif word_in UP "$(printf '%s' "$flags" | tr ',' ' ')"; then
        ok "awdl0 exists and is UP ($flags)."
    else
        warn "awdl0 exists but is not UP ($flags). The driver brings it up 1 s after attach unless -itlawdlnoup is set."
    fi
}

check_bootargs() {
    section "4. Boot arguments"
    booted=$(sysctl -n kern.bootargs 2>/dev/null)
    stored=$(nvram boot-args 2>/dev/null | sed 's/^boot-args[[:space:]]*//')
    info "This boot: ${booted:-(none)}"
    if [ "$stored" != "$booted" ]; then
        info "In NVRAM now (applies at next boot): ${stored:-(none)}"
    fi
    case $(macos_status "$(sw_vers -productVersion 2>/dev/null)" "$(sw_vers -buildVersion 2>/dev/null)" "$(kernel_id "$(uname -v)")") in
        tested-tahoe|tahoe-other|newer) want="$BOOTARGS_COMMON $BOOTARGS_TAHOE_EXTRA" ;;
        build-only) case $(sw_vers -productVersion 2>/dev/null) in 15.*) want=$BOOTARGS_COMMON ;; *) want="$BOOTARGS_COMMON $BOOTARGS_TAHOE_EXTRA" ;; esac ;;
        *) want=$BOOTARGS_COMMON ;;
    esac
    missing=$(bootargs_missing "$booted" "$want")
    for m in $missing; do
        case $m in
            -itlawdl) fail "-itlawdl is missing: without it no AWDL code runs." ;;
            -btlfxadvsets) fail "-btlfxadvsets is missing: without BLE advertising sets iPhones do not see the Mac." ;;
        esac
    done
    rest=""
    for m in $missing; do
        case $m in -itlawdl|-btlfxadvsets) ;; *) rest="$rest $m" ;; esac
    done
    if [ -n "$rest" ]; then warn "Missing from the tested set:$rest (see docs/boot-args.md)."; fi
    if [ -z "$missing" ]; then ok "All boot arguments of the tested set are present."; fi
    lead=$(bootarg_value "$booted" itlawdllead) && [ "$lead" != 30000 ] && warn "itlawdllead=$lead (we test with 30000)."
    if word_in -itlawdlnoinject "$booted"; then
        warn "-itlawdlnoinject is set: received AWDL frames never reach macOS, so AirDrop cannot work. Fine for a first boot only."
    fi
    if word_in -itlawdlpush "$booted"; then
        warn "-itlawdlpush is set: the old output model, with which QUIC uploads stall."
    fi
}

check_bluetooth() {
    section "5. Bluetooth"
    kexts=$(kmutil showloaded --list-only 2>/dev/null)
    if [ -z "$kexts" ] && have kextstat; then kexts=$(kextstat -l 2>/dev/null); fi
    for k in Lilu BlueToolFixup IntelBluetoothFirmware IntelBTPatcher; do
        v=$(printf '%s\n' "$kexts" | kext_version "$k")
        if [ -n "$v" ]; then ok "$k loaded: $v"; else fail "$k is not loaded."; fi
    done
    bt=""
    for c in IOBluetoothHCIController IOBluetoothHostController IOBluetoothHostControllerTransport; do
        if ioreg -r -c "$c" -d1 2>/dev/null | grep -q '<class '; then bt=$c; break; fi
    done
    if [ -n "$bt" ]; then
        ok "Bluetooth controller present ($bt)."
    else
        fail "No Bluetooth controller in the IORegistry."
    fi
    if ioreg -p IOUSB -l -w0 2>/dev/null | grep -q '"idVendor" = 32903'; then
        info "Intel USB Bluetooth device (vendor 0x8087) found."
    fi
    if ! word_in -btlfxadvsets "$(sysctl -n kern.bootargs 2>/dev/null)"; then
        info "-btlfxadvsets not set this boot: skipping the advertising-sets log check."
        return
    fi
    if [ "$NO_LOG" = 1 ]; then info "Skipping the advertising-sets log check (--no-log)."; return; fi
    info "Reading this boot's log for [advsets] lines (can take a while)..."
    lines=$(/usr/bin/log show --last boot --style compact \
        --predicate 'process == "kernel" AND eventMessage CONTAINS "[advsets]"' 2>/dev/null | grep '\[advsets\]')
    case $(printf '%s\n' "$lines" | advsets_verdict) in
        all) ok "BLE advertising sets: all patches applied." ;;
        perpage) ok "BLE advertising sets: per-page patches applied ($(printf '%s\n' "$lines" | grep -c '\[advsets\] patched ') lines)." ;;
        unverified) warn "BLE advertising sets applied on an unverified bluetoothd build (-btlfxadvsetsany)." ;;
        none) fail "BLE advertising sets: bluetoothd did not match the patch table, nothing applied. See docs/porting-awdl-to-new-macos-versions.md." ;;
        notable) fail "BLE advertising sets: no patch table for this macOS, nothing applied." ;;
        enabled-only) warn "BLE advertising sets enabled but no patch applied yet (bluetoothd not started, or patterns not found)." ;;
        *) warn "No [advsets] lines in this boot's log (BlueToolFixup without the advsets table, or the log is not readable by this user)." ;;
    esac
}

check_netstack() {
    section "6. Network stack (user-space networking)"
    fs=$(sysctl -n net.filter_state 2>/dev/null) || fs="?"
    na=$(sysctl -n net.link.generic.system.enable_netagent 2>/dev/null) || na="?"
    info "net.filter_state='$fs' net.link.generic.system.enable_netagent=$na"
    case $(netstack_verdict "$fs" "$na") in
        ok) ok "User-space networking (Skywalk netagent) is on." ;;
        broken)
            case $(sw_vers -productVersion 2>/dev/null) in
                15.*) warn "pf marked incompatible and netagent off. Check your own pf anchors or firewalls (docs/reference/skywalk-pf-quic.md)." ;;
                *) fail "pf marked incompatible and netagent off: AirDrop's QUIC transfers stall."
                   info "OpenCore Kernel > Quirks > PowerTimeoutKernelPanic is the likely cause on Tahoe; set it to false,"
                   info "or check your own pf anchors/firewalls. Details: docs/reference/skywalk-pf-quic.md." ;;
            esac ;;
        *) warn "Could not tell whether user-space networking is on (values above); see docs/reference/skywalk-pf-quic.md." ;;
    esac
}

main() {
    NO_LOG=0
    case ${1:-} in
        --no-log) NO_LOG=1 ;;
        -h|--help) sed -n '3,11p' "$0" | sed 's/^# \{0,1\}//'; return 0 ;;
        '') ;;
        *) echo "usage: $0 [--no-log]" >&2; return 2 ;;
    esac
    if [ "$(uname -s)" != Darwin ]; then echo "This check runs on macOS only." >&2; return 2; fi
    printf '%sAWDL/AirDrop compatibility check%s (read-only, changes nothing)\n' "$c_bold" "$c_off"
    check_macos
    check_card
    check_driver
    check_bootargs
    check_bluetooth
    check_netstack
    section "Verdict"
    if [ $n_fail -gt 0 ]; then
        printf '  %sFAIL%s %d problem(s), %d warning(s). Fix the FAIL lines first; docs/troubleshooting.md has details.\n' "$c_fail" "$c_off" "$n_fail" "$n_warn"
        return 1
    elif [ $n_warn -gt 0 ]; then
        printf '  %sWARN%s No problems, %d warning(s). It may work; the WARN lines are what differs from the tested setup.\n' "$c_warn" "$c_off" "$n_warn"
    else
        printf '  %sOK  %s Everything this script can check matches the tested setup.\n' "$c_ok" "$c_off"
    fi
    return 0
}

if [ "${CHECK_COMPAT_LIB:-}" != 1 ]; then
    main "$@"
    exit $?
fi
