#!/usr/bin/env python3
# Drives the headless client: create char (once), log in, stand in the world for N seconds,
# and record every nearby unit's position track + target + movement + combat/chat events.
# Usage: python3 observe.py <guid> <seconds> <label>
import sys, time, struct, zlib, math
from wow_eye import *

def parse_movement(r, updateFlags):
    """Mirror BuildMovementUpdate. Returns (x,y,z,o, moveflags) or None."""
    x=y=z=o=None; moveflags=0
    if updateFlags & UPDATEFLAG_LIVING:
        moveflags=r.u32(); r.u32()                 # moveFlags, stime
        x=r.f(); y=r.f(); z=r.f(); o=r.f()
        if moveflags & MF_ONTRANSPORT:
            r.packguid(); r.f(); r.f(); r.f(); r.f(); r.u32()  # tguid + tpos + ttime (guid packed here? Write uses raw; be safe: try raw u64)
        if moveflags & MF_SWIMMING: r.f()          # s_pitch
        r.u32()                                    # fallTime
        if moveflags & MF_JUMPING: r.f(); r.f(); r.f(); r.f()
        if moveflags & MF_SPLINE_ELEVATION: r.f()
        for _ in range(6): r.f()                   # 6 speeds
        if moveflags & MF_SPLINE_ENABLED:
            sfl=r.u32()
            if sfl & SFLAG_FINAL_ANGLE: r.f()
            elif sfl & SFLAG_FINAL_TARGET: r.u64()
            elif sfl & SFLAG_FINAL_POINT: r.f(); r.f(); r.f()
            r.i32(); r.i32(); r.u32()              # timePassed, duration, id
            nodes=r.u32()
            for _ in range(nodes): r.f(); r.f(); r.f()
            r.f(); r.f(); r.f()                    # final dest
    else:
        if updateFlags & UPDATEFLAG_HAS_POSITION:
            x=r.f(); y=r.f(); z=r.f(); o=r.f()
    if updateFlags & UPDATEFLAG_HIGHGUID: r.u32()
    if updateFlags & UPDATEFLAG_ALL: r.u32()
    if updateFlags & UPDATEFLAG_MELEE_ATTACKING: r.packguid()
    if updateFlags & UPDATEFLAG_TRANSPORT: r.u32()
    return (x,y,z,o,moveflags)

def parse_values(r):
    """Mask block: u8 blockcount, blockcount*u32 mask, then a u32 for each set bit."""
    nblocks=r.u8()
    mask=[r.u32() for _ in range(nblocks)]
    fields={}
    for i in range(nblocks*32):
        if mask[i>>5] & (1<<(i&31)):
            fields[i]=r.u32()
    return fields

class Scene:
    def __init__(s): s.units={}   # guid -> dict(entry,level,x,y,z,target,hp,maxhp,mf,last_seen,track[])
    def upsert(s,guid,**kw):
        u=s.units.setdefault(guid,{"guid":guid,"track":[],"entry":0,"level":0,"target":0,
                                   "hp":0,"maxhp":0,"mf":0,"x":None,"y":None,"z":None})
        u.update({k:v for k,v in kw.items() if v is not None})
    def apply_fields(s,guid,f):
        u=s.units.get(guid)
        if not u: return
        if UF_ENTRY in f: u["entry"]=f[UF_ENTRY]
        if UF_LEVEL in f: u["level"]=f[UF_LEVEL]
        if UF_HEALTH in f: u["hp"]=f[UF_HEALTH]
        if UF_MAXHEALTH in f: u["maxhp"]=f[UF_MAXHEALTH]
        if UF_TARGET in f:  # 2 dwords low+high
            lo=f.get(UF_TARGET,0); hi=f.get(UF_TARGET+1,0); u["target"]=lo|(hi<<32)
        if UF_FLAGS in f: u["unitflags"]=f[UF_FLAGS]

