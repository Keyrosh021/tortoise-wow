#!/usr/bin/env python3
# Turn snapshot tracks into per-bot behavior diagnoses.
import sys, math, glob, os, collections

SCR=os.path.dirname(__file__)

# expected level band per zone-label (rough classic leveling bands)
ZONE_BAND={"elwynn":(1,12),"westfall":(9,18),"redridge":(15,26),"barrens":(9,26),
           "durotar":(1,12),"tirisfal":(1,12),"stormwind":(1,60)}

def load(label):
    rows=[]
    path=f"{SCR}/snap_{label}.tsv"
    if not os.path.exists(path): return rows
    for ln in open(path):
        p=ln.rstrip("\n").split("\t")
        if len(p)<13: continue
        t=float(p[0]); kind=p[2]
        guid=int(p[3][4:]); entry=int(p[4][5:]); lvl=int(p[5][3:])
        xy=p[6].strip("()").split(","); x=float(xy[0]); y=float(xy[1])
        d=float(p[7][1:]); hp=int(p[8][2:]); mv=int(p[9][2:]); tgt=int(p[10][3:])
        name=p[11]
        rows.append(dict(t=t,kind=kind,guid=guid,entry=entry,lvl=lvl,x=x,y=y,d=d,hp=hp,mv=mv,tgt=tgt,name=name))
    return rows

def load_moves(label):
    """Per-name MOVE event list from events file."""
    mv=collections.defaultdict(list)
    path=f"{SCR}/events_{label}.tsv"
    if not os.path.exists(path): return mv
    for ln in open(path):
        p=ln.rstrip("\n").split("\t")
        if len(p)>=3 and p[1]=="MOVE":
            mv[p[2]].append((float(p[0]),p[3]))
    return mv

def analyze(label):
    rows=load(label)
    if not rows: return None
    bots=collections.defaultdict(list)
    for r in rows:
        if r["kind"]=="PLR" and r["name"]!="Obseye": bots[r["guid"]].append(r)
    npcs=set(r["guid"] for r in rows if r["kind"]=="NPC")
    moves=load_moves(label)
    band=ZONE_BAND.get(label,(1,60))
    diag=collections.Counter()
    detail=[]
    tmax=max(r["t"] for r in rows)
    for g,tr in bots.items():
        tr.sort(key=lambda r:r["t"])
        if len(tr)<2: continue
        name=tr[-1]["name"]; lvl=tr[-1]["lvl"]
        # movement metrics
        path=sum(math.hypot(tr[i]["x"]-tr[i-1]["x"], tr[i]["y"]-tr[i-1]["y"]) for i in range(1,len(tr)))
        net=math.hypot(tr[-1]["x"]-tr[0]["x"], tr[-1]["y"]-tr[0]["y"])
        movingfrac=sum(1 for r in tr if r["mv"])/len(tr)
        # movement COMMANDS (splines issued) — the true "is it pathing" signal
        mymoves=moves.get(name,[])
        nmove=sum(1 for _,d in mymoves if d.startswith("to"))
        nstop=sum(1 for _,d in mymoves if d=="stop")
        # target metrics: had a target that is an NPC (real grind) vs self/player/none
        had_npc_tgt=any(r["tgt"] in npcs for r in tr)
        self_tgt=any(r["tgt"]==g for r in tr)
        no_tgt=all(r["tgt"]==0 for r in tr)
        avgd=sum(r["d"] for r in tr)/len(tr)
        stacked=avgd<4.0
        # classify (movement judged by spline commands, not just position deltas)
        totalmove=path+sum(1 for _,d in mymoves if d.startswith("to"))  # >0 if any pathing
        tags=[]
        if lvl<band[0]-2 or lvl>band[1]+3: tags.append("LVLMISMATCH")
        if nmove==0 and path<3: tags.append("FROZEN")             # never issued a move, never displaced
        elif nmove>=3 and net<10: tags.append("OSCILLATE")        # many moves, no net progress
        if stacked and not had_npc_tgt: tags.append("STACKED_IDLE")
        if self_tgt: tags.append("SELFTARGET")
        if had_npc_tgt: tags.append("GRINDING")
        elif nmove==0 and no_tgt: tags.append("IDLE_NOACTION")
        for t in tags: diag[t]+=1
        detail.append((g,name,lvl,round(path),round(net),nmove,nstop,
                       "NPCtgt" if had_npc_tgt else ("self" if self_tgt else ("none" if no_tgt else "plr")),
                       round(avgd),",".join(tags)))
    return dict(label=label,nbots=len(bots),nnpc=len(npcs),tmax=round(tmax),diag=diag,detail=detail,band=band)

def report(label):
    a=analyze(label)
    if not a:
        print(f"[{label}] no data"); return
    print(f"\n===== {label}  (band L{a['band'][0]}-{a['band'][1]}, {a['tmax']}s) =====")
    print(f"  bots seen: {a['nbots']}   NPCs seen: {a['nnpc']}")
    tot=a["nbots"] or 1
    for k,v in a["diag"].most_common():
        print(f"    {k:16s} {v:3d}  ({100*v//tot}%)")
    # worst offenders
    print("  --- per-bot (lvl, pathYd, netYd, #moves, #stops, target, avgDistToMe, tags) ---")
    for g,name,lvl,path,net,nmove,nstop,tgt,avgd,tags in sorted(a["detail"],key=lambda r:-(r[5]))[:18]:
        print(f"    {name:14s} L{lvl:<2} path{path:<4} net{net:<4} mov{nmove:<3} stop{nstop:<3} {tgt:6s} d{avgd:<4} {tags}")

if __name__=="__main__":
    labels=sys.argv[1:] or ["elwynn","westfall","redridge","barrens","durotar","tirisfal","stormwind"]
    for l in labels: report(l)
