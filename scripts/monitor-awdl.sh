#!/bin/zsh
# monitor-awdl.sh - record AWDL/AirDrop diagnostics (AirportItlwm, Intel AX210) while you try AirDrop transfers.
#
# SPDX-License-Identifier: MIT
#
# Usage:
#   ./monitor-awdl.sh                 record until Ctrl-C
#   ./monitor-awdl.sh -m 5            stop automatically after 5 minutes
#   ./monitor-awdl.sh -i 0.5          sample interval in seconds (default: 1)
#   ./monitor-awdl.sh -n try1         suffix for the output directory name
#   ./monitor-awdl.sh -o DIR          write to DIR instead of ./awdl-monitor-<date>-<time>[-suffix]/
#   ./monitor-awdl.sh -s DIR          only regenerate summary.md for an existing recording
#   ./monitor-awdl.sh -T              do not try tcpdump
#
# Workflow: start the script, make your AirDrop attempts, press Ctrl-C.
# Everything goes to ./awdl-monitor-<YYYYmmdd-HHMMSS>[-suffix]/ in the current directory (or -o DIR):
#   meta.txt        date, uptime, boot-args, loaded kext (UUID), macOS version
#   ioreg-start.txt / ioreg-end.txt     full properties of AirportItlwmAWDLInterface
#   awdlctl-start.txt / awdlctl-end.txt (only if awdlctl is found)
#   samples.jsonl   one line per sample: only the keys that changed (absolute values)
#   deltas.log      the same in readable form: "HH:MM:SS key+delta ..."
#   logstream.txt   filtered log stream (sharingd, wifip2pd, kernel AWDL/p2p/tcp awdl0, bluetoothd AirDrop)
#   awdl0.pcap      only if tcpdump works without sudo (usually it does not)
#   summary.md      automatic report: attempts, timeline, counters, TCP, anomalies
#
# awdlctl is looked up in $AWDLCTL, then in PATH, then in ../tools/awdlctl/awdlctl relative to
# this script. Without it the awdlctl snapshots are skipped (ioreg counters are still sampled).
#
# Does not use sudo and does not change system state: read-only (ioreg, ifconfig, log stream, awdlctl show).
# Dependencies: zsh, perl, python3 (3.9 is enough), /usr/bin/log, ioreg, ifconfig.
#
# PRIVACY: the output (awdl0.pcap, logstream.txt, ioreg/awdlctl dumps, summary.md) contains MAC
# addresses, device names and other identifying data. Review and redact it before attaching it
# to a public issue.

emulate -L zsh
setopt no_unset pipe_fail no_monitor no_notify 2>/dev/null

SCRIPT=${0:A}
BASE=${SCRIPT:h}
MINUTES=0
INTERVAL=1
SUFFIX=""
SUMMARIZE=""
OUTDIR=""
WANT_TCPDUMP=1

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$SCRIPT"; exit ${1:-0}; }
while getopts "m:i:n:o:s:Th" opt; do
  case $opt in
    m) MINUTES=$OPTARG ;;
    i) INTERVAL=$OPTARG ;;
    n) SUFFIX="-$OPTARG" ;;
    o) OUTDIR=$OPTARG ;;
    s) SUMMARIZE=$OPTARG ;;
    T) WANT_TCPDUMP=0 ;;
    h) usage 0 ;;
    *) usage 2 ;;
  esac
done
[[ $MINUTES == <->(|.<->) ]] || { print -u2 "invalid value for -m: $MINUTES"; exit 2; }
[[ $INTERVAL == <->(|.<->) && $INTERVAL != 0 ]] || { print -u2 "invalid value for -i: $INTERVAL"; exit 2; }

# awdlctl: $AWDLCTL, then PATH, then <repo>/tools/awdlctl/awdlctl.
find_awdlctl() {
  local c
  for c in "${AWDLCTL:-}" "$(command -v awdlctl 2>/dev/null)" "${BASE:h}/tools/awdlctl/awdlctl"; do
    [[ -n $c && -x $c && ! -d $c ]] && { print -r -- "$c"; return 0; }
  done
  return 1
}

# ---------------------------------------------------------------------------
# Sampler (python): ioreg + ifconfig every interval, only what changed.
# ---------------------------------------------------------------------------
read -r -d '' SAMPLER_PY <<'PY'
import json, os, re, signal, subprocess, sys, time
out, interval = sys.argv[1], float(sys.argv[2])
stop = False
def onsig(*_):
    global stop
    stop = True
for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(s, onsig)
PROP = re.compile(r'^\s*\|?\s*"([^"]+)" = (.*)$')
NOISY = {"AWDLRxGap", "AWDLPeerSync"}       # change on every sample: state, not a delta
def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return ""
def expand(key, val, num, st):
    """Strings holding counters -> individual numeric keys."""
    if key in ("AWDLDataLatencyMs", "AWDLRocDelayHistMs", "AWDLRocStartVsSlotMs"):
        for m in re.finditer(r'(\S+):(\d+)', val):
            num["%s[%s]" % (key, m.group(1))] = int(m.group(2))
    elif key == "AWDLTxPump":
        for m in re.finditer(r'([A-Za-z]+) (\d+)', val):
            num["AWDLTxPump[%s]" % m.group(1)] = int(m.group(2))
    elif key == "AWDLTxVsDwell":
        for part in val.split("|"):
            w = part.split()
            if w:
                for m in re.finditer(r'([a-z]+) (\d+)', " ".join(w[1:])):
                    num["AWDLTxVsDwell[%s.%s]" % (w[0], m.group(1))] = int(m.group(2))
    elif key == "AWDLTxOkFailByBand":
        for m in re.finditer(r'(\S+) (\d+)/(\d+)', val):
            num["AWDLTxOk[%s]" % m.group(1)] = int(m.group(2))
            num["AWDLTxFail[%s]" % m.group(1)] = int(m.group(3))
    elif key == "AWDLListenByChan":
        for m in re.finditer(r'(\d+):(\d+)/(\d+)', val):
            num["AWDLListen[%s]" % m.group(1)] = int(m.group(2))
            num["AWDLRxAF[%s]" % m.group(1)] = int(m.group(3))
    elif key == "AWDLDataMcast":
        head, _, hist = val.partition(" lat ")
        for m in re.finditer(r'([a-z]+) (\d+)', head):
            num["AWDLMcast[%s]" % m.group(1)] = int(m.group(2))
        for m in re.finditer(r'(\S+):(\d+)', hist):
            num["AWDLMcastLat[%s]" % m.group(1)] = int(m.group(2))
    elif key == "NetRxDropStats":
        for m in re.finditer(r'([a-z_]+) (\d+)', val):
            num["NetRxDrop[%s]" % m.group(1)] = int(m.group(2))
    elif key == "AWDLPeerSync":
        for m in re.finditer(r'\b(aligned|misaligned|followsusmis|adopts|gatestale|dataswitch) (\d+)', val):
            num["AWDLPeerSync[%s]" % m.group(1)] = int(m.group(2))
        st[key] = val
    elif key == "AWDLRxGap":
        # "mac frames maxGapMs over6s ageMs; ..." -> transmitters heard in the last 10 s
        ents = [e.split() for e in val.split(";") if e.strip()]
        num["PeersHeard10s"] = sum(1 for e in ents if len(e) == 5 and e[4].isdigit() and int(e[4]) < 10000)
        st[key] = val
    else:
        st[key] = val