def handle_update_object(c, scene, pl, compressed=False):
    data=pl
    if compressed:
        # SMSG_COMPRESSED_UPDATE_OBJECT: u32 uncompressed_size, then zlib
        usize=struct.unpack_from("<I",pl,0)[0]
        data=zlib.decompress(pl[4:])
    r=R(data)
    count=r.u32()
    r.u8()            # hasTransport byte (UpdateData::BuildPacket writes it after blockCount)
    for _ in range(count):
        utype=r.u8()
        if utype==5:  # NEAR_OBJECTS (has count of guids) - vanilla may not use; skip defensively
            n=r.u32()
            for _ in range(n): r.packguid()
            continue
        if utype==4:  # OUT_OF_RANGE_OBJECTS
            n=r.u32()
            for _ in range(n):
                g=r.packguid()
                scene.units.pop(g,None)
            continue
        guid=r.packguid()
        if utype in (2,3):   # CREATE_OBJECT / CREATE_OBJECT2
            objtype=r.u8()
            uf=r.u8()
            mv=parse_movement(r,uf)
            fields=parse_values(r)
            x,y,z,o,mf=mv
            scene.upsert(guid,x=x,y=y,z=z,mf=mf,objtype=objtype)
            scene.apply_fields(guid,fields)
            if (is_creature(guid) or is_player(guid)) and guid not in c.names:
                c.name_query(guid)
        elif utype==1:       # MOVEMENT
            uf=r.u8()
            mv=parse_movement(r,uf)
            x,y,z,o,mf=mv
            scene.upsert(guid,x=x,y=y,z=z,mf=mf)
        elif utype==0:       # VALUES
            fields=parse_values(r)
            scene.apply_fields(guid,fields)

# player-type movement broadcasts: packguid + MovementInfo(pos)
MOVE_START={0xB5,0xB6,0xB8,0xB9,0xBB,0xBC,0xBD,0xBF,0xC0,0xCA}   # start fwd/back/strafe/jump/turn/pitch/swim
MOVE_STOP={0xB7,0xBA,0xBE,0xC1,0xCB}                              # stop / stop-strafe/turn/pitch/swim
MOVE_MISC={0xC9,0xDA,0xDB,0xEE}                                  # fall_land, set_facing, set_pitch, heartbeat
MOVE_OPS=MOVE_START|MOVE_STOP|MOVE_MISC

def parse_player_move(r):
    guid=r.packguid()
    r.u32()                       # moveFlags
    r.u32()                       # time
    x=r.f(); y=r.f(); z=r.f(); r.f()  # pos + orientation
    return guid,x,y,z

def parse_monster_move(r):
    guid=r.packguid()
    sx,sy,sz=r.f(),r.f(),r.f()
    r.u32()                       # spline id
    ftype=r.u8()
    if r.rest()==0:               # STOP packet
        return dict(guid=guid,x=sx,y=sy,z=sz,moving=False,dest=None,dur=0)
    if ftype==3: r.u64()          # facing target
    elif ftype==4: r.f()          # facing angle
    elif ftype==2: r.f(); r.f(); r.f()  # facing spot
    r.u32()                       # splineflags
    dur=r.u32()
    r.u32()                       # point count
    dx,dy,dz=r.f(),r.f(),r.f()    # absolute destination (first point written)
    return dict(guid=guid,x=sx,y=sy,z=sz,moving=True,dest=(dx,dy,dz),dur=dur)

def handle_name_query(c,pl):
    r=R(pl); guid=r.u64(); name=r.cstr().decode("utf-8","replace")
    c.names[guid]=name

def create_char(c,name="Obseye"):
    # human warrior: race,class,gender,skin,face,hairStyle,hairColor,facialHair,outfitId, +u32 challengeMask (Turtle)
    body=name.encode()+b"\x00"+struct.pack("<BBBBBBBBBI",1,1,0,5,5,2,7,0,0,0)
    c.send_packet(CMSG_CHAR_CREATE,body)
    t=time.time()
    while time.time()-t<5:
        op,pl=c.read_packet()
        if op==SMSG_CHAR_CREATE:
            return pl[0]
    return -1

