#!/usr/bin/env python3
"""
MEO_GenerateESP.py  — P1 plugin generator for Marth's Enchanting Overhaul.

Emits MEO.esp (single master: Skyrim.esm) + Data/MEO/meo_runtime.json:
  - MISC: every gem x level from data/gem_catalog.json (234 forms)
  - MGEF: marker (contact + self, inert AbBlank clones) + Gem Pouch scripted MGEF
  - SPEL: Gem Pouch lesser power ; QUST: startup (grants power)
  - FLST: all gems / weapon gems / armor gems (script iteration handles)
  - runtime JSON: per gem -> effect MGEF ref, per-level magnitude, tier, xp curve,
    and the MISC FormID of each gem x level (so the socket script maps
    typeIndex+level <-> item form and applies the effect via GetFormFromFile).

Effects are resolved at RUNTIME from the JSON (GetFormFromFile), so the ESP needs
no DLC masters and a missing DLC just disables that one gem. Magnitudes are the
tunable baseline (data/gem_balance -> gem_catalog), never a hard bake.

Binary helpers verified against vanilla donors in P0. Run: python3 MEO_GenerateESP.py [out]
"""
import struct, sys, os, json
from io import BytesIO

CATALOG = json.load(open(os.path.join(os.path.dirname(__file__), 'data/gem_catalog.json')))

# ── FormIDs (single master Skyrim.esm -> own file index 0x01; 0x800-0xFFF = ESL-able) ──
OWN = 0x01000000
FREF_PLAYER     = 0x00000014
FREF_EQUP_VOICE = 0x00025BEE

FID_MARKER_CONTACT = OWN | 0x800   # weapon effects (fire-and-forget, contact)
FID_MARKER_SELF    = OWN | 0x801   # armor effects (constant, self)
FID_POUCH_MGEF     = OWN | 0x802
FID_POUCH_SPELL    = OWN | 0x803
FID_STARTUP_QUEST  = OWN | 0x804
FID_FLST_ALL       = OWN | 0x805
FID_FLST_WEAPON    = OWN | 0x806
FID_FLST_ARMOR     = OWN | 0x807
FID_POUCH_CONT     = OWN | 0x8FE   # hidden Gem Pouch container (M5 ContainerMenu UI); FROZEN
FID_MENTOR_GEM     = OWN | 0x8FF   # unique support gem (M3d); FROZEN, outside the sequential range
FID_GEM_BASE       = OWN | 0x900   # MISC gems allocated sequentially from here
RUNTIME_REL = "MEO/meo_runtime.json"   # under Data/SKSE/Plugins (JsonUtil root)

ROMAN = {1:"I",2:"II",3:"III",4:"IV",5:"V"}

# ── binary helpers (forked from P0 / MRO — byte-for-byte valid) ──
FORM_VERSION = 44
def subrec(t,d): return t.encode('ascii')+struct.pack('<H',len(d))+d
def record(t,fid,fl,d):
    return (t.encode('ascii')+struct.pack('<I',len(d))+struct.pack('<I',fl)
            +struct.pack('<I',fid)+struct.pack('<I',0)+struct.pack('<H',FORM_VERSION)
            +struct.pack('<H',0)+d)
def group(label,data):
    return b'GRUP'+struct.pack('<I',24+len(data))+label.encode('ascii')+struct.pack('<iII',0,0,0)+data
def zstr(s): return s.encode('ascii')+b'\x00'

VMAD_VERSION, OBJECT_FORMAT = 5, 2
class VMADBuilder:
    def __init__(self): self.scripts=[]
    def add_script(self,name,props): self.scripts.append((name,props))
    def build(self):
        b=BytesIO(); b.write(struct.pack('<HHH',VMAD_VERSION,OBJECT_FORMAT,len(self.scripts)))
        for name,props in self.scripts:
            e=name.encode('ascii'); b.write(struct.pack('<H',len(e))+e+struct.pack('<B',0)+struct.pack('<H',len(props)))
            for pn,pv in props:
                pe=pn.encode('ascii'); b.write(struct.pack('<H',len(pe))+pe+bytes([pv[0]])+struct.pack('<B',1)+pv[1:])
        return b.getvalue()
def prop_obj(fid): return bytes([1])+struct.pack('<H',0)+struct.pack('<h',-1)+struct.pack('<I',fid)
def prop_str(s):   e=s.encode('ascii'); return bytes([2])+struct.pack('<H',len(e))+e
def prop_obj_array(fids):
    body=struct.pack('<I',len(fids))
    for f in fids: body+=struct.pack('<H',0)+struct.pack('<h',-1)+struct.pack('<I',f)
    return bytes([11])+body

def make_tes4(next_id):
    hedr=struct.pack('<f',1.70)+struct.pack('<I',100)+struct.pack('<I',next_id)
    body=subrec('HEDR',hedr)+subrec('CNAM',zstr("Marth"))+subrec('SNAM',zstr("Marth's Enchanting Overhaul"))
    body+=subrec('MAST',zstr("Skyrim.esm"))+subrec('DATA',struct.pack('<Q',0))
    return record('TES4',0,0x00000200,body)

