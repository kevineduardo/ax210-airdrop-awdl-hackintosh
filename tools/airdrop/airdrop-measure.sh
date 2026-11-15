#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
# Summarise recent AirDrop transfers from the unified log: flow types and interfaces,
# QUIC connections and byte totals, and HTTP/3 transaction durations.
#
#   tools/airdrop/airdrop-measure.sh [window]     window for `log show --last` (default 30m; e.g. 2h)
#
# Run it right after one or more AirDrop transfers. Read-only. The output can include
# peer link-local addresses: redact them before posting it publicly.
set -u
W=${1:-30m}
case $W in
    -h|--help) sed -n '4,10p' "$0"; exit 0 ;;
    *[!0-9smhd]*|'') echo "usage: $0 [window, e.g. 30m | 2h]" >&2; exit 2 ;;
esac
P='process == "sharingd" AND subsystem == "com.apple.network"'
logshow() { /usr/bin/log show --last "$W" --predicate "$1" --style compact 2>/dev/null; }

echo "== system state"
sysctl net.filter_state net.link.generic.system.enable_netagent 2>/dev/null
ifconfig -v awdl0 2>/dev/null | grep FlowSwitch || echo "awdl0: no FlowSwitch line (interface missing?)"

echo "== sharingd flows by type/interface (channel-flow = userspace networking)"
logshow "$P AND eventMessage CONTAINS \"flow:start_connect\"" |
    grep -oE "(in_progress|ready) [a-z-]*flow.*interface: [a-z0-9]+" |
    sed -E 's/\(.*interface: / /' | awk '{print $2, $NF}' | sort | uniq -c

echo "== quic_time_delta errors (expected 0)"
logshow 'process == "sharingd" AND eventMessage CONTAINS "quic_time_delta start"' | grep -c sharingd

echo "== QUIC connections and totals"
logshow "$P AND (eventMessage CONTAINS \"QUIC connection established\" OR eventMessage CONTAINS \"bytes in/out\")" |
    grep -E "established|bytes in/out|ecn packets" |
    sed -E 's/^([0-9-]+ [0-9:.]+).*(QUIC connection established.*)/\1 \2/' | cut -c1-200 | sort -u

echo "== HTTP/3 transactions (body transfer = longest outbound/inbound duration)"
logshow "$P AND eventMessage CONTAINS \"transaction summary\"" |
    grep -oE "^[0-9-]+ [0-9:.]+|duration_ms=[0-9]+|outbound_duration_ms=[0-9]+|inbound_duration_ms=[0-9]+" |
    paste - - - - | awk '$4 != "outbound_duration_ms=0" || $5 != "inbound_duration_ms=0"'
