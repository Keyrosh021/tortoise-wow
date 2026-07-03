#!/usr/bin/env python3
"""
nearby_census.py — live census of what the bots around YOUR character are doing.

The server writes one line per real player every ~3s to logs/nearby_census.csv:
  time,player,map,total,fighting,moving,idle,dead,casting,looting,avg_lvl,classes
This tool prints the latest census per player, with percentages.

Usage:
  python3 tools/nearby_census.py            # one snapshot (bot list collapsed)
  python3 tools/nearby_census.py --list     # also expand the per-bot list
  python3 tools/nearby_census.py --watch    # refresh every 3s (Ctrl-C to stop)
  python3 tools/nearby_census.py --watch --list   # live + expanded list
"""
import os, sys, time

CSV  = os.path.join(os.path.dirname(__file__), "..", "logs", "nearby_census.csv")
BOTS = os.path.join(os.path.dirname(__file__), "..", "logs", "nearby_bots.csv")

def latest_bot_list():
    """Latest snapshot's per-bot list, grouped by observer.
    nearby_bots.csv rows: ts,observer,bot,level,class,activity,distance"""
    if not os.path.exists(BOTS):
        return {}, None
    lines = open(BOTS).read().splitlines()[-4000:]
    if not lines:
        return {}, None
    last_ts = lines[-1].split(",", 1)[0]
    by_obs = {}
    for ln in lines:
        p = ln.split(",")
        if len(p) < 7 or p[0] != last_ts:
            continue
        by_obs.setdefault(p[1], []).append(
            {"bot": p[2], "lvl": p[3], "cls": p[4], "act": p[5], "dist": p[6]})
    return by_obs, last_ts

def latest_per_player():
    if not os.path.exists(CSV):
        return None
    rows = {}
    with open(CSV) as f:
        for line in f.readlines()[-400:]:          # only need the tail
            p = line.rstrip("\n").split(",", 11)
            if len(p) < 11:
                continue
            rows[p[1]] = p                          # keyed by player -> last wins
    return rows

def bar(pct, width=20):
    n = int(round(pct/100*width))
    return "#"*n + "."*(width-n)

def show():
    rows = latest_per_player()
    os.system("clear") if "--watch" in sys.argv else None
    bot_list, _ = latest_bot_list()
    print("="*70)
    print("  NEARBY BOT CENSUS  (bots within your render/visibility distance)")
    print("="*70)
    if not rows:
        print("  No data yet. Log in with your character near some bots and wait ~3s.")
        print(f"  (reads {os.path.normpath(CSV)})")
        return
    for player, p in rows.items():
        ts, _, mp = p[0], p[1], p[2]
        total = int(p[3]); fighting=int(p[4]); moving=int(p[5]); idle=int(p[6])
        dead=int(p[7]); casting=int(p[8]); looting=int(p[9]); avglvl=p[10]
        # p[11] holds the remainder: "<classes>,rest=..,lowhp=..,lowmana=..,stuck=.."
        remainder = p[11] if len(p) > 11 else ""
        parts = remainder.split(",")
        classes = parts[0]
        # idle breakdown fields (rest=.. lowhp=.. lowmana=.. stuck=..) appended after classes
        brk = {}
        for tok in parts[1:]:
            if "=" in tok:
                k,_,v = tok.partition("=")
                if v.strip().isdigit(): brk[k.strip()] = int(v)
        rng = brk.get('range')
        rngtxt = f", range {rng}yd" if rng else ""
        def pc(n): return 100.0*n/total if total else 0.0
        print(f"\n  {player}  ({mp})   @ {ts}   |   {total} bots nearby, avg lvl {avglvl}{rngtxt}")
        print(f"    fighting  {fighting:>4}  {pc(fighting):5.1f}%  {bar(pc(fighting))}")
        print(f"    moving    {moving:>4}  {pc(moving):5.1f}%  {bar(pc(moving))}")
        print(f"    idle      {idle:>4}  {pc(idle):5.1f}%  {bar(pc(idle))}")
        if brk:
            rest=brk.get('rest',0); lowhp=brk.get('lowhp',0); lowmana=brk.get('lowmana',0); stuck=brk.get('stuck',0)
            print(f"       ├ rest/eat/drink {rest:>4}  {pc(rest):5.1f}%   (benign — regenerating)")
            print(f"       ├ low HP         {lowhp:>4}  {pc(lowhp):5.1f}%")
            print(f"       ├ low mana       {lowmana:>4}  {pc(lowmana):5.1f}%")
            print(f"       └ STUCK (full)   {stuck:>4}  {pc(stuck):5.1f}%   <-- the real problem if high")
        print(f"    dead      {dead:>4}  {pc(dead):5.1f}%  {bar(pc(dead))}")
        print(f"    looting   {looting:>4}  {pc(looting):5.1f}%")
        print(f"    (casting  {casting:>4}  {pc(casting):5.1f}% of the fighting ones)")
        print(f"    classes:  {classes.strip()}")

        # per-bot list (sorted by distance) for this observer — collapsed by default,
        # expand with --list (or press 'l' note). Keeps the live view compact.
        blist = bot_list.get(player)
        if blist:
            show_list = ("--list" in sys.argv) or ("--bots" in sys.argv)
            if show_list:
                blist.sort(key=lambda b: float(b["dist"]))
                print(f"    v bots in range ({len(blist)}), nearest first:")
                for b in blist:
                    print(f"      {b['dist']:>4}yd  L{b['lvl']:<2} {b['cls']:<4} {b['bot']:<14} {b['act']}")
            else:
                print(f"    > bots in range ({len(blist)}) — collapsed; add --list to expand")
    print("="*70)

def main():
    if "--watch" in sys.argv:
        try:
            while True:
                show(); time.sleep(3)
        except KeyboardInterrupt:
            pass
    else:
        show()

if __name__ == "__main__":
    main()
