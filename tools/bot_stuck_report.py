#!/usr/bin/env python3
"""
bot_stuck_report.py — analyze why bots are idle/stuck, from the fleet diagnostics.

Reads:
  logs/bot_fleet.csv  — one aggregate row / 60s:
     ts,n,avglvl,combat=,moving=,rest=,travel_notmoving=,work=,cooldown=,
     expired=,prepare=,no_goal=,idle_other=,dead=,lowhp=
  logs/bot_diag.csv   — one row per bot / 60s:
     ts,name,guid,lvl,cls,map,zone,alive,combat,moving,sit,hpPct,manaPct,
     cat,status,moveRetry,extRetry,tdist

Usage: python3 tools/bot_stuck_report.py
"""
import os, collections

HERE = os.path.dirname(__file__)
FLEET = os.path.join(HERE, "..", "logs", "bot_fleet.csv")
DIAG  = os.path.join(HERE, "..", "logs", "bot_diag.csv")
CLS = {1:"War",2:"Pal",3:"Hun",4:"Rog",5:"Pri",7:"Sha",8:"Mag",9:"Lock",11:"Dru"}

def kv(row):
    d={}
    for tok in row:
        if "=" in tok:
            k,_,v=tok.partition("=")
            if v.strip().lstrip("-").isdigit(): d[k.strip()]=int(v)
    return d

def fleet_summary():
    if not os.path.exists(FLEET):
        print("no bot_fleet.csv yet"); return
    rows=[l.rstrip("\n").split(",") for l in open(FLEET) if l.strip()]
    if not rows: return
    cats=['combat','moving','rest','travel_notmoving','work','cooldown','expired','prepare','no_goal','idle_other','dead','lowhp']
    tot=collections.Counter(); samples=0; nsum=0
    for r in rows[-30:]:                      # last 30 minutes
        d=kv(r)
        if 'dead' not in d: continue
        try: nsum+=int(r[1])
        except: pass
        for c in cats: tot[c]+=d.get(c,0)
        samples+=1
    if not samples: return
    avgn=nsum/samples
    print(f"=== FLEET COMPOSITION (avg over last {samples} snapshots, ~{avgn:.0f} bots) ===")
    for c in cats:
        avg=tot[c]/samples
        print(f"  {c:18} {avg:7.0f}  {100*avg/avgn:5.1f}%")
    alive_idle = sum(tot[c] for c in ['travel_notmoving','work','cooldown','expired','prepare','no_goal','idle_other'])/samples
    print(f"\n  -> alive & not doing anything visible: {alive_idle:.0f}  ({100*alive_idle/avgn:.1f}%)")
    busy = (tot['combat']+tot['moving'])/samples
    print(f"  -> actively busy (combat+moving):      {busy:.0f}  ({100*busy/avgn:.1f}%)")
    # success metric: of kill-quest bots, how many are fighting their actual quest target?
    hq = sum(kv(r).get('has_killquest',0) for r in rows[-samples:]) / samples
    fq = sum(kv(r).get('fight_questtarget',0) for r in rows[-samples:]) / samples
    if hq:
        print(f"  -> SUCCESS METRIC: {fq:.0f}/{hq:.0f} kill-quest bots fighting their TARGET creature ({100*fq/hq:.1f}%)")

