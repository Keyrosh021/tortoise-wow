#!/usr/bin/env python3
"""
bot_deaths_report.py — analyze why bots are dying, from logs/bot_deaths.csv.

The server (Unit::Kill hook) appends one row per playerbot death:
  ts,victim,guid,vlvl,vclass,map,zone,x,y,z,killerType,killerName,kentry,
  klvl,kelite,lvlDelta,spellId,fleeing,attackers,group,dist

Usage: python3 tools/bot_deaths_report.py [N]    # N = only last N deaths (default all)
"""
import os, sys, collections

CSV = os.path.join(os.path.dirname(__file__), "..", "logs", "bot_deaths.csv")
CLS = {1:"War",2:"Pal",3:"Hun",4:"Rog",5:"Pri",7:"Sha",8:"Mag",9:"Lock",11:"Dru"}

def main():
    if not os.path.exists(CSV):
        print("no bot_deaths.csv yet"); return
    rows = [l.rstrip("\n").split(",") for l in open(CSV) if l.strip()]
    rows = [r for r in rows if len(r) >= 21]
    if len(sys.argv) > 1:
        rows = rows[-int(sys.argv[1]):]
    n = len(rows)
    if not n:
        print("no death rows"); return

    by_killer_type = collections.Counter()
    by_creature    = collections.Counter()
    by_vclass      = collections.Counter()
    by_zone        = collections.Counter()
    by_spell       = collections.Counter()
    leveldeltas    = []
    fleeing = outnumbered = grouped = env = 0
    attacker_hist = collections.Counter()

    for r in rows:
        vclass=int(r[4]); zone=r[6]; ktype=r[10]; kname=r[11]
        kentry=int(r[12]); lvlDelta=int(r[15]); spell=int(r[16])
        flee=int(r[17]); atk=int(r[18]); grp=int(r[19])
        by_killer_type[ktype]+=1
        by_vclass[CLS.get(vclass,vclass)]+=1
        by_zone[zone]+=1
        if ktype=="self/environment": env+=1
        else:
            by_creature[f"{kname} (#{kentry}) L{r[13]}{'+' if int(r[14]) else ''}"]+=1
            leveldeltas.append(lvlDelta)
        if spell: by_spell[spell]+=1
        if flee: fleeing+=1
        if atk>1: outnumbered+=1
        attacker_hist[min(atk,6)]+=1
        if grp>1: grouped+=1

    # death-loop split: a few bots dying 20+ times (env/elite loops) inflate every aggregate
    import collections as _c
    byg = _c.Counter(r[2] for r in rows)
    loopers = {g for g, c in byg.items() if c > 20}
    loop_deaths = sum(c for g, c in byg.items() if g in loopers)
    normal_rows = [r for r in rows if r[2] not in loopers]
    print(f"=== BOT DEATHS: {n} total from {len(byg)} bots ===")
    if loopers:
        print(f"  !! {len(loopers)} 'looper' bots (>20 deaths) = {loop_deaths} deaths "
              f"({100*loop_deaths/n:.0f}%) — stuck in death loops, inflate the stats below.")
        worst = byg.most_common(1)[0]
        wr = next(r for r in rows if r[2] == worst[0])
        print(f"     worst: {wr[1]} (guid {worst[0]}) died {worst[1]}x, zone {wr[6]}, killer={wr[10]}")
        # normal-fleet quick line
        nl = [int(r[15]) for r in normal_rows if r[10] in ('creature', 'elite')]
        n1 = sum(1 for r in normal_rows if int(r[18]) == 1)
        if nl:
            print(f"     NORMAL fleet ({len(normal_rows)} deaths): avg killer +{sum(nl)/len(nl):.1f}, "
                  f"{100*n1/len(normal_rows):.0f}% 1v1, "
                  f"{100*sum(1 for r in normal_rows if int(r[17])==1)/len(normal_rows):.1f}% fleeing")
    print()
    print("by killer type:")
    for k,c in by_killer_type.most_common():
        print(f"  {k:18} {c:5}  {100*c/n:5.1f}%")
    print(f"\nfleeing when killed : {fleeing:5}  {100*fleeing/n:5.1f}%   (died running away)")
    print(f"outnumbered (>1 atk): {outnumbered:5}  {100*outnumbered/n:5.1f}%   (multiple attackers)")
    print(f"in a group          : {grouped:5}  {100*grouped/n:5.1f}%")
    print(f"environment/self    : {env:5}  {100*env/n:5.1f}%   (drown/fall/lava/fire)")
    print("\n# attackers at death:")
    for a in sorted(attacker_hist):
        lbl = f"{a}+" if a==6 else str(a)
        print(f"  {lbl:>3} attackers: {attacker_hist[a]:5}  {100*attacker_hist[a]/n:5.1f}%")
    if leveldeltas:
        over = sum(1 for d in leveldeltas if d>=3)
        print(f"\nkiller level - bot level (creature kills, n={len(leveldeltas)}):")
        print(f"  avg {sum(leveldeltas)/len(leveldeltas):+.1f}   killed by mob >=3 lvls higher: {over} ({100*over/len(leveldeltas):.1f}%)")
    print("\ntop killer creatures:")
    for k,c in by_creature.most_common(15):
        print(f"  {c:5}  {k}")
    print("\ntop zones (where bots die):")
    for k,c in by_zone.most_common(12):
        print(f"  {c:5}  zone {k}")
    print("\nvictim class distribution:")
    for k,c in by_vclass.most_common():
        print(f"  {str(k):5} {c:5}  {100*c/n:5.1f}%")
    if by_spell:
        print("\ntop killing spells (spellId):")
        for k,c in by_spell.most_common(10):
            print(f"  {c:5}  spell {k}")

if __name__ == "__main__":
    main()
