#!/usr/bin/env python3
"""
bot_learning_report.py — read out what the fleet LEARNED this run.

The server records one row per fight in `ai_playerbot_combat_sample` (tw_world),
each tagged with a reward (fitness) and an A/B `cohort`. Cohorts run different
parameter values (see BotExperiment in PlayerbotAIConfig.h). This tool digests
that table so you walk away with something every run:

  * overall fight quality (avg reward, survival, fight length)
  * per-class diagnostics (which classes fight well / badly)
  * THE EXPERIMENT RESULT: avg reward by cohort -> which parameter value won

Usage:
    python3 tools/bot_learning_report.py [--since-id N] [--save]

DB creds default to the project's local MySQL (root/mangos, tw_world).
"""
import argparse, subprocess, datetime, os, sys

DB_HOST = "127.0.0.1"; DB_USER = "root"; DB_PASS = "mangos"; DB_NAME = "tw_world"

# Keep in sync with BotExperiment::PolicyStrength in PlayerbotAIConfig.h
EXPERIMENT_NAME = "learned-policy strength (does acting on the learning help?)"
COHORT_LABELS = {0: "policy OFF (control)", 1: "policy x0.6", 2: "policy x1.0 (full)"}

CLASS_NAMES = {1:"Warrior",2:"Paladin",3:"Hunter",4:"Rogue",5:"Priest",
               7:"Shaman",8:"Mage",9:"Warlock",11:"Druid"}