def sample():
    num, st = {}, {}
    txt = run(["/usr/sbin/ioreg", "-r", "-c", "AirportItlwmAWDLInterface", "-l", "-w0"])
    seen = 0
    for line in txt.splitlines():
        if "+-o " in line:
            seen += 1
            if seen > 1:
                break
            continue
        m = PROP.match(line)
        if not m:
            continue
        k, v = m.group(1), m.group(2).strip()
        if not (k.startswith("AWDL") or k == "NetRxDropStats"):
            continue
        if re.fullmatch(r'-?\d+', v):
            num[k] = int(v)
        elif v in ("Yes", "No"):
            st[k] = v
        elif v.startswith('"'):
            expand(k, v.strip('"'), num, st)
    if not txt.strip():
        st["ioreg"] = "AirportItlwmAWDLInterface missing"
    ifc = run(["/sbin/ifconfig", "awdl0"])
    m = re.search(r'flags=\w+<([^>]*)>', ifc)
    st["awdl0.flags"] = m.group(1) if m else "missing"
    m = re.search(r'status: (\S+)', ifc)
    st["awdl0.status"] = m.group(1) if m else "?"
    st["awdl0.inet6"] = "yes" if "inet6 fe80" in ifc else "no"
    return num, st
prev_n, prev_s = {}, {}
jf = open(os.path.join(out, "samples.jsonl"), "a", buffering=1)
df = open(os.path.join(out, "deltas.log"), "a", buffering=1)
first = True
nxt = time.time()
while not stop:
    if os.getppid() == 1:           # the main script died: do not stay orphaned
        break
    now = time.time()
    num, st = sample()
    ts = time.strftime("%H:%M:%S", time.localtime(now)) + ".%03d" % int((now % 1) * 1000)
    cn = {k: v for k, v in num.items() if prev_n.get(k) != v}
    cs = {k: v for k, v in st.items() if prev_s.get(k) != v}
    jf.write(json.dumps({"t": round(now, 3), "ts": ts, "full": first, "n": cn, "s": cs},
                        ensure_ascii=False, separators=(",", ":")) + "\n")
    if first:
        df.write("%s [initial] %d counters; %s\n" % (ts, len(num),
                 " ".join("%s=%s" % (k, v) for k, v in sorted(st.items()) if k not in NOISY)))
    else:
        parts = ["%s%+d" % (k.replace("AWDL", "", 1), v - prev_n.get(k, 0)) for k, v in sorted(cn.items())]
        parts += ["%s=<%s>" % (k, v) for k, v in sorted(cs.items()) if k not in NOISY]
        if parts:
            df.write(ts + " " + " ".join(parts) + "\n")
    prev_n.update(num)
    prev_s.update(st)
    first = False
    nxt += interval
    while not stop and time.time() < nxt:
        time.sleep(min(0.1, max(0.0, nxt - time.time())))
    if time.time() - nxt > 5 * interval:
        nxt = time.time()
PY

