#!/bin/bash
# n10cycle.sh [seconds]: cycle the link MCS0/20 MHz <-> MCS0/10 MHz on the bench and log every
# move (default 180 s). The GS runs a temporary config: its ladder cut to the bottom rung
# (MCS0/20) and the 10 MHz rung switched on with test values -- go to 10 after 3 s at MCS0/20
# whatever the loss, come back after 5 s clean with no SNR bar, 5 s between tries -- so the GS
# itself moves the link down and up, over and over. Nothing changes on the drone: it follows
# the GS. Both ends stay on the GS's current channel (pinned for the run; the GS menu's file is
# moved aside). The GS config is backed up first and put back at the end, also on Ctrl-C.
GS=${GS:-10.18.0.1}; DRONE=${DRONE:-10.42.0.53}
DUR=${1:-180}
OUT=~/fpv-nack/gs13/n10-$(date +%H%M); mkdir -p "$OUT"; SUM=$OUT/summary.txt
SSH="ssh -o ConnectTimeout=5 -o ControlMaster=auto -o ControlPath=/tmp/n10-%h -o ControlPersist=30m -o ServerAliveInterval=5 -o ServerAliveCountMax=2"
SCP="scp -q -O -o ControlPath=/tmp/n10-%h"
say() { echo "$*" | tee -a "$SUM"; }

# One status line from the GS: session channel op_bw op_mcs drone_linked t_ms
st() {
  $SSH root@$GS 's=$(cat /tmp/mabur-session 2>/dev/null); echo "$s"; tail -n2 "$s/flight.jsonl" 2>/dev/null | head -n1' 2>/dev/null |
  python3 -c '
import sys, json
p = sys.stdin.read().split("\n", 1)
s = p[0].strip().split("/")[-1] or "-"
try:
    o = json.loads(p[1]); L = o["link"]; op = L.get("op") or {}
    d = o.get("drone") or {}
    print(s, L.get("channel") or 0, op.get("bw", 0), op.get("mcs", -1), int(d.get("state") == "linked"), int(o.get("t_ms") or 0))
except Exception:
    print(s, 0, 0, -1, 0, 0)'
}

backed=0
restore() {
  trap - EXIT INT TERM
  [ $backed = 1 ] || exit
  echo "putting the GS back (maburgs restarts)..."
  $SCP root@$GS:/tmp/maburgs.log "$OUT/gs-maburgs.log" 2>/dev/null
  $SCP root@$DRONE:/tmp/mabur.log "$OUT/drone-mabur.log" 2>/dev/null
  $SSH root@$GS 'cp /root/maburgs.toml.n10 /config/maburgs.toml; [ -f /root/maburgs.menu.n10 ] && mv /root/maburgs.menu.n10 /etc/maburgs.menu; sync; /etc/init.d/S96maburgs restart >/dev/null 2>&1 </dev/null'
  [ -n "$SESS" ] && [ "$SESS" != "-" ] && $SCP -r root@$GS:/media/dvr/log/$SESS "$OUT/" 2>/dev/null
  tar czf "$OUT.tgz" -C "$(dirname "$OUT")" "$(basename "$OUT")" && echo "send me: $OUT.tgz"
  exit
}

echo "checking both ends (type each password once if asked)..."
$SSH root@$DRONE true && $SSH root@$GS 'test -f /config/maburgs.toml' ||
  { echo "cannot reach both ends"; exit 1; }
read -r SESS CH BW MCS L TM < <(st)
[ "$CH" -gt 0 ] 2>/dev/null || { echo "the GS reports no link channel: start with video running"; exit 1; }
$SSH root@$GS 'cp /config/maburgs.toml /root/maburgs.toml.n10; if [ -f /etc/maburgs.menu ]; then mv /etc/maburgs.menu /root/maburgs.menu.n10; fi' ||
  { echo "backup failed, nothing changed"; exit 1; }
backed=1; trap restore EXIT INT TERM
say "n10cycle $(date '+%F %H:%M'), channel $CH, $DUR s"