def run(guid, seconds, label, events_f, snap_f):
    c=Client(); code=c.auth()
    if code!=0x0C: print("auth failed",hex(code)); return
    c.send_packet(CMSG_PLAYER_LOGIN,struct.pack("<Q",guid))
    scene=Scene()
    in_world=False; t0=time.time(); last_snap=0; last_ping=0; last_hb=0
    my=[None,None,None,None]  # my pos, set from verify world
    while time.time()-t0<seconds:
        try: op,pl=c.read_packet()
        except socket.timeout: break
        except EOFError: print("server closed"); break
        now=time.time()-t0
        if op==SMSG_LOGIN_VERIFY_WORLD:
            r=R(pl); mapid=r.u32(); x=r.f(); y=r.f(); z=r.f(); o=r.f()
            my[0]=mapid; my[1]=x; my[2]=y; my[3]=z
            in_world=True
            print(f"IN WORLD map={mapid} pos=({x:.0f},{y:.0f},{z:.0f})")
        elif op==SMSG_UPDATE_OBJECT:
            try: handle_update_object(c,scene,pl,False)
            except Exception as e: pass
        elif op==SMSG_COMPRESSED_UPDATE_OBJECT:
            try: handle_update_object(c,scene,pl,True)
            except Exception as e: pass
        elif op in MOVE_OPS:
            try:
                g,x,y,z=parse_player_move(R(pl))
                u=scene.units.get(g)
                if u is not None:
                    u["x"]=x; u["y"]=y
                    if op in MOVE_START: u["moving_now"]=True
                    elif op in MOVE_STOP: u["moving_now"]=False
                    act={0xB5:"fwd",0xB7:"stop",0xBB:"jump",0xC9:"land",0xEE:"beat",0xDA:"face"}.get(op,hex(op))
                    events_f.write(f"{now:.1f}\tMOVE\t{c.names.get(g,g&0xFFFFFFFF)}\t{act}\t({x:.0f},{y:.0f})\n")
            except Exception: pass
        elif op==SMSG_ATTACKERSTATEUPDATE:
            try:
                r=R(pl); r.u32(); a=r.packguid(); v=r.packguid(); dmg=r.u32()
                events_f.write(f"{now:.1f}\tSWING\t{c.names.get(a,a&0xFFFFFFFF)}\t->\t{c.names.get(v,v&0xFFFFFFFF)}\tdmg{dmg}\n")
            except Exception: pass
        elif op==SMSG_SPELL_GO:
            try:
                r=R(pl); r.packguid(); caster=r.packguid(); spellid=r.u32()
                if caster not in c.names: c.name_query(caster)
                events_f.write(f"{now:.1f}\tCAST\t{c.names.get(caster,caster&0xFFFFFFFF)}\tspell{spellid}\n")
            except Exception: pass
        elif op==SMSG_MONSTER_MOVE:
            try:
                mm=parse_monster_move(R(pl))
                g=mm["guid"]
                u=scene.units.get(g)
                if u is not None:
                    u["x"]=mm["x"]; u["y"]=mm["y"]
                    u["moving_now"]=mm["moving"]
                    u["dest"]=mm["dest"]
                    if mm["moving"] and mm["dest"]:
                        u["dest_dist"]=math.hypot(mm["dest"][0]-mm["x"],mm["dest"][1]-mm["y"])
                    events_f.write(f"{now:.1f}\tMOVE\t{c.names.get(g,g&0xFFFFFFFF)}\t"
                                   f"{'stop' if not mm['moving'] else 'to(%.0f,%.0f)d%.0f'%(mm['dest'][0],mm['dest'][1],u.get('dest_dist',0))}\n")
            except Exception: pass
        elif op==SMSG_NAME_QUERY_RESPONSE:
            try: handle_name_query(c,pl)
            except Exception: pass
        elif op==SMSG_MESSAGECHAT:
            try:
                r=R(pl); ctype=r.u8(); lang=r.u32(); sg=r.u64()
                # (skip precise body parse; log sender guid + raw tail text if present)
                events_f.write(f"{now:.1f}\tCHAT\ttype{ctype}\tguid{sg}\n")
            except Exception: pass
        elif op==SMSG_EMOTE:
            try:
                r=R(pl); anim=r.u32(); g=r.u64()
                events_f.write(f"{now:.1f}\tEMOTE\tanim{anim}\t{c.names.get(g,g)}\n")
            except Exception: pass
        elif op==SMSG_AI_REACTION:
            try:
                r=R(pl); g=r.u64(); react=r.u32()
                events_f.write(f"{now:.1f}\tAI_REACT\t{c.names.get(g,g)}\treact{react}\n")
            except Exception: pass
        elif op==SMSG_ATTACKSTART:
            try:
                r=R(pl); a=r.u64(); v=r.u64()
                events_f.write(f"{now:.1f}\tATTACK_START\t{c.names.get(a,a)}\t->\t{c.names.get(v,v)}\n")
            except Exception: pass
        elif op==SMSG_ATTACKSTOP:
            events_f.write(f"{now:.1f}\tATTACK_STOP\n")
        # keepalive
        if now-last_ping>25:
            c.send_packet(CMSG_PING,struct.pack("<II",int(now*1000)&0xFFFFFFFF,50)); last_ping=now
        # snapshot scene every 2s
        if in_world and now-last_snap>=2.0:
            last_snap=now
            snapshot(scene,c,my,now,snap_f,label)
    # final summary
    return scene,c,my

