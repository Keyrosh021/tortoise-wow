#!/usr/bin/env python3
# Active-cohort thrash metrics from bot_events.csv BotStatsSnapshot. Usage: cohort_stats.py [N]
import re,collections,sys
LOG="/home/zuppier/tw/server_dev/tortoise-wow-pr79-replay/logs/bot_events.csv"
N=int(sys.argv[1]) if len(sys.argv)>1 else 2500
rows=[ln for ln in open(LOG,encoding='utf-8',errors='replace') if 'BotStatsSnapshot' in ln][-N:]
mv=cmb=n=tgt_none=thrash=cmbthrash=0; apms=[]; ts=collections.Counter()
for ln in rows:
    m=re.search(r'moving=(\d).*?combat=(\d).*?casting=(\d).*?apm=(\d+).*?sinceProgressSec=(\d+).*?target=(\S+).*?travelStatus=(\d)',ln)
    if not m: continue
    n+=1
    moving=int(m.group(1)); combat=int(m.group(2)); apm=int(m.group(4)); tgt=m.group(6); tstat=int(m.group(7))
    mv+=moving; cmb+=combat; apms.append(apm); ts[tstat]+=1
    if tgt=='none': tgt_none+=1
    if apm>=20 and moving==0 and combat==0: thrash+=1
    if combat==1 and moving==0 and apm>=200: cmbthrash+=1
if not n: print("no data"); sys.exit()
apms.sort()
print(f"n={n}  moving={100*mv//n}%  combat={100*cmb//n}%  target=none={100*tgt_none//n}%")
print(f"apm median={apms[n//2]} p90={apms[int(n*0.9)]} max={apms[-1]}")
print(f"IDLE-THRASH(apm>=20,mv0,cmb0)={100*thrash//n}%  COMBAT-THRASH(cmb1,mv0,apm>=200)={100*cmbthrash//n}%")
print("travelStatus 0NONE 1PREP 2READY 3TRAV 4WORK 5COOL 6EXP:",dict(sorted(ts.items())))