def diag_detail():
    if not os.path.exists(DIAG): return
    rows=[l.rstrip("\n").split(",") for l in open(DIAG) if l.strip()]
    rows=[r for r in rows if len(r)>=18]
    if not rows: return
    # use only the most recent timestamp block
    last_ts=rows[-1][0]
    recent=[r for r in rows if r[0]==last_ts]
    print(f"\n=== LATEST PER-BOT SNAPSHOT @ {last_ts} ({len(recent)} bots) ===")
    cat=collections.Counter(); byzone_stuck=collections.Counter()
    stuck_by_class=collections.Counter(); retry_stuck=0
    movenotmove_examples=[]
    for r in recent:
        c=r[13]; cat[c]+=1
        if c in ('travel_notmoving','no_goal','expired','idle_other'):
            byzone_stuck[r[6]]+=1
            stuck_by_class[CLS.get(int(r[4]),r[4])]+=1
            if c=='travel_notmoving' and len(movenotmove_examples)<8:
                # status TRAVEL but not moving: name, zone, dist-to-target, moveRetry
                movenotmove_examples.append((r[1],r[6],r[17],r[15]))
            if int(r[15])>0: retry_stuck+=1
    for c,n in cat.most_common():
        print(f"  {c:18} {n:5}")
    if byzone_stuck:
        print("\n  stuck bots by zone (top 10):")
        for z,n in byzone_stuck.most_common(10):
            print(f"     zone {z:>5}: {n}")
        print("\n  stuck bots by class:")
        for cl,n in stuck_by_class.most_common():
            print(f"     {str(cl):5}: {n}")
    if movenotmove_examples:
        print("\n  examples of 'travel_notmoving' (status=TRAVEL but standing still):")
        print("     name / zone / dist-to-target / moveRetry")
        for nm,z,d,mr in movenotmove_examples:
            print(f"     {nm:14} zone {z:>5}  dist={d:>6}  moveRetry={mr}")
        print(f"  (of stuck bots, {retry_stuck} have moveRetry>0 = travel tried & failed to path)")

def dwell_analysis():
    """Track each bot across snapshots: how long do bots stay stuck in 'cooldown'
    (validates the up-to-5-min NullTravelDestination lock) vs actually doing things."""
    if not os.path.exists(DIAG): return
    rows=[l.rstrip("\n").split(",") for l in open(DIAG) if l.strip()]
    rows=[r for r in rows if len(r)>=18]
    if len(rows)<2: return
    # per-bot ordered list of (ts, cat)
    bybot=collections.defaultdict(list)
    order=[]
    for r in rows:
        if r[0] not in order: order.append(r[0])
    tsidx={t:i for i,t in enumerate(order)}
    for r in rows:
        bybot[r[2]].append((tsidx[r[0]], r[13]))
    n_snap=len(order)
    if n_snap<3:
        print(f"\n(only {n_snap} snapshots — need a few minutes for dwell analysis)"); return
    # max consecutive cooldown streak per bot (1 streak step ~= 1 min)
    streaks=[]; frac_cooldown=[]; frac_busy=[]
    STUCK={'cooldown','travel_notmoving','expired','idle_other','no_goal'}
    BUSY={'combat','moving'}
    for guid,seq in bybot.items():
        seq.sort()
        cats=[c for _,c in seq]
        if len(cats)<3: continue
        # longest run of 'cooldown'
        best=cur=0
        for c in cats:
            cur=cur+1 if c=='cooldown' else 0
            best=max(best,cur)
        streaks.append(best)
        frac_cooldown.append(sum(1 for c in cats if c=='cooldown')/len(cats))
        frac_busy.append(sum(1 for c in cats if c in BUSY)/len(cats))
    if not streaks: return
    streaks.sort(); frac_cooldown.sort(); frac_busy.sort()
    def pct(a,p): return a[int(len(a)*p)] if a else 0
    print(f"\n=== DWELL ANALYSIS ({len(streaks)} bots tracked across {n_snap} snapshots, ~1min each) ===")
    print(f"  longest continuous COOLDOWN streak per bot (~minutes stuck):")
    print(f"     median {pct(streaks,0.5)}   p90 {pct(streaks,0.9)}   max {streaks[-1]}")
    print(f"  fraction of time a bot spends in COOLDOWN:")
    print(f"     median {pct(frac_cooldown,0.5)*100:.0f}%   p90 {pct(frac_cooldown,0.9)*100:.0f}%")
    print(f"  fraction of time a bot spends BUSY (combat+moving):")
    print(f"     median {pct(frac_busy,0.5)*100:.0f}%   p90 {pct(frac_busy,0.9)*100:.0f}%")
    over3=sum(1 for s in streaks if s>=3)
    print(f"  bots stuck in cooldown >=3 consecutive min: {over3} ({100*over3/len(streaks):.0f}%)")

if __name__ == "__main__":
    fleet_summary()
    diag_detail()
    dwell_analysis()