# ── MGEF markers: inert AbBlank clone (Script archetype, no VMAD), one contact one self ──
def mgef_marker_data(delivery):
    d=bytearray(152)
    struct.pack_into('<I',d,0,0x8000); struct.pack_into('<I',d,12,0xFFFFFFFF); struct.pack_into('<I',d,16,0xFFFFFFFF)
    struct.pack_into('<I',d,64,1); struct.pack_into('<i',d,68,-1)
    struct.pack_into('<I',d,80,1 if delivery==1 else 0)   # cast: FF for contact, Constant for self
    struct.pack_into('<I',d,84,delivery)                   # 1 contact / 0 self
    struct.pack_into('<i',d,88,-1); struct.pack_into('<f',d,112,1.0)
    return bytes(d)
def mgef_scripted_self_data():
    d=bytearray(152)
    struct.pack_into('<I',d,0,0x8000); struct.pack_into('<I',d,12,0xFFFFFFFF); struct.pack_into('<I',d,16,0xFFFFFFFF)
    struct.pack_into('<I',d,64,1); struct.pack_into('<i',d,68,-1)
    struct.pack_into('<I',d,80,1); struct.pack_into('<I',d,84,0); struct.pack_into('<i',d,88,-1); struct.pack_into('<f',d,112,1.0)
    return bytes(d)

def make_mgefs():
    out=BytesIO()
    for fid,edid,deliv in [(FID_MARKER_CONTACT,"MEO_MarkerContact",1),(FID_MARKER_SELF,"MEO_MarkerSelf",0)]:
        body =subrec('EDID',zstr(edid))+subrec('FULL',zstr(""))+subrec('MDOB',struct.pack('<I',0))
        body+=subrec('DATA',mgef_marker_data(deliv))+subrec('SNDD',b'')+subrec('DNAM',struct.pack('<I',0))
        out.write(record('MGEF',fid,0,body))
    # Pouch scripted MGEF (order copied from vanilla scripted MGEF: EDID,VMAD,FULL,MDOB,DATA,SNDD,DNAM)
    vmad=VMADBuilder(); vmad.add_script("MEO_PouchScript",[
        ("PlayerRef",prop_obj(FREF_PLAYER)),
        ("AllGems",prop_obj(FID_FLST_ALL)),
        ("RuntimePath",prop_str(RUNTIME_REL)),
        ("MarkerContact",prop_obj(FID_MARKER_CONTACT)),
        ("MarkerSelf",prop_obj(FID_MARKER_SELF)),
    ])
    body =subrec('EDID',zstr("MEO_PouchMGEF"))+subrec('VMAD',vmad.build())+subrec('FULL',zstr("Gem Pouch"))
    body+=subrec('MDOB',struct.pack('<I',0))+subrec('DATA',mgef_scripted_self_data())+subrec('SNDD',b'')+subrec('DNAM',struct.pack('<I',0))
    out.write(record('MGEF',FID_POUCH_MGEF,0,body))
    return group('MGEF',out.getvalue())

# ── MISC gems from catalog: assign FormIDs, build display names, record the map ──
def allocate_gems():
    """Return (misc_records_bytes, gem_form_map, weapon_fids, armor_fids).
    gem_form_map[gid] = {level: fid}. FormIDs stable via sorted gid + type_index order."""
    out=BytesIO(); fid=FID_GEM_BASE; gem_form_map={}; weapon_fids=[]; armor_fids=[]
    for gid in sorted(CATALOG, key=lambda g: CATALOG[g]['type_index']):
        g=CATALOG[gid]; levels=1 if g['single_level'] else 5
        gem_form_map[gid]={}
        for lvl in range(1,levels+1):
            name=g['name'] if levels==1 else f"{g['name']} {ROMAN[lvl]}"
            body =subrec('EDID',zstr(f"MEO_Gem_{gid}_{lvl}"))
            body+=subrec('OBND',b'\x00'*12)+subrec('FULL',zstr(f"{name} Gem" if levels==1 else name))
            body+=subrec('DATA',struct.pack('<If',50*lvl,0.1))
            out.write(record('MISC',fid,0,body))
            gem_form_map[gid][lvl]=fid
            (weapon_fids if g['domain']=='weapon' else armor_fids).append(fid)
            fid+=1
    return out.getvalue(), gem_form_map, weapon_fids, armor_fids, fid

def make_flst(fid,edid,member_fids):
    body=subrec('EDID',zstr(edid))
    for m in member_fids: body+=subrec('LNAM',struct.pack('<I',m))
    return record('FLST',fid,0,body)

def spit_lesser_power():
    return struct.pack('<fIIfIIffI',0.0,0,3,0.0,1,0,0.0,0.0,0)  # type3 lesser power, FF self