def q(sql):
    out = subprocess.run(["mysql","-h",DB_HOST,"-u",DB_USER,f"-p{DB_PASS}",DB_NAME,
                          "-N","-B","-e",sql], capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(out.stderr); sys.exit(1)
    rows=[]
    for line in out.stdout.splitlines():
        if not line.strip(): continue
        rows.append(line.split("\t"))
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since-id", type=int, default=0,
                    help="only fights with id > N (use to scope to the current run)")
    ap.add_argument("--save", action="store_true", help="also save a timestamped report file")
    args = ap.parse_args()
    where = f"WHERE id > {args.since_id}" if args.since_id else ""

    L = []
    def p(s=""): L.append(s)

    total = q(f"SELECT COUNT(*),ROUND(AVG(reward),1),ROUND(AVG(ended_alive)*100),"
              f"ROUND(AVG(duration_ms)/1000,1),ROUND(AVG(min_health_pct)) "
              f"FROM ai_playerbot_combat_sample {where};")[0]
    n = int(total[0])
    p("="*64)
    p(f"  BOT LEARNING REPORT  —  {datetime.datetime.now():%Y-%m-%d %H:%M:%S}")
    p("="*64)
    if n == 0:
        p("No fights recorded yet. Let the server run with bots in combat.");
        print("\n".join(L)); return
    p(f"Fights: {n}   avg reward: {total[1]}   survival: {total[2]}%   "
      f"avg fight: {total[3]}s   avg min-HP: {total[4]}%")
    p("")

    p("PER-CLASS (worst reward first — where to focus):")
    p(f"  {'class':<9}{'fights':>7}{'avg_reward':>12}{'survival':>10}{'avg_secs':>9}")
    for r in q(f"SELECT class,COUNT(*),ROUND(AVG(reward),1),ROUND(AVG(ended_alive)*100),"
               f"ROUND(AVG(duration_ms)/1000,1) FROM ai_playerbot_combat_sample {where} "
               f"GROUP BY class ORDER BY AVG(reward) ASC;"):
        cls = CLASS_NAMES.get(int(r[0]), f"cls{r[0]}")
        p(f"  {cls:<9}{r[1]:>7}{r[2]:>12}{r[3]+'%':>10}{r[4]+'s':>9}")
    p("")

    p(f"EXPERIMENT — {EXPERIMENT_NAME}:")
    p(f"  {'cohort':<26}{'fights':>7}{'avg_reward':>12}{'survival':>10}{'lift_vs_ctrl':>13}")
    rows = q(f"SELECT cohort,COUNT(*),ROUND(AVG(reward),2),ROUND(AVG(ended_alive)*100,1),"
             f"ROUND(AVG(reward),4) FROM ai_playerbot_combat_sample {where} GROUP BY cohort;")
    # cohort 0 is the CONTROL (no learned policy). Compute % lift of each cohort vs it.
    data = {int(r[0]): {"fights": r[1], "reward": float(r[2]), "surv": float(r[3]),
                        "reward_raw": float(r[4])} for r in rows}
    ctrl = data.get(0, {}).get("reward_raw")
    best = None
    for coh in sorted(data, key=lambda c: -data[c]["reward"]):
        d = data[coh]; label = COHORT_LABELS.get(coh, f"cohort {coh}")
        lift = ""
        if ctrl and ctrl != 0 and coh != 0:
            pct = (d["reward_raw"] - ctrl) / abs(ctrl) * 100.0
            lift = f"{pct:+.1f}%"
        if best is None: best = (label, d["reward"])
        p(f"  {label:<26}{d['fights']:>7}{d['reward']:>12}{str(d['surv'])+'%':>10}{lift:>13}")
    # Headline: policy ON (cohorts 1+2) vs control (cohort 0).
    if ctrl and ctrl != 0 and (1 in data or 2 in data):
        on_fights = sum(int(data[c]["fights"]) for c in (1,2) if c in data)
        on_rew = q(f"SELECT ROUND(AVG(reward),4),ROUND(AVG(ended_alive)*100,1) "
                   f"FROM ai_playerbot_combat_sample WHERE cohort IN (1,2)" +
                   (f" AND id > {args.since_id}" if args.since_id else "") + ";")
        ctrl_surv = data.get(0,{}).get("surv")
        if on_rew and on_rew[0][0]:
            onr = float(on_rew[0][0]); onsurv = float(on_rew[0][1])
            pct = (onr - ctrl) / abs(ctrl) * 100.0
            p("")
            p(f"  >> LEARNED POLICY vs control: {pct:+.1f}% reward, "
              f"survival {onsurv:.0f}% vs {ctrl_surv:.0f}%  (policy {'HELPS' if pct>0 else 'HURTS'})")
            p(f"     ({on_fights} policy fights vs {data.get(0,{}).get('fights','0')} control)")
    elif len(data) <= 1:
        p("  (only one cohort has data yet — need more fights for a comparison)")
    p("="*64)

    # POLICY readout: join per-decision (state, action) rows to the fight reward and
    # show, per class, which actions correlate with the best fights. This is the seed
    # of a learned rotation (best action per state) vs hand-coded triggers.
    dec = q("SELECT COUNT(*) FROM ai_playerbot_decision_sample" +
            (f" WHERE id > {args.since_id}" if args.since_id else "") + ";")
    if dec and int(dec[0][0]) > 0:
        p("")
        p(f"LEARNED ACTION VALUE (decisions joined to fight reward; {dec[0][0]} decisions):")
        dwhere = f"AND d.id > {args.since_id}" if args.since_id else ""
        rows = q("SELECT d.class, d.action, COUNT(*) n, ROUND(AVG(c.reward),1) avg_reward "
                 "FROM ai_playerbot_decision_sample d "
                 "JOIN ai_playerbot_combat_sample c ON c.fight_id = d.fight_id "
                 f"WHERE 1=1 {dwhere} GROUP BY d.class, d.action HAVING n >= 5 "
                 "ORDER BY d.class, avg_reward DESC;")
        by_class = {}
        for r in rows:
            by_class.setdefault(int(r[0]), []).append((r[1], r[2], r[3]))
        for cls in sorted(by_class):
            name = CLASS_NAMES.get(cls, f"cls{cls}")
            best = by_class[cls][:3]
            worst = by_class[cls][-2:] if len(by_class[cls]) > 3 else []
            p(f"  {name}:")
            for act, n, rwd in best:
                p(f"      + {act:<26} reward {rwd:>6}  (n={n})")
            for act, n, rwd in worst:
                p(f"      - {act:<26} reward {rwd:>6}  (n={n})")
        p("  (+ = actions present in higher-reward fights, - = in lower-reward fights;")
        p("   this is correlation over states - the raw material for a per-class policy)")
        p("="*64)

    report = "\n".join(L)
    print(report)
    if args.save:
        d = os.path.join(os.path.dirname(__file__), "..", "logs", "learning_reports")
        os.makedirs(d, exist_ok=True)
        fn = os.path.join(d, f"learning_{datetime.datetime.now():%Y%m%d-%H%M%S}.txt")
        with open(fn, "w") as f: f.write(report + "\n")
        print(f"\nsaved: {fn}")

if __name__ == "__main__":
    main()
