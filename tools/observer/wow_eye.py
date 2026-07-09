#!/usr/bin/env python3
# Headless 1.12.1 (build 5875) world client for tortoise-wow.
# Auth via planted session key K in account.sessionkey (no realmd/SRP6).
# Logs into the live server as a real player -> wakes the near-player bot cohort ->
# records what surrounding units actually do (position track, target, movement, chat, combat).
import socket, struct, hashlib, sys, time, zlib, os

HOST="192.168.1.28"; PORT=8091; BUILD=5875; REALM_ID=2; ACCOUNT="OBSEYE"
K=open(os.path.join(os.path.dirname(__file__),"obs_K.bin"),"rb").read()

SMSG_AUTH_CHALLENGE=0x1EC; CMSG_AUTH_SESSION=0x1ED; SMSG_AUTH_RESPONSE=0x1EE
CMSG_CHAR_ENUM=0x37; SMSG_CHAR_ENUM=0x3B; CMSG_CHAR_CREATE=0x36; SMSG_CHAR_CREATE=0x3A
CMSG_PLAYER_LOGIN=0x3D; SMSG_LOGIN_VERIFY_WORLD=0x236
CMSG_NAME_QUERY=0x50; SMSG_NAME_QUERY_RESPONSE=0x51
SMSG_UPDATE_OBJECT=0xA9; SMSG_COMPRESSED_UPDATE_OBJECT=0x1F6; SMSG_DESTROY_OBJECT=0xAA
SMSG_MONSTER_MOVE=0xDD; SMSG_MESSAGECHAT=0x96; CMSG_MESSAGECHAT=0x95
SMSG_EMOTE=0x103; SMSG_TEXT_EMOTE=0x105; SMSG_AI_REACTION=0x13C
SMSG_ATTACKSTART=0x143; SMSG_ATTACKSTOP=0x144; SMSG_SPELL_GO=0x132; SMSG_SPELL_START=0x131
SMSG_ATTACKERSTATEUPDATE=0x14A
CMSG_PING=0x1DC; SMSG_PONG=0x1DD; SMSG_LOGOUT_COMPLETE=0x4D; CMSG_LOGOUT_REQUEST=0x4B
MSG_MOVE_HEARTBEAT=0xEE
OPNAME={v:k for k,v in list(globals().items()) if k.startswith(("SMSG_","CMSG_","MSG_")) and isinstance(v,int)}

# update field indices (OBJECT_END=6)
UF_TARGET=16; UF_HEALTH=22; UF_MAXHEALTH=28; UF_LEVEL=34; UF_FACTION=35
UF_BYTES0=36; UF_FLAGS=46; UF_ENTRY=3
# movement flags
MF_ONTRANSPORT=0x02000000; MF_SWIMMING=0x00200000; MF_JUMPING=0x00002000
MF_SPLINE_ELEVATION=0x04000000; MF_SPLINE_ENABLED=0x00400000
MF_MASK_MOVING=0x00000001|0x2|0x4|0x8|0x400  # fwd/back/strafe/lev-ish (moving indicator)
UPDATEFLAG_LIVING=0x20; UPDATEFLAG_HAS_POSITION=0x40; UPDATEFLAG_TRANSPORT=0x02
UPDATEFLAG_HIGHGUID=0x08; UPDATEFLAG_ALL=0x10; UPDATEFLAG_MELEE_ATTACKING=0x04
SFLAG_FINAL_POINT=0x10000; SFLAG_FINAL_TARGET=0x20000; SFLAG_FINAL_ANGLE=0x40000

def is_creature(guid): return ((guid>>48)&0xFFFF)==0xF130
def is_player(guid):   return (guid>>32)==0 and guid!=0

