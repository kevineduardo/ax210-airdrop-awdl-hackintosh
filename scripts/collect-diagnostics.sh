#!/bin/sh
# SPDX-License-Identifier: MIT
# Collects a read-only snapshot of Wi-Fi, AWDL, AirDrop and Bluetooth state into one text file.
# Usage: scripts/collect-diagnostics.sh [output-file]
# Privacy: the output contains MAC addresses, network names and device names.
# Review and redact it before attaching it to a public issue.
set -u
out=${1:-diagnostics-$(date +%Y%m%d-%H%M%S).txt}
here=$(cd "$(dirname "$0")" && pwd)
awdlctl=${AWDLCTL:-$(command -v awdlctl 2>/dev/null || echo "$here/../tools/awdlctl/awdlctl")}
L=/usr/bin/log

section() { printf '\n== %s\n' "$1"; }
{
    section "system"; sw_vers; uname -a; uptime
    section "boot-args"; nvram boot-args 2>&1
    section "kexts"; kmutil showloaded --list-only 2>/dev/null | grep -iE 'Airport|itlwm|IntelBluetooth|BTPatcher|BlueTool|Lilu'
    section "AirportItlwm build"; ioreg -r -c AirportItlwm -l -w0 2>/dev/null | grep -E '"IOClass"|CFBundleVersion|DriverVersion' | head -5
    section "recent panics"; find /Library/Logs/DiagnosticReports -maxdepth 1 -iname "*panic*" 2>/dev/null | sort -r | head -3| grep -i panic | head -3
    section "Wi-Fi"; system_profiler SPAirPortDataType 2>/dev/null | grep -E 'Card Type|Firmware Version|Status:|PHY Mode|Channel|Transmit Rate|Country Code' | head -12
    section "interfaces"; ifconfig -l; ifconfig awdl0 2>&1
    section "AWDL counters (ioreg)"; ioreg -r -c AirportItlwmAWDLInterface -l -w0 2>/dev/null | grep -E '"AWDL|BSD Name' | head -60
    section "awdlctl"; if [ -x "$awdlctl" ]; then "$awdlctl" show 2>&1; else echo "awdlctl not built (make -C tools/awdlctl)"; fi
    section "AirportItlwm AWDL log (this boot, last 80)"
    $L show --last boot --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS[c] "awdl" OR eventMessage CONTAINS[c] "itlwm")' 2>/dev/null | tail -80
    section "BLE advertising sets (BlueToolFixup)"
    $L show --last boot --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS "[advsets]"' 2>/dev/null | tail -12
    $L show --last boot --style compact --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "advertisingsets" OR eventMessage CONTAINS "enableExtended")' 2>/dev/null | tail -6
    section "Bluetooth"; system_profiler SPBluetoothDataType 2>/dev/null | grep -E 'State|Chipset|Firmware Version|Transport|Vendor ID|Product ID' | head -8
    section "Bonjour _airdrop._tcp on awdl0 (8 s)"
    dns-sd -i awdl0 -B _airdrop._tcp local. > /tmp/.awdl-dnssd.$$ 2>&1 & pid=$!; sleep 8; kill "$pid" 2>/dev/null; tail -8 /tmp/.awdl-dnssd.$$; rm -f /tmp/.awdl-dnssd.$$
    section "sharingd AirDrop (last 10 min)"
    $L show --last 10m --style compact --predicate 'process == "sharingd" AND (eventMessage CONTAINS[c] "airdrop" OR eventMessage CONTAINS[c] "awdl")' 2>/dev/null | tail -30
    section "wifip2pd (last 10 min)"
    $L show --last 10m --style compact --predicate 'process == "wifip2pd"' 2>/dev/null | tail -25
} > "$out" 2>&1
echo "Saved to $out (review it for MAC addresses and names before sharing)"
