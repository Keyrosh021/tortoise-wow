#!/usr/bin/env python3
# Measure cross-faction intrusion at a captured spot: of the bots seen, how many are the ENEMY faction
# for that zone. Usage: faction_mix.py <label> <expected_team ALLI|HORDE>
import sys, subprocess, collections, os
SCR=os.path.dirname(__file__)
label=sys.argv[1]; expected=sys.argv[2] if len(sys.argv)>2 else "ALLI"

names=set()
for ln in open(f"{SCR}/snap_{label}.tsv"):
    p=ln.rstrip("\n").split("\t")
    if len(p)<13 or p[2]!="PLR": continue
    if p[11] and p[11]!="Obseye": names.add(p[11])
if not names:
    print("no bots seen"); sys.exit()
inlist=",".join("'%s'"%n.replace("'","") for n in names)
out=subprocess.run(["mysql","-h127.0.0.1","-uroot","-pmangos","tw_char","-N","-e",
  f"select name, case when race in (1,3,4,7,10) then 'ALLI' else 'HORDE' end team from characters where name in ({inlist})"],
  capture_output=True,text=True).stdout
team=collections.Counter(); tmap={}
for ln in out.splitlines():
    p=ln.split("\t")
    if len(p)>=2: team[p[1]]+=1; tmap[p[0]]=p[1]
tot=sum(team.values())
enemy = 'HORDE' if expected=='ALLI' else 'ALLI'
print(f"bots at {label} (expected {expected} territory): {tot} resolved")
print(f"  own-faction({expected}): {team.get(expected,0)}  ENEMY({enemy}): {team.get(enemy,0)}  ({100*team.get(enemy,0)//max(tot,1)}% intrusion)")
enemies=[n for n,t in tmap.items() if t==enemy]
if enemies: print("  enemy-faction bots present:", ", ".join(enemies[:12]))