# ---------------------------------------------------------------------------
# Log stream filter (perl): drops noise, keeps continuation lines of accepted
# messages and echoes the key events to the terminal.
# ---------------------------------------------------------------------------
read -r -d '' FILTER_PL <<'PL'
use strict; use warnings;
$| = 1;
my $out = shift @ARGV;
open(my $fh, '>>', $out) or die "$out: $!";
select((select($fh), $| = 1)[0]);
my $keep = 0; my $n = 0;
my $drop = qr/monitorAWDLState\[9849\]|setScheduleState\[\d+\]: reason:unknown|isInfraRealtimePacketThresholdAllowed|Streamed <private> bytes|Wrote <private> bytes|Zipper Update|Report progress|Decompression progress|activating connection: mach=false|NearbyAction\w* unchanged|All unfiltered endpoints|Returning validated endpoints|Notification center delegate|Delegating alert presentation|currentInfraTrafficType|setSyncFramePeriod|_writeEventBuffer: processing event|isScanDisallowedByAwdl|SFBLEScanner\] NearbyInfo: Clearing duplicate cache|SDXPCServer\] DeviceDiscovery(Update|Activate)|SO_NOWAKEFROMSLEEP|nw_protocol_socket_reset_linger|\[IOC DEBUG\]|findConflictingService|BLE NearbyInfo (found|lost|changed)|SFProximityEstimator|Identification removed stale/;
my $key = qr/Received (DISCOVER|ASK|UPLOAD) request$|Finished sending UPLOAD|Received UPLOAD response|(Starting|Stopping) AirDrop Bonjour Server|Ask request (START|END)|UPLOAD connection is not active|Decompression finished|Importing (START|END)|Monitoring Transfer END|AirDrop connection failed|idle connection timed out|AWDL ON:|runElection.*New P:|tcp_connection_summary \(.*awdl0|Moving to/;
my %last;
while (my $l = <STDIN>) {
    exit 0 if getppid() == 1;
    if ($l =~ /^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d/) {
        $keep = ($l !~ $drop);
        # kernel tcp: only what goes through awdl0
        $keep = 0 if $keep && $l =~ / kernel\[/ && $l =~ /\btcp[ _]/ && $l !~ /awdl0/;
        if ($keep && $l =~ $key) {
            my $s = $l; chomp $s;
            $s =~ s/^\S+ (\S+) \S+\s+(\S+?)\[\d+:\w+\] /$1 $2 /;
            $s =~ s/\(IO80211Family\) com\.apple\.p2p: //; $s =~ s/<IPv6-redacted>//g;
            my $sig = $s; $sig =~ s/^\S+ //; $sig =~ s/LFC:\d+/LFC:n/g;
            my $kind = ($sig =~ /(AWDL ON:|runElection|not active)/) ? $1 : '';
            unless ($kind && defined $last{$kind} && $last{$kind} eq $sig) {
                print STDERR "  ", substr($s, 0, 200), "\n";
            }
            $last{$kind} = $sig if $kind;
        }
    }
    print $fh $l if $keep;
}
PL

# ---------------------------------------------------------------------------
# Report (python): AirDrop attempts, per-window counters, TCP, anomalies.
# ---------------------------------------------------------------------------
read -r -d '' SUMMARY_PY <<'PY'
import json, os, re, sys
from datetime import datetime, timedelta
out = sys.argv[1]
def rd(name):
    p = os.path.join(out, name)
    return open(p, errors="replace").read() if os.path.exists(p) else ""
# ---- log -------------------------------------------------------------------
TS = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\S*\s+\S+\s+(\S+?)\[(\d+):(\w+)\]\s?(.*)$')
recs = []
for line in rd("logstream.txt").splitlines():
    m = TS.match(line)
    if m:
        try:
            t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
        except ValueError:
            continue
        recs.append([t, m.group(2), m.group(5)])
    elif recs and line.strip() and not line.startswith(("Filtering", "Timestamp")):
        recs[-1][2] += " | " + line.strip()
recs.sort(key=lambda r: r[0])
def hms(t):
    return t.strftime("%H:%M:%S.%f")[:-3]
# ---- samples ---------------------------------------------------------------
samples = []            # (datetime, accumulated numeric state, accumulated text state)
cn, cs = {}, {}
for line in rd("samples.jsonl").splitlines():
    try:
        j = json.loads(line)
    except ValueError:
        continue
    cn = dict(cn); cn.update(j.get("n", {}))
    cs = dict(cs); cs.update(j.get("s", {}))
    samples.append((datetime.fromtimestamp(j["t"]), cn, cs))
def at(t):
    """Last sample taken at or before t (or the first one)."""
    best = None
    for s in samples:
        if s[0] <= t:
            best = s
        else:
            break
    return best or (samples[0] if samples else None)
def delta(t0, t1):
    a, b = at(t0), at(t1)
    if not a or not b:
        return {}
    return {k: v - a[1].get(k, 0) for k, v in b[1].items() if v != a[1].get(k, 0)}
HIST = ("<5", "<20", "<50", "<100", "<200", "<500", "<1000", ">=1000")
def lat(d):
    return " ".join("%s:%d" % (h, d.get("AWDLDataLatencyMs[%s]" % h, 0)) for h in HIST)
def bands(d):
    return " ".join("%s %d/%d" % (b, d.get("AWDLTxOk[%s]" % b, 0), d.get("AWDLTxFail[%s]" % b, 0))
                    for b in ("2g", "unii1", "5g-mid", "unii3"))
KEYS = ["AWDLDataTxQueued", "AWDLDataTx", "AWDLDataRx", "AWDLDataTxDeferred", "AWDLDataTxDropFull",
        "AWDLDataTxErr", "AWDLTxFwOk", "AWDLTxFwFail", "AWDLTxFwBusy", "AWDLTxPSF", "AWDLTxMI",
        "AWDLTxRefused", "AWDLTxPump[windows]", "AWDLTxPump[peeraway]", "AWDLTxPump[immediate]",
        "AWDLTxPump[norun]", "AWDLTxPump[lateAF]", "AWDLListenStarted", "AWDLListenBusy",
        "AWDLListenPeerSlots", "AWDLRocStarted", "AWDLRocEnded", "AWDLRocEndFail", "AWDLRocStartFail",
        "AWDLRxFrames", "AWDLPeerUpdates", "AWDLPeerPredHit", "AWDLPeerPredMiss", "AWDLSyncUpdates",
        "AWDLPeerSync[aligned]", "AWDLPeerSync[misaligned]", "AWDLPeerSync[adopts]",
        "AWDLPeerSync[gatestale]", "AWDLPeerSync[dataswitch]", "AWDLFwUcData", "AWDLFwForeignData",
        "AWDLMcast[rx]", "AWDLMcast[queued]", "AWDLMcast[sent]", "AWDLMcast[held]", "AWDLMcast[heldmc]",
        "AWDLMcast[dropstale]", "NetRxDrop[wrongdir]"]
def mlat(d):
    return " ".join("%s:%d" % (h, d.get("AWDLMcastLat[%s]" % h, 0)) for h in HIST)
def counters(d):
    return ", ".join("%s %+d" % (k.replace("AWDL", "", 1), d[k]) for k in KEYS if d.get(k))
# ---- events ----------------------------------------------------------------
EV = [
    (r'Received DISCOVER request$', "DISCOVER"),
    (r'Received ASK request$', "ASK"),
    (r'Ask request START', "ASK_START"),
    (r'Ask request END - (\w+)', "ASK_END"),
    (r'UPLOAD connection is not active', "UPLOAD_INACTIVE"),
    (r'Cancelling ASK connection', "ASK_CANCEL"),
    (r'Received UPLOAD request$', "UPLOAD"),
    (r'Decompression finished', "DECOMP_OK"),
    (r'Importing START', "IMPORT_START"),
    (r'Importing END', "IMPORT_END"),
    (r'Monitoring Transfer END(?: - (\w+))?', "MON_END"),
    (r'AirDrop connection failed with error (.{0,80})', "CONN_FAIL"),
    (r'idle connection timed out', "IDLE_TIMEOUT"),
    (r'Receive handler error (.{0,60})', "RX_ERR"),
    (r'Finished sending UPLOAD', "SEND_DONE"),
    (r'Received UPLOAD response', "SEND_RESP"),
    (r'Starting AirDrop Bonjour Server', "BONJOUR_START"),
    (r'Stopping AirDrop Bonjour Server', "BONJOUR_STOP"),
    (r'Discoverable mode changed', "DISCOVERABLE"),
    (r'Found BLE device', "BLE_FOUND"),
    (r'Lost BLE device', "BLE_LOST"),
]
AUX = ("BONJOUR_START", "BONJOUR_STOP", "DISCOVERABLE", "BLE_FOUND", "BLE_LOST", "SEND_DONE", "SEND_RESP")
EV = [(re.compile(p), n) for p, n in EV]
events = []                         # (t, name, detail)
sched, elect, tcpsum, tcpconn, peersn, presence = [], [], {}, [], [], []
last_seq = last_master = None
SEQ = re.compile(r'AWDL ON:\[infra\((\d+)\).*?\] \[sdb:\d+\] \([^)]*\) \[([0-9 ]+)\] (.+)$')
REASON = re.compile(r'setScheduleState\[\d+\]: reason:(\w+) sc:(.+?) and')
ELEC = re.compile(r'runElection: .*New P:(\S+) R:(\S+) \[.*?HC:(\d+) RMet:(\d+).*?SMet:(\d+)')
TCPC = re.compile(r'tcp (connect incoming|connect outgoing|connected|accept|listen): \[\S*?:(\d+)<->\S*?:(\d+)\] interface: awdl0')
TCPS = re.compile(r'tcp_connection_summary (?:\((\w+):\d+\))?\s*\[\S*?:(\d+)<->\S*?:(\d+)\] interface: awdl0')
for t, proc, msg in recs:
    if proc == "sharingd":
        for rx, name in EV:
            m = rx.search(msg)
            if m:
                det = m.group(1) if m.groups() and m.group(1) else ""
                if events and events[-1][1] == name and (t - events[-1][0]).total_seconds() < 0.05:
                    break
                events.append((t, name, det))
                break
    elif proc == "wifip2pd":
        m = re.search(r'Posted peer (absence|presence) for (\S+)', msg)
        if m:
            presence.append((t, m.group(1), m.group(2)))
    elif proc == "kernel":
        m = SEQ.search(msg)
        if m:
            cur = (m.group(2).strip(), m.group(3).strip())
            if cur != last_seq:
                sched.append((t, "sequence [%s] %s (infra %s)" % (cur[0], cur[1], m.group(1))))
                last_seq = cur
            continue
        m = REASON.search(msg)
        if m and m.group(1) != "unknown":
            sched.append((t, "setScheduleState %s -> %s" % (m.group(1), m.group(2))))
            continue
        m = ELEC.search(msg)
        if m:
            cur = (m.group(2), m.group(4), m.group(5))
            if cur != last_master:
                elect.append((t, m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))))
                last_master = cur
            continue
        m = TCPS.search(msg)
        if m:
            key = (m.group(2), m.group(3))
            e = tcpsum.setdefault(key, {"t": t, "why": m.group(1) or "", "txt": ""})
            if m.group(1):
                e["why"] = m.group(1)
            e["txt"] += " " + msg
            continue
        m = TCPC.search(msg)
        if m:
            tcpconn.append((t, m.group(1), m.group(2), m.group(3)))
            continue
        m = re.search(r'AWDLStateDump: AWDL up time: .*? (\S+) peers:(\d+)', msg)
        if m and (not peersn or peersn[-1][1] != int(m.group(2))):
            peersn.append((t, int(m.group(2))))
