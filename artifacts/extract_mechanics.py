#!/usr/bin/env python3
"""Regex scaffold: extract boss mechanics from src/scripts/dungeons boss scripts.
Output: encounter_mechanics_draft.json — for human review before seeding
ai_playerbot_encounter_mechanic. Accuracy target ~70-80% (targeting semantics
and conditional phases need review)."""
import json, os, re, sys, glob

ROOT = "src/scripts/dungeons"
out = {}

for path in sorted(glob.glob(f"{ROOT}/*/boss_*.cpp")):
    instance = path.split("/")[3]
    boss = os.path.basename(path)[5:-4]
    src = open(path, errors="replace").read()

    spells = dict(re.findall(r"SPELL_(\w+)\s*=\s*(\d+)", src))
    # pass 2: newer enum-class style (SpellImpendingDoom = 19702) used by e.g. boss_lucifron
    spells.update({k.upper(): v for k, v in re.findall(r"\bSpell(\w+)\s*=\s*(\d+)", src)})
    npcs = dict(re.findall(r"NPC_(\w+)\s*=\s*(\d+)", src))

    casts = []
    for m in re.finditer(r"DoCastSpellIfCan\(\s*([^,]+?)\s*,\s*(?:SPELL_|eSpells::Spell|Spell)(\w+)", src):
        target_expr, spell = m.group(1), m.group(2)
        target = ("self" if "m_creature" in target_expr and "Victim" not in target_expr
                  else "victim" if "Victim" in target_expr
                  else "random" if "Target" in target_expr or "pTarget" in target_expr
                  else target_expr[:30])
        casts.append({"spell": spell, "spell_id": spells.get(spell), "target": target})

    timers = {m.group(1): [int(m.group(2)), int(m.group(3))]
              for m in re.finditer(r"m_ui(\w+)Timer\s*=\s*urand\((\d+),\s*(\d+)\)", src)}
    fixed_timers = {m.group(1): [int(m.group(2)), int(m.group(2))]
                    for m in re.finditer(r"m_ui(\w+)Timer\s*=\s*(\d+)\s*;", src)}
    for k, v in fixed_timers.items():
        timers.setdefault(k, v)

    health_phases = sorted({float(m.group(1)) for m in
                            re.finditer(r"GetHealthPercent\(\)\s*<\s*([\d.]+)", src)}, reverse=True)

    summons = []
    for m in re.finditer(r"SummonCreature\(\s*NPC_(\w+)", src):
        e = npcs.get(m.group(1))
        summons.append({"npc": m.group(1), "entry": int(e) if e else None})

    boss_entry = None
    em = re.search(r"NPC_" + boss.upper().replace("-", "_") + r"\s*=\s*(\d+)", src)
    if em: boss_entry = int(em.group(1))

    out[f"{instance}/{boss}"] = {
        "instance": instance, "boss": boss, "boss_entry": boss_entry,
        "spells": {k: int(v) for k, v in spells.items()},
        "casts": casts, "timers_ms": timers,
        "health_phase_thresholds": health_phases,
        "summons": summons,
        "review_status": "draft",
    }

json.dump(out, open("artifacts/encounter_mechanics_draft.json", "w"), indent=1)
n_bosses = len(out)
n_spells = sum(len(b["spells"]) for b in out.values())
n_casts = sum(len(b["casts"]) for b in out.values())
print(f"bosses={n_bosses} spells={n_spells} cast-sites={n_casts}")
