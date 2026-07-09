#!/usr/bin/env python3
# Tour several leveling hotspots as the observer: reposition via DB, log in, record 45s each.
import subprocess, time, sys
import observe

GUID=20000
def db(sql):
    return subprocess.run(["mysql","-h127.0.0.1","-uroot","-pmangos","tw_char","-N","-e",sql],
                          capture_output=True,text=True).stdout.strip()

SPOTS=[
    # label, map, x, y, z
    ("elwynn",   0, -9460.0,   45.0,  56.0),   # Goldshire / Fargodeep leveling
    ("westfall", 0, -10640.0, 1150.0, 40.0),   # Sentinel Hill
    ("redridge", 0, -9280.0, -2150.0, 65.0),   # Lakeshire
    ("barrens",  1,  -430.0, -2650.0, 95.0),   # Crossroads
    ("durotar",  1,  -618.0, -4250.0, 38.0),   # Valley of Trials / Sen'jin
    ("tirisfal", 0,  1676.0, 1676.0, 140.0),   # Brill / Deathknell (undead)
]

SECS=int(sys.argv[1]) if len(sys.argv)>1 else 45
SCR="out"

for label,mapid,x,y,z in SPOTS:
    # wait until previous session fully logged out
    for _ in range(30):
        if db(f"SELECT online FROM characters WHERE guid={GUID}") in ("0",""): break
        time.sleep(1)
    db(f"UPDATE characters SET map={mapid}, position_x={x}, position_y={y}, position_z={z}, "
       f"zone=0 WHERE guid={GUID}")
    print(f"\n=== {label} map{mapid} ({x:.0f},{y:.0f}) ===")
    ev=open(f"{SCR}/events_{label}.tsv","w"); sn=open(f"{SCR}/snap_{label}.tsv","w")
    try:
        observe.run(GUID,SECS,label,ev,sn)
    except Exception as e:
        print("  run error:",e)
    ev.close(); sn.close()
    time.sleep(2)
print("\nTOUR DONE")