def tcpinfo(txt):
    f = {}
    for name, p in (("state", r't_state: (\w+)'), ("dur", r'Duration: ([\d.]+) sec'), ("conn", r'Conn_Time: ([\d.]+) sec'),
                    ("bytes", r'bytes in/out: (\d+/\d+)'), ("pkts", r'pkts in/out: (\d+/\d+)'),
                    ("rxmit", r'pkt rxmit: (\d+)'), ("ooo", r'ooo pkts: (\d+)'), ("dup", r'dup bytes in: (\d+)'),
                    ("rtt", r'rtt: ([\d.]+) ms'), ("syn", r'SYN in/out: (\d+/\d+)'), ("fin", r'FIN in/out: (\d+/\d+)'),
                    ("rst", r'RST in/out: (\d+/\d+)')):
        m = re.search(p, txt)
        f[name] = m.group(1) if m else "?"
    return f
def tcpline(key, e):
    f = tcpinfo(e["txt"])
    return ("%s ports %s<->%s (%s, %s): duration %s s, connected in %s s, bytes in/out %s, packets in/out %s, "
            "rxmit %s, ooo %s, dup in %s, rtt %s ms, SYN in/out %s, FIN %s, RST in/out %s" %
            (hms(e["t"]), key[0], key[1], f["state"], e["why"] or "-", f["dur"], f["conn"], f["bytes"], f["pkts"],
             f["rxmit"], f["ooo"], f["dup"], f["rtt"], f["syn"], f["fin"], f["rst"])), f
# ---- attempts --------------------------------------------------------------
attempts = []
cur = None
last_discover = None
for t, name, det in events:
    if name == "DISCOVER":
        last_discover = t
    if name == "ASK":
        cur = {"ask": t, "ev": [], "acc": None,
               "discover": last_discover if last_discover and (t - last_discover).total_seconds() < 20 else None}
        attempts.append(cur)
    if name in AUX:
        continue
    if cur is not None and name != "DISCOVER":
        # after the accept only ~45 s matter (an idle connection is closed after 30 s)
        ref = cur["acc"] or cur["ask"]
        if (t - ref).total_seconds() > (900 if cur.get("upl") else 45 if cur["acc"] else 120):
            cur = None
            continue
        cur["ev"].append((t, name, det))
        if name == "ASK_END":
            cur["acc"] = t
        if name == "UPLOAD":
            cur["upl"] = t
        if name == "IMPORT_END" or (name == "MON_END" and det):
            cur = None
t_end = recs[-1][0] if recs else (samples[-1][0] if samples else datetime.now())
t_begin = recs[0][0] if recs else (samples[0][0] if samples else t_end)
if samples:
    t_begin = min(t_begin, samples[0][0]); t_end = max(t_end, samples[-1][0])
anomalies = []
L = []
w = L.append
w("# AWDL/AirDrop recording report")
w("")
w("- Directory: `%s`" % out)
w("- Window: %s → %s (%.0f s); %d samples, %d log lines" %
  (t_begin.strftime("%Y-%m-%d %H:%M:%S"), t_end.strftime("%H:%M:%S"), (t_end - t_begin).total_seconds(), len(samples), len(recs)))
meta = rd("meta.txt")
for k in ("boot-args", "kext", "macOS", "uptime"):
    m = re.search(r'^%s: (.*)$' % re.escape(k), meta, re.M)
    if m:
        w("- %s: `%s`" % (k, m.group(1).strip()))
if samples:
    s0, s1 = samples[0], samples[-1]
    w("- TX modes (start): `%s`" % s0[2].get("AWDLTxModes", "?"))
    if s1[2].get("AWDLTxModes") != s0[2].get("AWDLTxModes"):
        w("- TX modes (end): `%s`" % s1[2].get("AWDLTxModes", "?"))
    w("- Sync master (driver): start `%s`, end `%s`; own address `%s`" %
      (s0[2].get("AWDLSyncMaster", "?"), s1[2].get("AWDLSyncMaster", "?"), s1[2].get("AWDLRadioAddr", "?")))
    if s1[2].get("AWDLPeerSync"):
        w("- Peer alignment (end): `%s`" % s1[2]["AWDLPeerSync"])
    w("- Own sequence `%s` · peer sequence `%s`" %
      (s1[2].get("AWDLChanSeq", "?").strip(), s1[2].get("AWDLPeerChanSeq", "?").strip()))
w("")
def state_at(t):
    seq = [s for s in sched if s[0] <= t and s[1].startswith("sequence")]
    el = [e for e in elect if e[0] <= t]
    return (seq[-1][1] if seq else "?"), (el[-1] if el else None)
def role_txt(e, own):
    if not e:
        return "unknown (no runElection line in the window)"
    who = "this Mac (MASTER)" if own and e[1].lower() == own.lower() else "%s (Mac is SLAVE)" % e[1]
    return "%s — master metric %d, own metric %d, hops %d" % (who, e[3], e[4], e[2])
own = samples[-1][2].get("AWDLRadioAddr", "") if samples else ""
if not own:
    m = re.search(r'AWDLStateDump: AWDL up time: .*? ([0-9A-Fa-f:]{17}) peers', "\n".join(r[2] for r in recs[-400:]))
    own = m.group(1) if m else ""
