#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
# Unattended reboot test for the BlueToolFixup -btlfxadvsets patches: reboots the machine
# N times and records, for every boot, how many of the three controller-init sites are
# patched in bluetoothd and the kext's own log lines.
#
#   sudo scripts/boot-cycles.sh --start [N] [--wait SECONDS]   reboot N times (default 3, wait 50 s)
#   sudo scripts/boot-cycles.sh --status                       show the record
#   sudo scripts/boot-cycles.sh --stop                         cancel the remaining cycles
#
# --start installs a copy of this script and a LaunchDaemon, then reboots. At every boot
# the daemon waits SECONDS (so bluetoothd has started and the patcher has run), appends
# to the record and reboots while cycles remain; after the last one it removes itself.
# Record: /var/log/ax210-boot-cycles.log. Save your work first: the machine WILL reboot.
#
# "Sites patched" counts the patched byte patterns of the -btlfxadvsets init sites
# (the same patterns as driver/BlueToolFixup/BrcmPatchRAM/BlueToolFixup.cpp) in the page
# cache copy of /usr/sbin/bluetoothd: Lilu patches pages in place, so a read of the file
# while the patcher is active sees the patched bytes.
set -u
LABEL=local.ax210-airdrop-awdl-hackintosh.boot-cycles
BIN=/usr/local/libexec/ax210-boot-cycles.sh
PLIST=/Library/LaunchDaemons/$LABEL.plist
STATE=/var/db/ax210-boot-cycles
LOG=/var/log/ax210-boot-cycles.log
BLUETOOTHD=/usr/sbin/bluetoothd

usage() { sed -n '4,15p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-2}"; }
need_root() { [ "$(/usr/bin/id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }; }
remove_daemon() { /bin/rm -f "$STATE.left" "$STATE.wait" "$PLIST" "$BIN"; }

# Number of patched init sites (0-3) in the x86_64 slice of bluetoothd.
sites_patched() {
    /usr/bin/perl -e '
        open my $f, "<:raw", $ARGV[0] or do { print "?"; exit 0 }; local $/; my $d = <$f>;
        if (unpack("N", $d) == 0xcafebabe) {
            my $n = unpack("N", substr($d, 4, 4));
            for my $i (0 .. $n - 1) {
                my ($ct, $cs, $off, $sz) = unpack("N4", substr($d, 8 + $i * 20, 16));
                if ($ct == 0x01000007) { $d = substr($d, $off, $sz); last }
            }
        }
        my @patched = (
            qr/\x85\xC0\x48\x8D\x05.{4}\x48\x8D\x15.{4}\x0F\x1F\x40\x00/s,   # LE event mask: cmove -> nop4
            qr/\x83\xB8..\x00\x00\x03\xEB\x07\x31\xFF\xE8/s,                 # adv set random address: ja -> jmp +7
            qr/\xBF\x01\x00\x00\x00\x66\x90\xE8.{3}\xFF\x44\x0F\xB6\xBD/s,   # ext create connection: mov edi, 1
        );
        my $p = 0; $p += () = $d =~ /$_/g for @patched; print $p;' "$BLUETOOTHD"
}

case "${1:-}" in
--start)
    need_root
    shift
    n=3 wait=50
    while [ $# -gt 0 ]; do
        case $1 in
            --wait) wait=${2:-}; shift 2 ;;
            *) n=$1; shift ;;
        esac
    done
    case "$n$wait" in *[!0-9]*|'') usage ;; esac
    [ "$n" -ge 1 ] || usage
    /bin/mkdir -p /usr/local/libexec
    /bin/cp "$0" "$BIN" && /bin/chmod 755 "$BIN" || exit 1
    echo "$n" > "$STATE.left"
    echo "$wait" > "$STATE.wait"
    /bin/cat > "$PLIST" <<EOP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key><array><string>$BIN</string><string>--run</string></array>
    <key>RunAtLoad</key><true/>
</dict></plist>
EOP
    /bin/chmod 644 "$PLIST"
    echo "== $(/bin/date '+%F %T') start: $n cycles, wait ${wait}s, sites patched now: $(sites_patched) of 3" >> "$LOG"
    echo "rebooting; follow the record with: sudo $0 --status"
    /bin/sync
    /bin/launchctl reboot
    exit 0 ;;
--status)
    /bin/cat "$LOG" 2>/dev/null || echo "no record yet ($LOG)"
    if [ -f "$STATE.left" ]; then echo "cycles left: $(/bin/cat "$STATE.left")"; fi
    exit 0 ;;
--stop)
    need_root
    remove_daemon
    echo "== $(/bin/date '+%F %T') stopped" >> "$LOG"
    echo "stopped; no further reboots"
    exit 0 ;;
--run) ;;
-h|--help) usage 0 ;;
*) usage ;;
esac

# --run: one boot, started by the LaunchDaemon.
wait=$(/bin/cat "$STATE.wait" 2>/dev/null || echo 50)
/bin/sleep "$wait"
left=$(/bin/cat "$STATE.left" 2>/dev/null || echo 0)
{
    echo "== $(/bin/date '+%F %T') boot $(/usr/bin/uname -r), init sites patched: $(sites_patched) of 3, cycles left: $left"
    /sbin/dmesg | /usr/bin/grep -E "btlfx: @ (\[advsets\] all|stats|late start: bluetoothd)" | /usr/bin/cut -c1-260
} >> "$LOG"
if [ "$left" -gt 1 ] 2>/dev/null; then
    echo $((left - 1)) > "$STATE.left"
    /bin/sync
    /bin/launchctl reboot
else
    remove_daemon
    echo "== $(/bin/date '+%F %T') done" >> "$LOG"
fi