class Crypt:
    def __init__(s,key): s.key=key; s.si=s.sj=s.ri=s.rj=0; s.on=False
    def init(s): s.on=True
    def enc_send(s,d):
        if not s.on: return d
        b=bytearray(d)
        for t in range(len(b)):
            s.si%=len(s.key); x=((b[t]^s.key[s.si])+s.sj)&0xFF; s.si+=1; s.sj=x; b[t]=x
        return bytes(b)
    def dec_recv(s,d):
        if not s.on: return d
        b=bytearray(d)
        for t in range(len(b)):
            s.ri%=len(s.key); x=((b[t]-s.rj)&0xFF)^s.key[s.ri]; s.ri+=1; s.rj=b[t]; b[t]=x
        return bytes(b)

class R:  # little reader
    def __init__(s,d): s.d=d; s.p=0
    def rest(s): return len(s.d)-s.p
    def u8(s):  v=s.d[s.p]; s.p+=1; return v
    def u16(s): v=struct.unpack_from("<H",s.d,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from("<I",s.d,s.p)[0]; s.p+=4; return v
    def i32(s): v=struct.unpack_from("<i",s.d,s.p)[0]; s.p+=4; return v
    def u64(s): v=struct.unpack_from("<Q",s.d,s.p)[0]; s.p+=8; return v
    def f(s):   v=struct.unpack_from("<f",s.d,s.p)[0]; s.p+=4; return v
    def cstr(s):
        e=s.d.index(b"\x00",s.p); v=s.d[s.p:e]; s.p=e+1; return v
    def packguid(s):
        mask=s.u8(); g=0
        for i in range(8):
            if mask&(1<<i): g|=s.d[s.p]<<(8*i); s.p+=1
        return g

class Client:
    def __init__(s):
        s.s=socket.create_connection((HOST,PORT),timeout=30); s.s.settimeout(30)
        s.crypt=Crypt(K); s.buf=b""; s.names={}
    def recv_raw(s,n):
        while len(s.buf)<n:
            c=s.s.recv(65536)
            if not c: raise EOFError("closed")
            s.buf+=c
        o,s.buf=s.buf[:n],s.buf[n:]; return o
    def read_packet(s):
        hdr=s.crypt.dec_recv(s.recv_raw(4))
        size=struct.unpack(">H",hdr[:2])[0]; op=struct.unpack("<H",hdr[2:4])[0]
        pl=s.recv_raw(size-2) if size>=2 else b""
        return op,pl
    def send_packet(s,op,pl=b""):
        hdr=struct.pack(">H",len(pl)+4)+struct.pack("<I",op)
        s.s.sendall(s.crypt.enc_send(hdr)+pl)
    def auth(s):
        op,pl=s.read_packet(); assert op==SMSG_AUTH_CHALLENGE,hex(op)
        server_seed=struct.unpack("<I",pl[:4])[0]; client_seed=0x1BADCAFE
        acct=ACCOUNT.encode()
        h=hashlib.sha1(); h.update(acct); h.update(b"\x00\x00\x00\x00")
        h.update(struct.pack("<I",client_seed)); h.update(struct.pack("<I",server_seed)); h.update(K)
        names=["Blizzard_BindingUI","Blizzard_InspectUI","Blizzard_MacroUI","Blizzard_RaidUI"]
        raw=b"".join(n.encode()+b"\x00"+struct.pack("<BII",3,0x4C1C776D,0) for n in names)
        addon=struct.pack("<I",len(raw))+zlib.compress(raw,6)
        body=struct.pack("<II",BUILD,REALM_ID)+acct+b"\x00"+struct.pack("<I",client_seed)+h.digest()+addon
        s.send_packet(CMSG_AUTH_SESSION,body); s.crypt.init()
        # drain until AUTH_RESPONSE
        for _ in range(10):
            op,pl=s.read_packet()
            if op==SMSG_AUTH_RESPONSE: return pl[0]
        return -1
    def name_query(s,guid):
        if guid in s.names: return
        s.names[guid]=None
        s.send_packet(CMSG_NAME_QUERY,struct.pack("<Q",guid))

if __name__=="__main__":
    c=Client()
    print("AUTH", hex(c.auth()))