w("## AirDrop attempts (receiving on the Mac)")
w("")
if not attempts:
    w("No ASK request received during the recording (%d DISCOVER requests)." % sum(1 for e in events if e[1] == "DISCOVER"))
    w("")
NAMES = {"ASK": "ASK received", "ASK_START": "request shown to the user", "ASK_END": "ASK answered",
         "UPLOAD_INACTIVE": "\"UPLOAD connection is not active\" (also appears on successful transfers)",
         "ASK_CANCEL": "ASK HTTP stream closed (normal)", "UPLOAD": "UPLOAD received",
         "DECOMP_OK": "decompression finished", "IMPORT_START": "import started", "IMPORT_END": "import finished",
         "MON_END": "transfer monitoring ended", "CONN_FAIL": "AirDrop connection failed",
         "IDLE_TIMEOUT": "connection idle for 30 s: closed", "RX_ERR": "receive handler error"}
for i, a in enumerate(attempts, 1):
    nxt = attempts[i]["ask"] if i < len(attempts) else None
    ev = a["ev"]
    get = lambda n: next((e for e in ev if e[1] == n), None)
    acc, upl, imp, monend = get("ASK_END"), get("UPLOAD"), get("IMPORT_END"), get("MON_END")
    fails = [e for e in ev if e[1] in ("CONN_FAIL", "IDLE_TIMEOUT", "RX_ERR")]
    last_t = ev[-1][0] if ev else a["ask"]
    if imp:
        res, ok = "SUCCESS", True
    elif not acc:
        res, ok = "NO ANSWER to the ASK (not accepted/declined during the recording)", None
    elif acc[2] != "Accept":
        res, ok = "ASK ended with \"%s\"" % acc[2], None
    elif not upl:
        waited = ((nxt or t_end) - acc[0]).total_seconds()
        res, ok = "FAILURE: the UPLOAD never arrived (%.0f s observed after the accept)" % waited, False
        if waited < 5:
            res, ok = "INCOMPLETE: recording ended %.1f s after the accept, no UPLOAD" % waited, None
    elif monend and monend[2]:
        res, ok = "FAILURE: transfer interrupted (%s)" % monend[2], False
    else:
        res, ok = "INCOMPLETE: UPLOAD received, no import end in the recording (transfer in progress or stalled)", None
    t0 = a["discover"] or a["ask"] - timedelta(seconds=2)
    t1 = (imp[0] if imp else last_t) + timedelta(seconds=2)
    if nxt and t1 > nxt:
        t1 = nxt
    w("### Attempt %d — %s — %s" % (i, hms(a["ask"]), res))
    w("")
    seq, el = state_at(acc[0] if acc else a["ask"])
    w("- State at accept: %s; AWDL master: %s" % (seq, role_txt(el, own)))
    sa = at(acc[0] if acc else a["ask"])
    if sa and sa[2].get("AWDLPeerSync"):
        w("- Peer alignment at that moment: `%s`" % sa[2]["AWDLPeerSync"])
        m = re.search(r'role (\w+) .*slotoff (\d+) err (-?\d+)', sa[2]["AWDLPeerSync"])
        if m and (int(m.group(2)) != 0 or abs(int(m.group(3))) > 6000):
            anomalies.append("Attempt %d: peer timeline MISALIGNED with the Mac's at accept (role %s, slotoff %s, error %s µs)" %
                             (i, m.group(1), m.group(2), m.group(3)))
    w("")
    w("| Time | Δ since ASK | Event |")
    w("|---|---|---|")
    rows = []
    if a["discover"]:
        rows.append((a["discover"], "DISCOVER received"))
    for t, n, det in ev:
        if nxt and t >= nxt:
            break
        txt = NAMES.get(n, n) + ((": **%s**" % det) if det else "")
        rows.append((t, txt))
    for t, txt in sched:
        if t0 <= t <= t1 + timedelta(seconds=3):
            rows.append((t, "kernel: " + txt))
    for e in elect:
        if t0 <= e[0] <= t1 + timedelta(seconds=3):
            rows.append((e[0], "election: master %s (metric %d vs. own %d)" % (e[1], e[3], e[4])))
    for t, kind, pa, pb in tcpconn:
        if t0 <= t <= t1:
            rows.append((t, "tcp %s %s<->%s" % (kind, pa, pb)))
    seen = set()
    for t, txt in sorted(rows, key=lambda r: r[0]):
        if (t, txt) in seen:
            continue
        seen.add((t, txt))
        w("| %s | %+.3f s | %s |" % (hms(t), (t - a["ask"]).total_seconds(), txt))
    w("")
    if acc and upl:
        w("- Accept → UPLOAD: %.3f s" % (upl[0] - acc[0]).total_seconds())
    if upl and imp:
        w("- UPLOAD → import finished: %.3f s" % (imp[0] - upl[0]).total_seconds())
    if acc and acc[2] == "Accept":
        ch = [s for s in sched if s[1].startswith("sequence") and -1 <= (s[0] - acc[0]).total_seconds() <= 10]
        for s in ch:
            anomalies.append("Attempt %d: channel sequence changed %+.2f s relative to the accept (%s)" %
                             (i, (s[0] - acc[0]).total_seconds(), s[1]))
    d = delta(t0, t1)
    if d:
        w("- Counters in window %s–%s: %s" % (hms(t0)[:8], hms(t1)[:8], counters(d) or "no changes"))
        w("- Data latency (queue → firmware), ms: `%s`" % lat(d))
        w("- TX ok/fail per band: `%s`" % bands(d))
        slow = sum(d.get("AWDLDataLatencyMs[%s]" % h, 0) for h in ("<1000", ">=1000"))
        if slow:
            anomalies.append("Attempt %d: %d data frames waited ≥ 500 ms in the driver queue" % (i, slow))
        if d.get("AWDLTxFwFail", 0) >= 3:
            anomalies.append("Attempt %d: TxFwFail +%d (%s)" % (i, d["AWDLTxFwFail"], bands(d)))
        if d.get("AWDLDataTxDropFull"):
            anomalies.append("Attempt %d: data queue full, %d frames dropped" % (i, d["AWDLDataTxDropFull"]))
    elif not samples:
        w("- (no counter samples in this recording)")
    ports = set()
    for t, kind, pa, pb in tcpconn:
        if t0 <= t <= t1:
            ports.add((pa, pb))
    shown = False
    for key, e in sorted(tcpsum.items(), key=lambda kv: kv[1]["t"]):
        if key in ports or (t0 <= e["t"] <= t1):
            line, f = tcpline(key, e)
            if not shown:
                w("- TCP connections on awdl0 for this attempt:")
                shown = True
            w("  - " + line)
    if ok is False:
        anomalies.append("Attempt %d (%s): %s" % (i, hms(a["ask"]), res))
    w("")