def make_spel():
    body =subrec('EDID',zstr("MEO_GemPouchPower"))+subrec('OBND',b'\x00'*12)+subrec('FULL',zstr("Gem Pouch"))
    body+=subrec('MDOB',struct.pack('<I',0))+subrec('ETYP',struct.pack('<I',FREF_EQUP_VOICE))
    body+=subrec('DESC',zstr("Open the Gem Pouch to socket, view, or remove gems."))
    body+=subrec('SPIT',spit_lesser_power())+subrec('EFID',struct.pack('<I',FID_POUCH_MGEF))+subrec('EFIT',struct.pack('<fII',0.0,0,0))
    return group('SPEL',record('SPEL',FID_POUCH_SPELL,0,body))

def make_cont():
    # Hidden Gem Pouch container: no model, no contents, no respawn flags.
    # The DLL spawns a temporary reference of it and activates that ref to
    # open a real two-pane ContainerMenu (M5). DATA = flags u8 + weight f32.
    body =subrec('EDID',zstr("MEO_GemPouchCont"))+subrec('OBND',b'\x00'*12)
    body+=subrec('FULL',zstr("Gem Pouch"))+subrec('DATA',struct.pack('<Bf',0,0.0))
    return group('CONT',record('CONT',FID_POUCH_CONT,0,body))

def qust_dnam(): return struct.pack('<B',20)+b'\x01\x00\xff'+struct.pack('<HHI',0x0001|0x0004,0,0)
def make_qust():
    vmad=VMADBuilder(); vmad.add_script("MEO_StartupQuest",[("PlayerRef",prop_obj(FREF_PLAYER)),("GemPouchPower",prop_obj(FID_POUCH_SPELL))])
    body =subrec('EDID',zstr("MEO_StartupQuest"))+subrec('FULL',zstr("MEO Startup"))+subrec('VMAD',vmad.build())
    body+=subrec('DNAM',qust_dnam())+subrec('NEXT',b'')+subrec('ANAM',struct.pack('<I',0))
    return group('QUST',record('QUST',FID_STARTUP_QUEST,0,body))

def write_runtime_json(out_dir, gem_form_map):
    """Per-gem runtime data the socket script reads via JsonUtil. Lives under
    Data/SKSE/Plugins/MEO/ so JsonUtil resolves 'MEO/meo_runtime.json'."""
    rt={'_meta':{'count':len(CATALOG),'version':1}}
    for gid,g in CATALOG.items():
        rt[gid]={
            'name':g['name'],'type_index':g['type_index'],'domain':g['domain'],
            'tier':g['power_tier'],'xp_mult':g['xp_mult'],'single_level':g['single_level'],
            'curve':g['curve'],'mgef_refs':g['mgef_refs'],
            # LOCAL FormID (lower 24 bits) for Game.GetFormFromFile(id,"MEO.esp")
            'forms':{str(l):f"0x{fid & 0xFFFFFF:06X}" for l,fid in gem_form_map[gid].items()},
        }
    d=os.path.join(out_dir,'SKSE','Plugins','MEO'); os.makedirs(d,exist_ok=True)
    json.dump(rt,open(os.path.join(d,'meo_runtime.json'),'w'),indent=1)

def main():
    out_dir=sys.argv[1] if len(sys.argv)>1 else "out"; os.makedirs(out_dir,exist_ok=True)
    misc_bytes, gem_form_map, weapon_fids, armor_fids, next_local = allocate_gems()
    # Mentor gem (unique support: doubles Gem XP; DLL-managed, not in FLSTs).
    mentor=subrec('EDID',zstr("MEO_Gem_mentor"))+subrec('OBND',b'\x00'*12)
    mentor+=subrec('FULL',zstr("Mentor Gem"))+subrec('DATA',struct.pack('<If',750,0.1))
    misc_bytes=record('MISC',FID_MENTOR_GEM,0,mentor)+misc_bytes
    esp=BytesIO()
    esp.write(make_tes4(next_local & 0xFFFFFF))
    esp.write(make_mgefs())
    esp.write(group('MISC',misc_bytes))
    esp.write(make_cont())
    esp.write(make_spel())
    flsts=(make_flst(FID_FLST_ALL,"MEO_AllGems",weapon_fids+armor_fids)
          +make_flst(FID_FLST_WEAPON,"MEO_WeaponGems",weapon_fids)
          +make_flst(FID_FLST_ARMOR,"MEO_ArmorGems",armor_fids))
    esp.write(group('FLST',flsts))
    esp.write(make_qust())
    data=esp.getvalue()
    with open(os.path.join(out_dir,"MEO.esp"),'wb') as f: f.write(data)
    write_runtime_json(out_dir, gem_form_map)
    ngems=len(CATALOG); nmisc=len(weapon_fids)+len(armor_fids)
    print(f"Written: {out_dir}/MEO.esp ({len(data):,} bytes)")
    print(f"  MGEF x3  (marker contact+self, pouch)   CONT x1  SPEL x1  QUST x1  FLST x3")
    print(f"  MISC x{nmisc}  ({ngems} gems: {len(weapon_fids)} weapon-domain + {len(armor_fids)} armor-domain forms)")
    print(f"Written: {out_dir}/SKSE/Plugins/{RUNTIME_REL} ({ngems} gems)")

if __name__=="__main__": main()
