#!/usr/bin/env python3
# Reconstruct a per-bot behavior STORY from an observe capture, to judge whether bot actions read as
# purposeful human play. Usage: narrate.py <label> [topN]
import sys, subprocess, collections, math, os
SCR=os.path.dirname(__file__)
label=sys.argv[1]; topN=int(sys.argv[2]) if len(sys.argv)>2 else 6

def spell_names(ids):
    if not ids: return {}
    idlist=",".join(str(i) for i in ids)
    out=subprocess.run(["mysql","-h127.0.0.1","-uroot","-pmangos","tw_world","-N","-e",
        f"select entry,name from spell_template where entry in ({idlist})"],capture_output=True,text=True).stdout
    m={}
    for ln in out.splitlines():
        p=ln.split("\t")
        if len(p)>=2: m[int(p[0])]=p[1]
    return m

# load events
ev=collections.defaultdict(list)   # name -> [(t, kind, detail)]
spellids=set()
epath=f"{SCR}/events_{label}.tsv"
for ln in open(epath):
    p=ln.rstrip("\n").split("\t")
    if len(p)<3: continue
    t=float(p[0]); kind=p[1]
    if kind=="MOVE": ev[p[2]].append((t,"move",p[3]+" "+(p[4] if len(p)>4 else "")))
    elif kind=="SWING": ev[p[2]].append((t,"swing","-> "+p[4]))
    elif kind=="CAST":
        sid=int(p[3][5:]); spellids.add(sid); ev[p[2]].append((t,"cast",sid))
    elif kind=="EMOTE": ev[p[2]].append((t,"emote",p[2+1] if len(p)>3 else ""))
    elif kind=="ATTACK_START": ev[p[2]].append((t,"attack",("-> "+p[4]) if len(p)>4 else ""))

# load snapshots for position/target track (PLR only)
snap=collections.defaultdict(list)  # name -> [(t,x,y,lvl,tgtname,mv,hp)]
spath=f"{SCR}/snap_{label}.tsv"
for ln in open(spath):
    p=ln.rstrip("\n").split("\t")
    if len(p)<13 or p[2]!="PLR": continue
    name=p[11]
    if name=="Obseye": continue
    t=float(p[0]); xy=p[6].strip("()").split(","); x=float(xy[0]); y=float(xy[1])
    lvl=int(p[5][3:]); tgt=p[12][1:] if len(p)>12 and p[12].startswith(">") else ""
    mv=int(p[9][2:]); hp=int(p[8][2:])
    snap[name].append((t,x,y,lvl,tgt,mv,hp))

names=spell_names(spellids)
# pick busiest bots by combined event+snapshot count
busy=sorted(set(list(ev)+list(snap)), key=lambda n:-(len(ev.get(n,[]))+len(snap.get(n,[]))))
busy=[n for n in busy if n and n!="Obseye"][:topN]

for name in busy:
    tr=sorted(snap.get(name,[]))
    evs=sorted(ev.get(name,[]))
    lvl=tr[-1][3] if tr else "?"
    # movement summary
    path=sum(math.hypot(tr[i][1]-tr[i-1][1],tr[i][2]-tr[i-1][2]) for i in range(1,len(tr)))
    swings=sum(1 for e in evs if e[1]=="swing"); casts=[e[2] for e in evs if e[1]=="cast"]
    moves=sum(1 for e in evs if e[1]=="move")
    tgts=[s[4] for s in tr if s[4]]
    print(f"\n=== {name} (L{lvl})  pathYd={path:.0f}  swings={swings}  casts={len(casts)}  moveCmds={moves} ===")
    if casts:
        cc=collections.Counter(names.get(s,f"spell{s}") for s in casts)
        print("   spells used:", ", ".join(f"{k}x{v}" for k,v in cc.most_common()))
    if tgts:
        tc=collections.Counter(tgts)
        print("   targeted:", ", ".join(f"{k}x{v}" for k,v in tc.most_common(5)))
    # interleaved timeline (compact)
    tl=[(t,"cast:"+names.get(d,f"spell{d}")) for (t,k,d) in evs if k=="cast"]
    tl+=[(t,"swing "+d) for (t,k,d) in evs if k=="swing"]
    tl+=[(t,"move "+d) for (t,k,d) in evs if k=="move"]
    tl.sort()
    # collapse repeated
    line=[]; last=None; cnt=0
    for t,a in tl:
        key=a.split()[0]
        if key==last: cnt+=1
        else:
            if last: line.append(f"{last}x{cnt}")
            last=key; cnt=1
    if last: line.append(f"{last}x{cnt}")
    print("   timeline:", " | ".join(line[:30]))