# ---- reappearance ----------------------------------------------------------
w("## Reappearance after each transfer")
w("")
w("Time from the end of a transfer until the iPhone contacts the Mac again over AWDL. These approximate "
  "\"the Mac showed up again in the share sheet\": the iPhone only sends DISCOVER when the share sheet is open, "
  "so the time counts from the end of the transfer **or** from when the sheet was reopened, whichever is later.")
w("")
ends = [(t, "receive finished (Importing END)") for t, n, d in events if n == "IMPORT_END"]
ends += [(t, "send finished (UPLOAD response)") for t, n, d in events if n == "SEND_RESP"]
ends.sort()
if not ends:
    w("No completed transfer in the recording.")
    w("")
AUXN = {"BONJOUR_START": "AirDrop Bonjour server started", "BONJOUR_STOP": "AirDrop Bonjour server stopped",
        "DISCOVERABLE": "discoverable mode changed (sharingd)", "BLE_FOUND": "AirDrop BLE device found",
        "BLE_LOST": "AirDrop BLE device lost"}
for i, (te, what) in enumerate(ends, 1):
    lim = ends[i][0] if i < len(ends) else t_end
    nd = next((t for t, n, d in events if n == "DISCOVER" and te < t <= lim), None)
    ns = next((t for t, k, pa, pb in tcpconn if k == "connect incoming" and "8770" in (pa, pb) and te < t <= lim), None)
    na = next((t for t, n, d in events if n == "ASK" and te < t <= lim), None)
    stop = nd or ns or lim
    w("### After transfer %d — %s at %s" % (i, what, hms(te)))
    w("")
    f = lambda t: ("%.1f s (%s)" % ((t - te).total_seconds(), hms(t))) if t else "did not happen within %.0f s of recording" % (lim - te).total_seconds()
    w("- First incoming SYN on port 8770 (awdl0): %s" % f(ns))
    w("- First DISCOVER received: %s" % f(nd))
    w("- Next ASK: %s" % f(na))
    rows = [(t, AUXN[n]) for t, n, d in events if n in AUXN and te < t <= stop]
    rows += [(t, "kernel: " + txt) for t, txt in sched if te < t <= stop]
    rows += [(t, "wifip2pd: peer %s %s" % (mac, "present" if k == "presence" else "absent")) for t, k, mac in presence if te < t <= stop]
    rows += [(e[0], "election: master %s" % e[1]) for e in elect if te < e[0] <= stop]
    seenr = set()
    for t, txt in sorted(rows):
        if txt in seenr and txt.startswith("AirDrop BLE device"):
            continue
        seenr.add(txt)
        w("  - %+.1f s %s" % ((t - te).total_seconds(), txt))
    restart = [t for t, n, d in events if n == "BONJOUR_START" and te < t <= stop]
    first = nd or ns
    if first and restart and 0 <= (first - restart[-1]).total_seconds() <= 6:
        w("- **Contact only resumed %.1f s after the Bonjour server restarted** (fresh mDNS announcement and Discovery sequence), "
          "not in response to a query from the iPhone." % (first - restart[-1]).total_seconds())
        anomalies.append("After transfer %d: the iPhone only contacted the Mac again after the Bonjour server restarted (%.1f s after the end)" %
                         (i, (first - te).total_seconds()))
    elif first and (first - te).total_seconds() > 15:
        anomalies.append("After transfer %d: %.0f s until the next contact from the iPhone" % (i, (first - te).total_seconds()))
    elif not first and (lim - te).total_seconds() > 20:
        anomalies.append("After transfer %d: no contact from the iPhone in %.0f s" % (i, (lim - te).total_seconds()))
    d = delta(te, stop)
    hasmc = bool(samples) and any(k.startswith("AWDLMcast") for k in samples[-1][1])
    if d and not hasmc:
        w("- Other counters: %s" % (counters(d) or "no changes"))
        d = {}
    if d:
        w("- Group (multicast) data in the interval: received %+d, queued %+d, sent %+d, ticks held by the gate %+d "
          "(of which with multicast at the head %+d), dropped as stale %+d; `wrongdir` %+d" %
          (d.get("AWDLMcast[rx]", 0), d.get("AWDLMcast[queued]", 0), d.get("AWDLMcast[sent]", 0), d.get("AWDLMcast[held]", 0),
           d.get("AWDLMcast[heldmc]", 0), d.get("AWDLMcast[dropstale]", 0), d.get("NetRxDrop[wrongdir]", 0)))
        w("- Multicast latency (queue → firmware), ms: `%s`" % mlat(d))
        w("- Other counters: %s" % (counters(d) or "no changes"))
        sa = at(stop)
        if sa and sa[2].get("AWDLPeerSync"):
            w("- Peer followed at the end of the interval: `%s`" % sa[2]["AWDLPeerSync"])
    w("")
# multicast seen in the pcap (only exists if the script ran with BPF access)
pcap = os.path.join(out, "awdl0.pcap")
if os.path.exists(pcap) and os.path.getsize(pcap) > 24:
    import subprocess
    try:
        txt = subprocess.run(["/usr/sbin/tcpdump", "-r", pcap, "-n", "-e", "-tttt"], capture_output=True, text=True, timeout=60).stdout
    except Exception:
        txt = ""
    me = (own or "").lower()
    inmc = outmc = 0
    firstin = None
    for line in txt.splitlines():
        m = re.match(r'(\S+ \S+) (\S+) > (\S+?),', line)
        if not m:
            continue
        src, dst = m.group(2).lower(), m.group(3).lower()
        if int(dst[:2], 16) & 1:
            if me and src == me:
                outmc += 1
            else:
                inmc += 1
                firstin = firstin or m.group(1)
    w("Capture `awdl0.pcap`: %d group packets sent by the Mac, **%d received from others**%s." %
      (outmc, inmc, (" (first at %s)" % firstin[11:23]) if firstin else ""))
    if outmc and not inmc:
        anomalies.append("pcap: no multicast packet received on awdl0 (peers' mDNS/ND does not reach the stack)")
    w("")
if samples:
    d = delta(samples[0][0], samples[-1][0])
    if any(k.startswith("AWDLMcast") for k in samples[-1][1]):
        w("Multicast over the whole recording: received %+d, sent %+d of %+d queued; latency `%s`." %
          (d.get("AWDLMcast[rx]", 0), d.get("AWDLMcast[sent]", 0), d.get("AWDLMcast[queued]", 0), mlat(d)))
    else:
        w("(This kext does not publish `AWDLDataMcast`: no separate multicast counters.)")
    w("")
# ---- global TCP ------------------------------------------------------------
w("## TCP on awdl0 (all connections closed during the recording)")
w("")
if not tcpsum:
    w("No `tcp_connection_summary` on awdl0.")