# The test config: every [[link.ladder]] and any [link.narrow10] block dropped, the channel
# pinned, then one MCS0/20 rung (the bundle's bottom-rung FEC) and the 10 MHz test values.
DL0=$($SSH root@$DRONE 'wc -l < /tmp/mabur.log' 2>/dev/null || echo 0)
$SSH root@$GS "f=/config/maburgs.toml
awk '/^\\[/ { skip = (\$0 ~ /^\\[\\[link\\.ladder\\]\\]/ || \$0 ~ /^\\[link\\.narrow10\\]/) } !skip' /root/maburgs.toml.n10 > \$f.new &&
sed -i 's/^channel *=.*/channel  = $CH/' \$f.new &&
printf '\n[[link.ladder]]\nmcs = 0\nbw  = 20\noverhead_base = 1.0\noverhead_enh  = 0.75\n\n[link.narrow10]\nenable = true\nenter_ms = 3000\nenter_post_loss_pct = 0.0\nclean_ms = 5000\npromote_margin_db = -10.0\ncooldown_ms = 5000\n' >> \$f.new &&
cat \$f.new > \$f && rm -f \$f.new && sync && /etc/init.d/S96maburgs restart >/dev/null 2>&1 </dev/null"
sleep 5
e=$($SSH root@$GS "grep -m1 '^error' /tmp/maburgs.log" 2>/dev/null)
[ -n "$e" ] && { say "GS refused the test config: $e"; exit 1; }
TMIN=$(st | awk '{print $6}')

echo "waiting for video at MCS0/20..."
t0=$(date +%s)
while :; do
  read -r SESS C BW MCS L TM < <(st)
  [ "$TM" -gt "$TMIN" ] && [ "$L" = 1 ] && break
  [ $(( $(date +%s) - t0 )) -ge 90 ] && { say "no link within 90 s of the restart: stopping"; exit 1; }
  sleep 1
done
say "linked; cycling for $DUR s (each line is a change of the link's width)"

# Time is shared out by wall clock between polls (ms), so a slow poll still counts right.
ms() { echo $(( $(date +%s%N) / 1000000 )); }
t0=$(ms); last=$t0; prev=""; n10=0; n20=0; nd=0; moves=0
while [ $(( $(ms) - t0 )) -lt $(( DUR * 1000 )) ]; do
  read -r SESS C BW MCS L TM < <(st)
  t=$(ms); dt=$(( t - last )); last=$t
  if [ "$BW" = 10 ] || [ "$BW" = 20 ]; then
    [ "$BW" = 10 ] && n10=$((n10 + dt)) || n20=$((n20 + dt))
    [ "$L" = 1 ] || nd=$((nd + dt))
    if [ -n "$prev" ] && [ "$BW" != "$prev" ]; then
      moves=$((moves + 1)); say "  $(( (t - t0) / 1000 )) s: ${prev} -> ${BW} MHz"
    fi
    prev=$BW
  fi
  sleep 1
done
n10=$((n10 / 1000)); n20=$((n20 / 1000)); nd=$((nd / 1000))

g=$($SSH root@$GS "grep 'maburgs narrow10:' /tmp/maburgs.log" 2>/dev/null)
d=$($SSH root@$DRONE "tail -n +$(( DL0 + 1 )) /tmp/mabur.log | grep -E 'width [0-9]+ -> 10 MHz|width 10 -> |10 MHz rescue'" 2>/dev/null)
cnt() { echo "$g" | grep -c "narrow10: $1" ; }
say "GS: $(cnt enter) moves to 10 ordered, $(cnt on) made it, $(cnt cancel) cancelled, $(cnt rescue) rescued back to 20; $(cnt leave) moves back to 20 ordered, $(cnt off) made it, $(cnt stay) stayed at 10"
say "drone: $(echo "$d" | grep -c -- '-> 10 MHz') switches to 10 MHz (times: $(echo "$d" | grep -- '-> 10 MHz' | grep -oE 'in [0-9.]+ ms' | head -8 | sed 's/^in //' | tr '\n' ' ')), $(echo "$d" | grep -c 'width 10 -> ') back from 10 (times: $(echo "$d" | grep 'width 10 -> ' | grep -oE 'in [0-9.]+ ms' | head -8 | sed 's/^in //' | tr '\n' ' ')), $(echo "$d" | grep -c 'MHz rescue: no RCF') drone rescues"
say "about ${n10} s at 10 MHz, ${n20} s at 20 MHz, ${nd} s without the drone linked, ${moves} width changes seen"
echo "$g" | tail -20 > "$OUT/gs-narrow10.txt"