def snapshot(scene,c,my,now,snap_f,label):
    mx,my_,mz=my[1],my[2],my[3]
    for g,u in scene.units.items():
        if u["x"] is None: continue
        if mx is not None:
            d=math.hypot(u["x"]-mx,u["y"]-my_)
            if d>120: continue    # only log units near me
        else: d=-1
        # movement track: append current pos with time
        u["track"].append((now,u["x"],u["y"]))
        kind="PLR" if is_player(g) else ("NPC" if is_creature(g) else "OBJ")
        name=c.names.get(g,"?")
        hp=u.get("hp",0); mhp=u.get("maxhp",0)
        hpp=int(100*hp/mhp) if mhp else 0
        moving=1 if (u.get("moving_now") or (u.get("mf",0)&(MF_SPLINE_ENABLED|MF_MASK_MOVING))) else 0
        tgt=u.get("target",0)
        tname=c.names.get(tgt,"") if tgt else ""
        ddist=u.get("dest_dist",0) if u.get("moving_now") else 0
        snap_f.write(f"{now:.1f}\t{label}\t{kind}\tguid{g&0xFFFFFFFF}\tentry{u.get('entry',0)}\t"
                     f"lvl{u.get('level',0)}\t({u['x']:.0f},{u['y']:.0f})\td{d:.0f}\thp{hpp}\t"
                     f"mv{moving}\ttgt{tgt&0xFFFFFFFF}\t{name}\t>{tname}\tdd{ddist:.0f}\n")

if __name__=="__main__":
    guid=int(sys.argv[1]); secs=int(sys.argv[2]); label=sys.argv[3] if len(sys.argv)>3 else "spot"
    ev=open(f"/tmp/claude-1000/-home-zuppier-tw-server-dev/feea4b59-7628-4127-b8b5-0bba71fa2c61/scratchpad/events_{label}.tsv","w")
    sn=open(f"/tmp/claude-1000/-home-zuppier-tw-server-dev/feea4b59-7628-4127-b8b5-0bba71fa2c61/scratchpad/snap_{label}.tsv","w")
    run(guid,secs,label,ev,sn)
    ev.close(); sn.close()
    print("wrote events_%s.tsv snap_%s.tsv"%(label,label))