tot = {"rxmit": 0, "rst_in": 0, "rst_out": 0, "synfail": 0, "n": 0}
for key, e in sorted(tcpsum.items(), key=lambda kv: kv[1]["t"]):
    line, f = tcpline(key, e)
    w("- " + line)
    tot["n"] += 1
    if f["rxmit"].isdigit():
        tot["rxmit"] += int(f["rxmit"])
    if "/" in f["rst"]:
        a_, b_ = f["rst"].split("/")
        tot["rst_in"] += int(a_); tot["rst_out"] += int(b_)
    if f["state"] == "SYN_SENT":
        tot["synfail"] += 1
    if f["rxmit"].isdigit() and int(f["rxmit"]) >= 3:
        anomalies.append("TCP %s<->%s: %s retransmissions" % (key[0], key[1], f["rxmit"]))
if tcpsum:
    w("")
    w("Totals: %d connections, %d retransmissions, RST in/out %d/%d, %d outgoing connections that never got past SYN_SENT." %
      (tot["n"], tot["rxmit"], tot["rst_in"], tot["rst_out"], tot["synfail"]))
    if tot["rst_in"]:
        anomalies.append("%d RSTs received from the peer on awdl0 connections (the iPhone also closes DISCOVER "
                         "connections idle for 30 s this way; only relevant inside an attempt)" % tot["rst_in"])
    if tot["synfail"]:
        anomalies.append("%d outgoing connections on awdl0 stayed in SYN_SENT (the peer never answered the SYN)" % tot["synfail"])
w("")
# ---- AWDL state ------------------------------------------------------------
w("## AWDL state during the recording")
w("")
w("Scheduler sequence/state changes (kernel):")
w("")
for t, txt in sched[:80]:
    w("- %s %s" % (hms(t), txt))
if not sched:
    w("- (no `AWDL ON:` line in the recording)")
w("")
w("Master election (changes):")
w("")
for e in elect[:60]:
    w("- %s master %s%s, hops %d, master metric %d, own metric %d" %
      (hms(e[0]), e[1], " (this Mac)" if own and e[1].lower() == own.lower() else "", e[2], e[3], e[4]))
if not elect:
    w("- (no `runElection` line in the recording)")
w("")
if peersn:
    w("Peers according to IO80211: " + ", ".join("%s→%d" % (hms(t)[:8], n) for t, n in peersn[:40]))
    w("")
# ---- global counters + per-sample anomalies ---------------------------------
if samples:
    d = delta(samples[0][0], samples[-1][0])
    w("## Counters (whole recording)")
    w("")
    w("- %s" % (counters(d) or "no changes"))
    w("- Data latency, ms: `%s` · absolute max %s ms" %
      (lat(d), samples[-1][1].get("AWDLDataLatencyMaxMs", "?")))
    w("- TX ok/fail per band: `%s`" % bands(d))
    w("- Dwell request→START delay, ms: `%s`" %
      " ".join("%s:%d" % (h, d.get("AWDLRocDelayHistMs[%s]" % h, 0)) for h in ("<2", "<5", "<10", "<20", "<40", "<80", "<160", ">=160")))
    ls, lp = d.get("AWDLListenStarted", 0) + d.get("AWDLListenBusy", 0), d.get("AWDLListenPeerSlots", 0)
    if ls:
        w("- Dwells requested %d, of which %d (%.0f %%) in peer slots and %d refused (busy). In Low Power, with the peer's timeline "
          "aligned with the Mac's, expect ~25 %%; much higher values indicate misalignment (see `AWDLPeerSync`)." %
          (ls, lp, 100.0 * lp / ls, d.get("AWDLListenBusy", 0)))
        if ls > 40 and lp > 0.7 * ls:
            anomalies.append("%.0f %% of dwells were in peer slots: peer timeline misaligned with the Mac's" % (100.0 * lp / ls))
    w("")
    prev = samples[0]
    pend_run = 0
    for s in samples[1:]:
        dt = (s[0] - prev[0]).total_seconds()
        g = lambda k: s[1].get(k, 0) - prev[1].get(k, 0)
        if g("AWDLTxFwFail") >= 3:
            anomalies.append("%s TxFwFail +%d in a %.1f s interval" % (hms(s[0]), g("AWDLTxFwFail"), dt))
        if g("AWDLDataLatencyMs[>=1000]") > 0:
            anomalies.append("%s %d data frame(s) with latency ≥ 1 s (max %s ms)" %
                             (hms(s[0]), g("AWDLDataLatencyMs[>=1000]"), s[1].get("AWDLDataLatencyMaxMs", "?")))
        tick, away = g("AWDLTxPump[ticks]"), g("AWDLTxPump[peeraway]")
        if tick >= 200 and away > 0.6 * tick and g("AWDLDataTxQueued") > 0:
            anomalies.append("%s peer off-channel in %d of %d pump ticks with data queued" % (hms(s[0]), away, tick))
        pend_run = pend_run + dt if s[1].get("AWDLDataTxPending", 0) > 0 else 0
        if pend_run >= 2 and pend_run - dt < 2:
            anomalies.append("%s data queue has had pending frames for ≥ 2 s (%d)" % (hms(s[0]), s[1].get("AWDLDataTxPending", 0)))
        if g("AWDLRocStartFail") > 0:
            anomalies.append("%s firmware refused %d dwell(s) (RocStartFail)" % (hms(s[0]), g("AWDLRocStartFail")))
        if g("AWDLPeerSync[adopts]") > 0:
            anomalies.append("%s -itlawdlfollow: peer timeline adopted (%d time(s))" % (hms(s[0]), g("AWDLPeerSync[adopts]")))
        for k in ("awdl0.flags", "awdl0.status", "AWDLSyncMaster", "AWDLTxModes", "AWDLAWRunning", "ioreg"):
            if s[2].get(k) != prev[2].get(k):
                anomalies.append("%s %s: %s → %s" % (hms(s[0]), k, prev[2].get(k), s[2].get(k)))
        prev = s
w("## Anomalies")
w("")
if anomalies:
    seen = set()
    for a in anomalies:
        if a not in seen:
            w("- " + a)
            seen.add(a)
else:
    w("Nothing abnormal detected by the automatic rules.")
w("")
w("_Notes: the driver publishes its counters in batches, so per-window deltas have ~1 s resolution; "
  "the line \"UPLOAD connection is not active\" also appears on successful transfers and only counts as a failure "
  "if the UPLOAD does not follow._")
w("")
w("_Privacy: this report and the files next to it contain MAC addresses and device names. "
  "Review and redact them before attaching them to a public issue._")
open(os.path.join(out, "summary.md"), "w").write("\n".join(L) + "\n")
print("\n".join(L[:12]))
ok = sum(1 for l in L if l.startswith("### Attempt") and "SUCCESS" in l)
print("... %d attempt(s), %d successful, %d anomaly(ies). Report: %s" %
      (len(attempts), ok, len(set(anomalies)), os.path.join(out, "summary.md")))
PY

