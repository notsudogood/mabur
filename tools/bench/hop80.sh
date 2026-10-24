#!/bin/bash
# hop80.sh [channels...]: the 80 MHz hop tour, drone on the bench cable, laptop on
# the GS hotspot. The tour starts and ends on its LAST channel; every other entry is
# one hop. Default: 36 132 36 132 (four hops across the old blocker). Each step pins
# the GS to the next channel and restarts maburgs; the GS finds the drone where it is
# and orders one hop there. The drone's width stepping is off, so every hop is an
# 80 MHz hop. Both ends' settings are backed up first and put back at the end, also
# on Ctrl-C. All channels must sit on one side of the 40 MHz pair grid (docs/bw40.md):
#   lower: 36 44 52 60 100 108 116 124 132 140 149 157
#   upper: 40 48 56 64 104 112 120 128 136 144 153 161
GS=${GS:-10.18.0.1}; DRONE=${DRONE:-10.42.0.53}
CHS=${*:-36 132 36 132}
LOWER=" 36 44 52 60 100 108 116 124 132 140 149 157 "
UPPER=" 40 48 56 64 104 112 120 128 136 144 153 161 "
side=""
for ch in $CHS; do
  case "$LOWER" in *" $ch "*) s=lower ;; *) case "$UPPER" in *" $ch "*) s=upper ;; *) s=bad ;; esac ;; esac
  [ "$s" = bad ] && { echo "channel $ch is not one this tour can use"; exit 1; }
  [ -n "$side" ] && [ "$s" != "$side" ] && {
    echo "channels must all be on one side of the 40 MHz pair grid: $ch is $s, the others $side"
    echo "  lower:$LOWER"; echo "  upper:$UPPER"; exit 1; }
  side=$s
done
SET=$(echo $CHS | tr ' ' '\n' | sort -nu | paste -sd, - | sed 's/,/, /g')
START=$(echo $CHS | awk '{print $NF}')
HOLD=${HOLD:-60}; WAIT_LINK=${WAIT_LINK:-90}; WAIT_TOP=${WAIT_TOP:-90}
OUT=~/fpv-nack/gs13/hop80-$(date +%H%M); mkdir -p "$OUT"; SUM=$OUT/summary.txt
SSH="ssh -o ConnectTimeout=5 -o ControlMaster=auto -o ControlPath=/tmp/hop80-%h -o ControlPersist=30m -o ServerAliveInterval=5 -o ServerAliveCountMax=2"
say() { echo "$*" | tee -a "$SUM"; }

# One status line from the GS: session channel linked rung bw mcs t_ms (GS uptime clock)
st() {
  $SSH root@$GS 's=$(cat /tmp/mabur-session 2>/dev/null); echo "$s"; tail -n2 "$s/flight.jsonl" 2>/dev/null | head -n1' 2>/dev/null |
  python3 -c '
import sys, json
p = sys.stdin.read().split("\n", 1)
s = p[0].strip().split("/")[-1] or "-"
try:
    o = json.loads(p[1]); L = o["link"]; r = (L.get("ctl") or {}).get("rung") or {}
    d = o.get("drone") or {}
    print(s, L.get("channel") or 0, int(d.get("state") == "linked"), r.get("idx", -1), r.get("bw", 0), r.get("mcs", 0), int(o.get("t_ms") or 0))
except Exception:
    print(s, 0, 0, -1, 0, 0, 0)'
}

# A maburgs restart keeps its session folder (only a reboot starts a new one), so
# a record is judged fresh by its t_ms: newer than the last record seen after the
# old process was stopped (gs_pin waits for that) can only come from the new one.
newest() { st | awk '{print $7}'; }

# Restart maburgs with the GS pinned to $1; stop the tour if it refuses its config.
gs_pin() {
  local e
  $SSH root@$GS "sh /root/setch.sh '$SET' $1 >/dev/null && sync && /etc/init.d/S96maburgs restart >/dev/null 2>&1 </dev/null"
  sleep 4
  e=$($SSH root@$GS "grep -m1 '^error' /tmp/maburgs.log" 2>/dev/null)
  [ -n "$e" ] && { say "GS refused its config: $e"; exit 1; }
}

# Wait for the link on $1 in a record stamped after $2 (GS ms), then for /80, then hold.
watch_ch() {
  local ch=$1 tmin=$2 t0 now s c l r bw m tm tl="" t80="" n=0 n80=0 lo=99 moved=0 end
  t0=$(date +%s)
  while :; do
    now=$(( $(date +%s) - t0 )); read -r s c l r bw m tm < <(st)
    if [ -z "$tl" ] && [ "$tm" -gt "$tmin" ] && [ "$c" = "$ch" ] && [ "$l" = 1 ]; then tl=$now; fi
    if [ -n "$tl" ] && [ "$bw" = 80 ]; then t80=$now; break; fi
    if [ -z "$tl" ] && [ "$now" -ge "$WAIT_LINK" ]; then break; fi
    if [ -n "$tl" ] && [ "$now" -ge $(( tl + WAIT_TOP )) ]; then break; fi
    sleep 2
  done
  if [ -n "$tl" ]; then
    end=$(( $(date +%s) + HOLD ))
    while [ "$(date +%s)" -lt "$end" ]; do
      read -r s c l r bw m tm < <(st); n=$((n + 1))
      [ "$bw" = 80 ] && n80=$((n80 + 1))
      [ "$l" = 1 ] && [ "$r" -ge 0 ] && [ "$r" -lt "$lo" ] && lo=$r
      [ "$c" != 0 ] && [ "$c" != "$ch" ] && moved=1
      sleep 2
    done
  fi
  SESS=$s
  if [ -z "$tl" ]; then say "ch $ch: FAILED, no link on $ch within $WAIT_LINK s"; return 1; fi
  if [ -z "$t80" ]; then say "ch $ch: link in $tl s, but never reached /80 within $WAIT_TOP s"
  else say "ch $ch: link in $tl s, /80 in $t80 s, then /80 for $(( 100 * n80 / (n > 0 ? n : 1) ))% of $HOLD s (lowest rung $([ $lo = 99 ] && echo - || echo $lo))$([ $moved = 1 ] && echo ', LEFT the channel during the hold')"; fi
  return 0
}

