#!/bin/bash
# Combat-share plateau verdict — run after the recovery curve has had 30+ min undisturbed.
# Computes the post-fix equilibrium and prints the decision per branch.
cd "$(dirname "$0")/.."

SINCE="${1:-$(date -d '30 minutes ago' '+%Y-%m-%d %H:%M')}"
echo "window since: $SINCE"

awk -F, -v since="$SINCE" '$2=="S" && $1 >= since && $4 ~ /^L[12][0-9]$/ {
  v=substr($7,5); a=$8; sub("intend=","",a); tot++;
  if (v=="MELEE"||v=="CAST") c++;
  else if (v=="IDLE") { idle++; ia[a]++ }
  else if (v=="DEAD") d++;
  else if (v=="LOOT") l++;
} END {
  if (!tot) { print "NO DATA in window"; exit 1 }
  printf "combat %.1f%%  idle %.1f%%  dead %.1f%%  loot %.2f%%  (n=%d)\n", c*100/tot, idle*100/tot, d*100/tot, l*100/tot, tot
  print "--- idle composition (>1%) ---"
  for (x in ia) if (ia[x] > tot*0.01) printf "%5.1f%%  %s\n", ia[x]*100/tot, x
  cs = c*100/tot
  print "--- verdict ---"
  if (cs >= 25)      print "PLATEAU IN/NEAR BAND: goal #1 combat share effectively reached; measure XP/hr next."
  else if (cs >= 15) print "PARTIAL RECOVERY: ceiling lifted but short of band; attack the largest idle line above."
  else               print "RECOVERY STALLED: re-trace one chronic-idle bot (same method as Froland) against the largest idle line."
}' logs/supervisor.csv

echo "--- XP pulse (same window; needs bot_events coverage) ---"
grep BotStatsSnapshot logs/bot_events.csv | awk -F'"' -v since="$SINCE" '
$1 >= since {
  split($1, meta, ","); bot=meta[2];
  lvl=0; xp=-1;
  n=split($6, kv, " ");
  for(i=1;i<=n;i++){ if(kv[i]~/^level=/){sub("level=","",kv[i]);lvl=kv[i]+0}
    else if(kv[i]~/^xp=/){sub("xp=","",kv[i]);xp=kv[i]+0} }
  if(xp<0) next;
  if(!(bot in fx)){fx[bot]=xp; fl[bot]=lvl}
  lx[bot]=xp; ll[bot]=lvl;
} END {
  for(b in fx){ dxp=lx[b]-fx[b]; if(ll[b]>fl[b]) dxp=lx[b]+900*(ll[b]-fl[b]);
    if(ll[b]>=10&&ll[b]<25){c++; s+=dxp} }
  if (c) printf "L10-24: bots=%d  total dXP=%d  (divide by window hours for XP/hr; baseline plateau was ~500)\n", c, s
}'