summarize() { /usr/bin/python3 -c "$SUMMARY_PY" "$1"; }

if [[ -n $SUMMARIZE ]]; then
  [[ -d $SUMMARIZE ]] || { print -u2 "no such directory: $SUMMARIZE"; exit 2; }
  summarize "${SUMMARIZE:A}"
  exit $?
fi

if [[ -n $OUTDIR ]]; then
  OUT=${OUTDIR:A}
else
  OUT=$PWD/awdl-monitor-$(date +%Y%m%d-%H%M%S)$SUFFIX
fi
mkdir -p "$OUT" || exit 1

AWDLCTL_BIN=$(find_awdlctl) || AWDLCTL_BIN=""
if [[ -z $AWDLCTL_BIN ]]; then
  print -u2 "WARNING: awdlctl not found (set \$AWDLCTL, put it in PATH or build tools/awdlctl); continuing without awdlctl snapshots."
fi

ioreg_awdl() { /usr/sbin/ioreg -r -c AirportItlwmAWDLInterface -l -w0 2>&1; }
{
  print "date: $(date '+%Y-%m-%d %H:%M:%S %z')"
  print "uptime: $(uptime | sed 's/^ *//')"
  print "macOS: $(sw_vers -productVersion 2>/dev/null) $(sw_vers -buildVersion 2>/dev/null)"
  print "boot-args: $(sysctl -n kern.bootargs 2>/dev/null)"
  print "kext: $(kmutil showloaded 2>/dev/null | grep -i AirportItlwm | sed 's/^ *//' | tr -s ' ')"
  print "interval: $INTERVAL s; limit: $MINUTES min"
  print "awdlctl: ${AWDLCTL_BIN:-not found}"
  print -r -- "--- ifconfig awdl0"
  ifconfig awdl0 2>&1
  print -r -- "--- ifconfig en0 (summary)"
  ifconfig en0 2>&1 | grep -E "flags|status|inet "
} > "$OUT/meta.txt"
ioreg_awdl > "$OUT/ioreg-start.txt"
[[ -n $AWDLCTL_BIN ]] && "$AWDLCTL_BIN" show > "$OUT/awdlctl-start.txt" 2>&1
if ! grep -q AirportItlwmAWDLInterface "$OUT/ioreg-start.txt"; then
  print -u2 "WARNING: AirportItlwmAWDLInterface is not in the IORegistry (awdl0 not created?). Recording the log only."
fi

typeset -a PIDS
PIDS=()
STOP=0
CLEANED=0

# log stream (no sudo needed). The exec makes the PID that of "log" itself.
PRED='(process == "sharingd" AND (subsystem == "com.apple.sharing" OR subsystem == "com.apple.network")) OR (process == "wifip2pd") OR (process == "kernel" AND (eventMessage CONTAINS[c] "awdl" OR eventMessage CONTAINS "so_gencnt" OR eventMessage CONTAINS "com.apple.p2p" OR eventMessage CONTAINS "IO80211AWDL" OR eventMessage CONTAINS "runElection")) OR (process == "bluetoothd" AND (eventMessage CONTAINS[c] "airdrop" OR eventMessage CONTAINS "NearbyAction" OR eventMessage CONTAINS[c] "advertising started" OR eventMessage CONTAINS[c] "advertising stopped" OR eventMessage CONTAINS[c] "AdvSet"))'
{ exec /usr/bin/log stream --style compact --level debug --predicate "$PRED" 2>"$OUT/logstream.err" } \
  | /usr/bin/perl -e "$FILTER_PL" "$OUT/logstream.txt" &
PIDS+=($!)

/usr/bin/python3 -c "$SAMPLER_PY" "$OUT" "$INTERVAL" 2>"$OUT/sampler.err" &
PIDS+=($!)

# tcpdump: only if BPF is accessible without sudo (usually it is not: skipped silently).
if (( WANT_TCPDUMP )); then
  if [[ -r /dev/bpf0 ]] && command -v tcpdump >/dev/null 2>&1; then
    tcpdump -i awdl0 -n -s 160 -w "$OUT/awdl0.pcap" >/dev/null 2>"$OUT/tcpdump.err" &
    tpid=$!
    sleep 1
    if kill -0 $tpid 2>/dev/null; then
      PIDS+=($tpid)
      print "tcpdump: capturing to $OUT/awdl0.pcap"
    else
      print "tcpdump: did not start ($(head -1 "$OUT/tcpdump.err" 2>/dev/null)) — skipped"
      rm -f "$OUT/awdl0.pcap"
    fi
  else
    print "tcpdump: /dev/bpf0 is not readable without sudo — skipped" | tee "$OUT/tcpdump.err"
  fi
fi

cleanup() {
  (( CLEANED )) && return
  CLEANED=1
  trap '' INT TERM HUP
  print "\nStopping the recording..."
  local p
  for p in $PIDS; do kill -TERM $p 2>/dev/null; done
  pkill -TERM -P $$ 2>/dev/null
  local n=0
  while (( n < 30 )) && pgrep -P $$ >/dev/null 2>&1; do sleep 0.1; (( n++ )); done
  pkill -KILL -P $$ 2>/dev/null
  ioreg_awdl > "$OUT/ioreg-end.txt"
  [[ -n $AWDLCTL_BIN ]] && "$AWDLCTL_BIN" show > "$OUT/awdlctl-end.txt" 2>&1
  print "end: $(date '+%Y-%m-%d %H:%M:%S')" >> "$OUT/meta.txt"
  for f in logstream.err sampler.err tcpdump.err; do [[ -s $OUT/$f ]] || rm -f "$OUT/$f"; done
  print "Generating the report..."
  summarize "$OUT" || print -u2 "report generation failed; the data is in $OUT"
  print "Directory: $OUT"
  print "Note: review/redact MAC addresses and device names before sharing these files publicly."
}
trap 'STOP=1' INT TERM HUP
# safety net: if the script exits some other way, do not leave orphan processes
trap 'pkill -TERM -P $$ 2>/dev/null' EXIT

print "Recording to $OUT"
print "Make your AirDrop attempts now. Ctrl-C to finish$( (( MINUTES > 0 )) && print " (or it stops by itself after $MINUTES min)")."
print "Key events are shown below:"
SECONDS=0
typeset -F LIMIT=$(( MINUTES * 60 ))
while (( ! STOP )); do
  (( LIMIT > 0 && SECONDS >= LIMIT )) && { print "Limit of $MINUTES min reached."; break; }
  # if log stream dies (e.g. no permission), warn once but keep sampling
  if [[ -s $OUT/logstream.err && -z ${WARNED:-} ]] && grep -qiE "error|denied|must be" "$OUT/logstream.err"; then
    print -u2 "WARNING: log stream: $(head -1 "$OUT/logstream.err")"; WARNED=1
  fi
  sleep 0.5 2>/dev/null || true
done
cleanup
exit 0