logs() {  # $1 = file tag
  $SSH root@$DRONE "grep -E 'retune .*->' /tmp/mabur.log | tail -1" 2>/dev/null | sed 's/^/    drone: /' | tee -a "$SUM"
  $SSH root@$GS "grep -E 'maburgs channel: (drone found|relocate)|maburgs hop:' /tmp/maburgs.log | tail -3" 2>/dev/null | sed 's/^/    gs:    /' | tee -a "$SUM"
  scp -q -O -o ControlPath=/tmp/hop80-%h root@$GS:/tmp/maburgs.log "$OUT/gs-$1.log" 2>/dev/null
  echo "$SESS" >> "$OUT/sessions"
}

backed=0
restore() {
  trap - EXIT INT TERM
  [ $backed = 1 ] || exit
  echo "putting both ends back (GS restarts maburgs, drone reboots)..."
  scp -q -O -o ControlPath=/tmp/hop80-%h root@$DRONE:/tmp/mabur.log "$OUT/drone-mabur.log" 2>/dev/null
  scp -q -O -o ControlPath=/tmp/hop80-%h root@$GS:/tmp/maburgs.log "$OUT/gs-last.log" 2>/dev/null
  $SSH root@$GS 'cp /root/maburgs.toml.hop80 /config/maburgs.toml; [ -f /root/maburgs.menu.hop80 ] && mv /root/maburgs.menu.hop80 /etc/maburgs.menu; sync; /etc/init.d/S96maburgs restart >/dev/null 2>&1 </dev/null'
  $SSH root@$DRONE 'cp /root/mabur.toml.hop80 /etc/mabur.toml; cp /root/mabur.channel.hop80 /etc/mabur.channel; sync; reboot' 2>/dev/null
  for s in $(sort -u "$OUT/sessions" 2>/dev/null); do
    [ "$s" = "-" ] || scp -q -O -r -o ControlPath=/tmp/hop80-%h root@$GS:/media/dvr/log/$s "$OUT/" 2>/dev/null
  done
  tar czf "$OUT.tgz" -C "$(dirname "$OUT")" "$(basename "$OUT")" && echo "send me: $OUT.tgz"
  exit
}

echo "checking both ends (type each password once if asked)..."
$SSH root@$DRONE 'test -f /root/setch.sh' && $SSH root@$GS 'test -f /root/setch.sh' ||
  { echo "cannot reach both ends, or setch.sh is missing (test doc step 1)"; exit 1; }
$SSH root@$DRONE 'cp /etc/mabur.toml /root/mabur.toml.hop80; cp /etc/mabur.channel /root/mabur.channel.hop80' &&
$SSH root@$GS 'cp /config/maburgs.toml /root/maburgs.toml.hop80; if [ -f /etc/maburgs.menu ]; then mv /etc/maburgs.menu /root/maburgs.menu.hop80; fi' ||
  { echo "backup failed, nothing changed"; exit 1; }
backed=1; trap restore EXIT INT TERM
say "hop80 $(date '+%F %H:%M'), start $START, hops: $CHS, channel set [$SET], hold $HOLD s"

echo "setting up: drone width stepping off + channel set, then it reboots..."
$SSH root@$DRONE "sed -i 's/^width_step *= *true/width_step = false/' /etc/mabur.toml; sh /root/setch.sh '$SET' $START >/dev/null; sync; reboot" 2>/dev/null
ssh -O exit -o ControlPath=/tmp/hop80-%h root@$DRONE 2>/dev/null
gs_pin "$START"
tmin=$(newest)
sleep 16
for i in $(seq 60); do ping -c1 -W1 "$DRONE" >/dev/null 2>&1 && break; sleep 2; done
sleep 15
e=$($SSH root@$DRONE "pidof maburd >/dev/null || grep -m1 '^error' /tmp/mabur.log || echo 'maburd is not running'" 2>/dev/null)
[ -n "$e" ] && { say "drone: $e"; exit 1; }
echo "waiting for the start link on $START..."
watch_ch "$START" "$tmin" || { say "no start link on $START: stopping"; exit 1; }
logs start

fails=0
for ch in $CHS; do
  echo "hop to $ch..."
  gs_pin "$ch"
  tmin=$(newest)
  if watch_ch "$ch" "$tmin"; then fails=0; else fails=$((fails + 1)); fi
  logs "$ch"
  [ $fails -ge 2 ] && { say "two failures in a row: stopping"; break; }
done
