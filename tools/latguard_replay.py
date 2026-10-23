#!/usr/bin/env python3
"""Replay the GS promote latency guard (link.promote_lat_guard_ms) on a session.

usage: latguard_replay.py <session>/au.log <session>/ctl.log <guard_ms>

For each promote in ctl.log: would the guard have fired, when, and what
actually happened next. Same rule as LadderController block 4a: probation
(3 s) after a promote, 300 ms blank, median transit over the last 200 ms of
AUs (at least 3) at or over guard_ms for 100 ms, on 50 ms ticks. Transit is
t_first - pts - enc above the session's best, from au.log; the GS's own
PtsAnchor floor differs slightly (docs/link-adaptation.md).
"""
import sys,statistics
au, ctl, G = sys.argv[1], sys.argv[2], float(sys.argv[3])
pts=[]
for l in open(au):
    if l.startswith('#'): continue
    f=l.split()
    if len(f)<12: continue
    t_first=int(f[7]); pts_us=int(f[1]); enc=int(f[9]); t_us=int(f[0])
    if t_first: pts.append((t_us/1000.0, t_first-pts_us-enc))
floor=None; out=[]
for t,raw in pts:
    floor=raw if floor is None or raw<floor else floor
    out.append((t,(raw-floor)/1000.0))
ev=[]
for l in open(ctl):
    if l.startswith('E '):
        f=l.split(); ev.append((int(f[1]),int(f[2]),int(f[3]),f[4]))
def p50(t):
    xs=[v for tt,v in out if t-200<tt<=t]
    return statistics.median(xs) if len(xs)>=3 else float('nan')
for i,(t,fr,to,why) in enumerate(ev):
    if to<=fr: continue
    nxt=next((e for e in ev[i+1:]), None)
    end=min(t+3000, nxt[0] if nxt else t+3000)
    since=None; fired=None; peak=0
    tt=t+300
    while tt<end:
        v=p50(tt)
        if v==v: peak=max(peak,v)
        if v==v and v>=G:
            since=tt if since is None else since
            if tt-since>=100: fired=tt; break
        else: since=None
        tt+=50
    nx="%s %s +%d"%(nxt[3], "%d->%d"%(nxt[1],nxt[2]), nxt[0]-t) if nxt else '-'
    print("promote %d->%d at %d: %s (peak p50 %.0f ms in probation); next event: %s"%(fr,to,t, "FIRES +%d"%(fired-t) if fired else "quiet", peak, nx))
