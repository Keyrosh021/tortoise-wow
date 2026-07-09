#!/usr/bin/env python3
# Leader client: log in as a real player, RECRUIT nearby fleet bots into a party (they auto-accept),
# then pull a mob and observe how each group MEMBER performs (tank holds? healer heals? dps assist?
# who dies / stands idle). Usage: lead.py <guid> <seconds> <label>
import sys, time, struct, math, collections, subprocess, os
from wow_eye import *
import observe
from observe import R, parse_player_move, handle_update_object, handle_name_query, is_creature, is_player

CMSG_GROUP_INVITE=0x06E; SMSG_GROUP_LIST=0x07D; SMSG_PARTY_COMMAND_RESULT=0x07F
CMSG_SET_SELECTION=0x13D; CMSG_ATTACKSWING=0x141; CMSG_ATTACKSTOP=0x142; MSG_MOVE_HEARTBEAT=0x0EE

SCR=os.path.dirname(__file__)

def spell_names(ids):
    if not ids: return {}
    out=subprocess.run(["mysql","-h127.0.0.1","-uroot","-pmangos","tw_world","-N","-e",
        f"select entry,name from spell_template where entry in ({','.join(map(str,ids))})"],
        capture_output=True,text=True).stdout
    return {int(p[0]):p[1] for p in (l.split('\t') for l in out.splitlines()) if len(p)>=2}

class Leader(Client):
    def invite(self, name):
        self.send_packet(CMSG_GROUP_INVITE, name.encode()+b"\x00")
    def select(self, guid):
        self.send_packet(CMSG_SET_SELECTION, struct.pack("<Q", guid))
    def attack(self, guid):
        self.send_packet(CMSG_ATTACKSWING, struct.pack("<Q", guid))
    def heartbeat(self, x,y,z,o=0.0):
        # MovementInfo: moveFlags,time,x,y,z,o,fallTime
        body=struct.pack("<IIffffI", 0, int(time.time()*1000)&0xFFFFFFFF, x,y,z,o, 0)
        self.send_packet(MSG_MOVE_HEARTBEAT, body)

def run(guid, secs, label):
    c=Leader(); code=c.auth()
    if code!=0x0C: print("auth failed",hex(code)); return
    c.send_packet(CMSG_PLAYER_LOGIN, struct.pack("<Q", guid))
    scene=observe.Scene()
    ev=open(f"{SCR}/lead_{label}.tsv","w")
    members=set(); memberGuids={}; mypos=[None,None,None]; t0=time.time()
    phase="observe"; invited=[]; invite_i=0; target=None; spellids=set()
    last_hb=0; attack_sent=0; last_invite=0
    memberActs=collections.defaultdict(lambda: collections.Counter())  # name -> Counter(casts,swings,moves)
    memberHP=collections.defaultdict(list)
    tgtHP=[]
    while time.time()-t0 < secs:
        try: op,pl=c.read_packet()
        except socket.timeout: break
        except EOFError: print("server closed"); break
        now=time.time()-t0
        if op==SMSG_LOGIN_VERIFY_WORLD:
            r=R(pl); mp=r.u32(); mypos[0]=r.f(); mypos[1]=r.f(); mypos[2]=r.f()
            print(f"IN WORLD map={mp} ({mypos[0]:.0f},{mypos[1]:.0f})")
        elif op in (SMSG_UPDATE_OBJECT,SMSG_COMPRESSED_UPDATE_OBJECT):
            try: handle_update_object(c,scene,pl,op==SMSG_COMPRESSED_UPDATE_OBJECT)
            except Exception: pass
        elif op==SMSG_NAME_QUERY_RESPONSE:
            try: handle_name_query(c,pl)
            except Exception: pass
        elif op==SMSG_GROUP_LIST:
            try:
                # groupType(u8), ownFlags(u8), count(u32), then per member: name(cstr) guid(u64) status(u8) flags(u8)
                r=R(pl); gtype=r.u8(); flags=r.u8(); cnt=r.u32()
                mem=[]
                for _ in range(cnt):
                    nm=r.cstr().decode('utf-8','replace'); g=r.u64(); status=r.u8(); flg=r.u8()
                    mem.append(nm); memberGuids[g]=nm      # attribute by GUID (name resolution is unreliable)
                members=set(mem)
                ev.write(f"{now:.1f}\tGROUP\t{len(members)}\t{','.join(mem)}\n")
            except Exception: pass
        elif op==SMSG_PARTY_COMMAND_RESULT:
            ev.write(f"{now:.1f}\tPARTYRESULT\n")
        elif op in observe.MOVE_OPS:
            try:
                g,x,y,z=parse_player_move(R(pl))
                if g in memberGuids: memberActs[memberGuids[g]]["move"]+=1
            except Exception: pass
        elif op==SMSG_ATTACKERSTATEUPDATE:
            try:
                r=R(pl); r.u32(); a=r.packguid(); v=r.packguid(); dmg=r.u32()
                if a in memberGuids: memberActs[memberGuids[a]]["swing"]+=1
                if v in memberGuids: memberActs[memberGuids[v]]["taken"]+=dmg   # damage TAKEN (tank should be high)
                ev.write(f"{now:.1f}\tSWING\t{memberGuids.get(a) or c.names.get(a,a&0xFFFFFFFF)}\t->\t{memberGuids.get(v) or c.names.get(v,v&0xFFFFFFFF)}\t{dmg}\n")
            except Exception: pass
        elif op==SMSG_SPELL_GO:
            try:
                r=R(pl); r.packguid(); caster=r.packguid(); sid=r.u32(); spellids.add(sid)
                if caster in memberGuids: memberActs[memberGuids[caster]][("cast",sid)]+=1
                ev.write(f"{now:.1f}\tCAST\t{memberGuids.get(caster) or c.names.get(caster,caster&0xFFFFFFFF)}\t{sid}\n")
            except Exception: pass

        # PHASE 1: recruit nearby bots into a group (~4)
        if phase=="observe" and now>8:
            phase="invite"
        if phase=="invite":
            if now-last_invite>1.5 and len(members)<5:
                # pick a nearby named PLR not me/not member
                cand=None
                for g,u in scene.units.items():
                    if not is_player(g): continue
                    nm=c.names.get(g)
                    if nm and nm!="Obseye" and nm not in members and nm not in invited and u.get("x") is not None:
                        if mypos[0] and math.hypot(u["x"]-mypos[0],u["y"]-mypos[1])<55:
                            cand=nm; break
                if cand:
                    c.invite(cand); invited.append(cand); last_invite=now
                    ev.write(f"{now:.1f}\tINVITE\t{cand}\n")
                elif now>25:
                    phase="pull"
            if now>28: phase="pull"
        # PHASE 2: pull a nearby hostile creature; attack -> group should assist
        if phase=="pull" and not target:
            best=None; bd=9999
            for g,u in scene.units.items():
                if not is_creature(g): continue
                if u.get("maxhp",0)<=0 or u.get("hp",0)<=0: continue
                if u.get("x") is None or not mypos[0]: continue
                d=math.hypot(u["x"]-mypos[0],u["y"]-mypos[1])
                if d<bd: bd=d; best=g
            if best and bd<40:
                target=best; c.select(target); c.attack(target)
                ev.write(f"{now:.1f}\tPULL\t{c.names.get(target,target&0xFFFFFFFF)}\td{bd:.0f}\n")
                phase="fight"
            elif now>35: phase="fight"
        # keepalive heartbeat + re-attack
        if now-last_hb>2 and mypos[0]:
            c.heartbeat(mypos[0],mypos[1],mypos[2]); last_hb=now
        if phase=="fight" and target and now-attack_sent>3:
            c.attack(target); attack_sent=now
        # track member + target HP each loop via scene
        if target and target in scene.units:
            u=scene.units[target]; hp=u.get("hp",0); mhp=u.get("maxhp",1)
            tgtHP.append((now, int(100*hp/max(mhp,1))))

    # report
    names=spell_names(spellids)
    CLS={1:'Warrior',2:'Paladin',3:'Hunter',4:'Rogue',5:'Priest',7:'Shaman',8:'Mage',9:'Warlock',11:'Druid'}
    cinfo={}
    if members:
        inlist=",".join("'%s'"%n.replace("'","") for n in members)
        out=subprocess.run(["mysql","-h127.0.0.1","-uroot","-pmangos","tw_char","-N","-e",
          f"select name,class,level from characters where name in ({inlist})"],capture_output=True,text=True).stdout
        for ln in out.splitlines():
            p=ln.split("\t")
            if len(p)>=3: cinfo[p[0]]=(CLS.get(int(p[1]),'?'),p[2])
    print(f"\n=== GROUP TEST {label} ===  {len(members)} members")
    if tgtHP:
        print(f"  pull target HP: {tgtHP[0][1]}% -> {tgtHP[-1][1]}%  ({'downed' if tgtHP[-1][1]<=1 else 'survived/ongoing'})")
    for nm in sorted(members):
        a=memberActs[nm]; cls,lv=cinfo.get(nm,('?','?'))
        casts=[(k[1],v) for k,v in a.items() if isinstance(k,tuple)]
        cs=", ".join(f"{names.get(sid,f'sp{sid}')}x{v}" for sid,v in casts)
        total=a['swing']+sum(v for _,v in casts)
        verdict="IDLE(no combat actions)" if total==0 else "OK"
        if cls in ('Warrior','Rogue','Paladin') and a['swing']==0 and total==0: verdict="melee IDLE"
        if cls in ('Mage','Warlock','Priest','Shaman','Hunter','Druid') and not casts and total==0: verdict="caster: 0 spells/actions"
        print(f"  {nm:13} {cls:8} L{str(lv):2}  swings={a['swing']:3} casts=[{cs}] moves={a['move']:3} dmgTaken={a['taken']:5}  -> {verdict}")
    ev.close()
    print(f"wrote lead_{label}.tsv")

if __name__=="__main__":
    guid=int(sys.argv[1]); secs=int(sys.argv[2]); label=sys.argv[3] if len(sys.argv)>3 else "grp"
    run(guid, secs, label)
