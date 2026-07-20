#!/usr/bin/env python3
"""
MEO_GenerateESP.py  — P1 plugin generator for marth's Enchanting Overhaul.

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
import struct
import re, sys, os, json
from io import BytesIO

CATALOG = json.load(open(os.path.join(os.path.dirname(__file__), 'data/gem_catalog.json')))

def read_meo_version(default="0.0.0"):
    """The single source of truth for the version is kMEOVersion in
    native/plugin.cpp; read it so the build-stamped MCM readout can never drift
    from the DLL/log/console string. Falls back to a placeholder if unreadable."""
    try:
        src = open(os.path.join(os.path.dirname(__file__), 'native', 'plugin.cpp')).read()
        m = re.search(r'kMEOVersion\s*=\s*"([^"]+)"', src)
        if m:
            return m.group(1)
    except OSError:
        pass
    return default

# ── FormIDs (single master Skyrim.esm -> own file index 0x01; 0x800-0xFFF = ESL-able) ──
OWN = 0x01000000
FREF_PLAYER     = 0x00000014
FREF_EQUP_VOICE = 0x00025BEE

FID_MARKER_CONTACT = OWN | 0x800   # weapon effects (fire-and-forget, contact)
FID_MARKER_SELF    = OWN | 0x801   # armor effects (constant, self)
FID_POUCH_MGEF     = OWN | 0x802
FID_POUCH_SPELL    = OWN | 0x803
FID_ECHO_SPELL     = OWN | 0x809   # m36: Echo armor follower-share (DLL swaps its effect at runtime); FROZEN
FID_STARTUP_QUEST  = OWN | 0x804
FID_FLST_ALL       = OWN | 0x805
FID_FLST_WEAPON    = OWN | 0x806
FID_FLST_ARMOR     = OWN | 0x807
FID_MCM_QUEST      = OWN | 0x808   # MCM Helper config quest (attaches MEO_MCM); FROZEN
FID_PERK_BASE      = OWN | 0x810   # MEO perks 0x810.. (DESIGN §6); FROZEN
FID_POUCH_CONT     = OWN | 0x8FE   # hidden Gem Pouch container (M5 ContainerMenu UI); FROZEN
FID_MENTOR_GEM     = OWN | 0x8FF   # unique support gem (M3d); FROZEN, outside the sequential range
FID_GEM_BASE       = OWN | 0x900   # MISC gems allocated sequentially from here
# Phase 3 auto-minting (marth 2026-07-16): a RESERVED pool of pre-minted MISC
# gem forms the installer assigns to load-order-detected enchant families at
# patch time. Band 0xB00-0xD7F (128 slots x 5 levels — grown 32->64 the same
# day, pre-ship, after the keep-generic-uncovered class fix revealed Authoria
# alone has 54 viable candidates, then 64->128 on 2026-07-20 after Authoria
# +Summermyst+Thaumaturgy exhausted the 64-slot pool), FROZEN once shipped.
# Growth is append-only legal (new slots at the end); shrinking never is (allocate_pool trips).
# Curated-catalog growth stays BELOW 0xB00 (0xA11-0xAFF headroom, ~47 more
# families); allocate_gems() hard-fails before it can ever enter the pool band.
FID_POOL_BASE      = OWN | 0xB00
POOL_SLOTS         = 128
POOL_LEVELS        = 5
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
    body=subrec('HEDR',hedr)+subrec('CNAM',zstr("marth"))+subrec('SNAM',zstr("marth's Enchanting Overhaul"))
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
    # Pouch scripted MGEF. The DLL owns the pouch entirely (it intercepts the
    # power's cast — plugin.cpp kPouchSpellID), so NO VMAD/MEO_PouchScript: that
    # pex isn't shipped in any 1.0.x zip, and attaching it only spammed a
    # "Cannot open store" Papyrus error every load (audit). Order: EDID,FULL,MDOB,DATA,SNDD,DNAM.
    body =subrec('EDID',zstr("MEO_PouchMGEF"))+subrec('FULL',zstr("Gem Pouch"))
    body+=subrec('MDOB',struct.pack('<I',0))+subrec('DATA',mgef_scripted_self_data())+subrec('SNDD',b'')+subrec('DNAM',struct.pack('<I',0))
    out.write(record('MGEF',FID_POUCH_MGEF,0,body))
    return group('MGEF',out.getvalue())

# ── MISC gems from catalog: assign FormIDs, build display names, record the map ──
# Theme map lives in tools/gen_catalog_header.py (single source of truth) —
# parsed as a literal, not imported, because that script writes files on import.
_ghs = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         'tools', 'gen_catalog_header.py')).read()
GEM_THEME = eval(re.search(r'THEME = ({.*?\n})', _ghs, re.S).group(1))
THEME_STONE = {  # vanilla gemstone per theme — the gem's accent color family
    'FIRE': 'Ruby', 'FROST': 'Sapphire', 'SHOCK': 'Amethyst', 'ARCANE': 'Amethyst',
    'DRAIN': 'Garnet', 'MARTIAL': 'Garnet', 'ROGUISH': 'Emerald', 'HOLY': 'Diamond',
    'UTILITY': 'Emerald',
}

FREEZE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'gem_forms.frozen.json')

def load_frozen_forms():
    """FormID FREEZE: every (gid, level) -> fid pair is immutable (co-saves
    store gem base FormIDs; drift = save corruption + DLL/ESP disagreement).
    Anchor is data/gem_forms.frozen.json — a COMMITTED file, so a fresh clone
    or a wiped out/ can never re-allocate from scratch (m35 audit). Falls back
    to the legacy out/meo_runtime.json only if the committed anchor is absent."""
    frozen={}
    if os.path.exists(FREEZE_PATH):
        for gid,forms in json.load(open(FREEZE_PATH)).items():
            frozen[gid]={int(l):OWN|int(f,16) for l,f in forms.items()}
        return frozen
    legacy=os.path.join('out','SKSE','Plugins','MEO','meo_runtime.json')
    if os.path.exists(legacy):
        for gid,g in json.load(open(legacy)).items():
            if isinstance(g,dict) and 'forms' in g:
                frozen[gid]={int(l):OWN|int(f,16) for l,f in g['forms'].items()}
    return frozen

def write_frozen_forms(gem_form_map):
    """Persist the current (gid,level)->local-fid map to the committed anchor.
    Union with existing so a removed gem's ids stay reserved (never recycled)."""
    existing={}
    if os.path.exists(FREEZE_PATH):
        existing=json.load(open(FREEZE_PATH))
    for gid,levels in gem_form_map.items():
        existing.setdefault(gid,{})
        for lvl,fid in levels.items():
            existing[gid][str(lvl)]=f"0x{fid & 0xFFFFFF:06X}"
    os.makedirs(os.path.dirname(FREEZE_PATH),exist_ok=True)
    json.dump(existing,open(FREEZE_PATH,'w'),indent=1,sort_keys=True)

def allocate_gems():
    """Return (misc_records_bytes, gem_form_map, weapon_fids, armor_fids).
    Frozen (shipped) fids are reused verbatim; only never-shipped gem x level
    pairs allocate new fids. Level count follows the curve length (Muffle=2)."""
    frozen=load_frozen_forms()
    next_fid=max([FID_GEM_BASE-1]+[f for m in frozen.values() for f in m.values()])+1
    out=BytesIO(); gem_form_map={}; weapon_fids=[]; armor_fids=[]
    for gid in sorted(CATALOG, key=lambda g: CATALOG[g]['type_index']):
        g=CATALOG[gid]
        levels=1 if g['single_level'] else min(5,len(g.get('curve', g.get('tiers', []))))  # m36: supports use tiers
        gem_form_map[gid]={}
        for lvl in range(1,levels+1):
            fid=frozen.get(gid,{}).get(lvl)
            if fid is None:
                fid=next_fid; next_fid+=1
            name=g['name'] if levels==1 else f"{g['name']} {ROMAN[lvl]}"
            body =subrec('EDID',zstr(f"MEO_Gem_{gid}_{lvl}"))
            body+=subrec('OBND',b'\x00'*12)+subrec('FULL',zstr(f"{name} Gem" if levels==1 else name))
            # m27 (marth): dropped gems look like real gemstones in the gem's
            # color — vanilla meshes, flawed cut at I-III, flawless at IV-V.
            stone=THEME_STONE.get(GEM_THEME.get(gid,'ARCANE'),'Amethyst')
            mesh=f"Clutter\\Gemstones\\{stone}.nif" if lvl>=4 or levels==1 \
                 else f"Clutter\\Gemstones\\{stone}Flawed.nif"
            body+=subrec('MODL',zstr(mesh))
            # Gems are not sellable (marth): value 0. They live in the hidden
            # pouch so a vendor never lists them; the DLL also zeroes value at
            # load as belt-and-suspenders. Weight 0.1.
            body+=subrec('DATA',struct.pack('<If',0,0.1))
            out.write(record('MISC',fid,0,body))
            gem_form_map[gid][lvl]=fid
            # Support-domain gems (Echo/Conduit/Focus) belong to NEITHER the weapon
            # nor the armor FLST — the DLL keys off the compiled catalog (isSupport),
            # not these lists. Bucketing them under armor was a mis-classification.
            if g['domain']=='weapon': weapon_fids.append(fid)
            elif g['domain']=='armor': armor_fids.append(fid)
    # Curated growth must NEVER reach the reserved pool band — pool fids are
    # frozen contracts with installer assignments on users' machines. If this
    # ever fires, the pool band has to move (a breaking change): stop and think.
    if next_fid >= FID_POOL_BASE:
        raise SystemExit(f"FATAL: curated gem allocation reached 0x{next_fid & 0xFFFFFF:06X} — "
                         f"collides with the reserved pool band at 0x{FID_POOL_BASE & 0xFFFFFF:06X}")
    return out.getvalue(), gem_form_map, weapon_fids, armor_fids, next_fid

POOL_FREEZE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'pool_forms.frozen.json')

def allocate_pool():
    """Phase 3 reserved pool: POOL_SLOTS x 5 levels of placeholder MISC gems
    from 0xB00 (128 slots -> 0xB00-0xD7F). Fids are pure arithmetic
    (base + slot*5 + level-1) but the
    committed anchor data/pool_forms.frozen.json is the CONTRACT both this
    generator and the installer's slot-assignment step read — one source of
    truth, no duplicated constant. Append-only, same discipline as
    gem_forms.frozen.json. Pool forms join NO FLST (economy: conversion-only,
    marth 2026-07-16) and the DLL renames them at runtime, so the baked name
    is only what a user sees if a minted family's calibration goes missing."""
    out=BytesIO(); pool_map={}
    for slot in range(POOL_SLOTS):
        sid=f"pool{slot:02d}"; pool_map[sid]={}
        for lvl in range(1,POOL_LEVELS+1):
            fid=FID_POOL_BASE + slot*POOL_LEVELS + (lvl-1)
            body =subrec('EDID',zstr(f"MEO_Pool{slot:02d}_{lvl}"))
            body+=subrec('OBND',b'\x00'*12)+subrec('FULL',zstr("Uncut Gem"))
            # Neutral mesh; a minted family's real theme arrives at patch time
            # and only names change at runtime — mesh stays cosmetic-neutral.
            mesh="Clutter\\Gemstones\\Amethyst.nif" if lvl>=4 \
                 else "Clutter\\Gemstones\\AmethystFlawed.nif"
            body+=subrec('MODL',zstr(mesh))
            body+=subrec('DATA',struct.pack('<If',0,0.1))
            out.write(record('MISC',fid,0,body))
            pool_map[sid][lvl]=fid
    # Committed anchor: union-write like write_frozen_forms (kept separate so
    # allocate_gems' max-scan never sees pool fids and leapfrogs the band).
    existing={}
    if os.path.exists(POOL_FREEZE_PATH):
        existing=json.load(open(POOL_FREEZE_PATH))
        for sid,levels in existing.items():
            for lvl,f in levels.items():
                want=pool_map.get(sid,{}).get(int(lvl))
                if want is not None and (OWN|int(f,16))!=want:
                    raise SystemExit(f"FATAL: pool anchor mismatch {sid} L{lvl}: "
                                     f"frozen {f} vs computed 0x{want & 0xFFFFFF:06X}")
    for sid,levels in pool_map.items():
        existing.setdefault(sid,{})
        for lvl,fid in levels.items():
            existing[sid][str(lvl)]=f"0x{fid & 0xFFFFFF:06X}"
    # Pool slots are APPEND-ONLY: a POOL_SLOTS reduction changes no computed
    # fid (arithmetic doesn't depend on the count), so without this tripwire
    # shrinking would silently drop shipped slots — and users whose installer
    # assigned a family to a dropped slot would lose those gems on load
    # (unresolvable base fid -> co-save record dropped). Fable review S1.
    for sid,levels in existing.items():
        for lvl in levels:
            if pool_map.get(sid,{}).get(int(lvl)) is None:
                raise SystemExit(f"FATAL: frozen pool entry {sid} L{lvl} no longer emitted — "
                                 f"pool slots are append-only; never shrink POOL_SLOTS/POOL_LEVELS")
    os.makedirs(os.path.dirname(POOL_FREEZE_PATH),exist_ok=True)
    json.dump(existing,open(POOL_FREEZE_PATH,'w'),indent=1,sort_keys=True)
    return out.getvalue(), pool_map

def make_flst(fid,edid,member_fids):
    body=subrec('EDID',zstr(edid))
    for m in member_fids: body+=subrec('LNAM',struct.pack('<I',m))
    return record('FLST',fid,0,body)

def spit_lesser_power():
    return struct.pack('<fIIfIIffI',0.0,0,3,0.0,1,0,0.0,0.0,0)  # type3 lesser power, FF self
def spit_ff_target_actor():
    # SPIT: cost 0, flags 0, type 0 (Spell), chargeTime 0, castType 1 (Fire&Forget),
    # delivery 4 (Target Actor), castDuration 0, range 0, castingPerk 0. The DLL
    # rewrites this spell's single effect (MGEF + magnitude + duration) each tick
    # before casting it on followers (m36 Echo armor share).
    return struct.pack('<fIIfIIffI',0.0,0,0,0.0,1,4,0.0,0.0,0)
def make_echo_spell():
    # One placeholder effect (marker-self MGEF, mag 0, 12s) — swapped at runtime.
    body =subrec('EDID',zstr("MEO_EchoShare"))+subrec('OBND',b'\x00'*12)+subrec('FULL',zstr("Echo"))
    body+=subrec('MDOB',struct.pack('<I',0))+subrec('ETYP',struct.pack('<I',FREF_EQUP_VOICE))
    body+=subrec('DESC',zstr(""))
    body+=subrec('SPIT',spit_ff_target_actor())+subrec('EFID',struct.pack('<I',FID_MARKER_SELF))+subrec('EFIT',struct.pack('<fII',0.0,0,12))
    return record('SPEL',FID_ECHO_SPELL,0,body)
def make_spel():
    body =subrec('EDID',zstr("MEO_GemPouchPower"))+subrec('OBND',b'\x00'*12)+subrec('FULL',zstr("Gem Pouch"))
    body+=subrec('MDOB',struct.pack('<I',0))+subrec('ETYP',struct.pack('<I',FREF_EQUP_VOICE))
    body+=subrec('DESC',zstr("Open the Gem Pouch to socket, view, or remove gems."))
    body+=subrec('SPIT',spit_lesser_power())+subrec('EFID',struct.pack('<I',FID_POUCH_MGEF))+subrec('EFIT',struct.pack('<fII',0.0,0,0))
    return record('SPEL',FID_POUCH_SPELL,0,body)

def make_cont():
    # Hidden Gem Pouch container: no model, no contents, no respawn flags.
    # The DLL spawns a temporary reference of it and activates that ref to
    # open a real two-pane ContainerMenu (M5). DATA = flags u8 + weight f32.
    body =subrec('EDID',zstr("MEO_GemPouchCont"))+subrec('OBND',b'\x00'*12)
    body+=subrec('FULL',zstr("Gem Pouch"))+subrec('DATA',struct.pack('<Bf',0,0.0))
    return group('CONT',record('CONT',FID_POUCH_CONT,0,body))

# DNAM flags: 0x0001 = Start Game Enabled, 0x0004 = Run Once (layout verified
# against MRO's shipped, in-game-proven MCM quest — flags live at offset 4).
def qust_dnam(flags=0x0001|0x0004): return struct.pack('<B',20)+b'\x01\x00\xff'+struct.pack('<HHI',flags,0,0)
def make_startup_quest():
    # No VMAD/MEO_StartupQuest: the DLL grants the Gem Pouch power itself
    # (plugin.cpp AddSpell), that pex ships in no 1.0.x zip, and attaching it
    # only spammed a "Cannot open store" Papyrus error every load (audit). The
    # empty run-once quest record is kept so its FormID stays reserved (frozen).
    body =subrec('EDID',zstr("MEO_StartupQuest"))+subrec('FULL',zstr("MEO Startup"))
    body+=subrec('DNAM',qust_dnam())+subrec('NEXT',b'')+subrec('ANAM',struct.pack('<I',0))
    return record('QUST',FID_STARTUP_QUEST,0,body)
def make_mcm_quest():
    # Start-game-enabled (not run-once) quest whose only job is to carry the
    # MEO_MCM script (extends MCM Helper's MCM_ConfigBase). Zero VMAD properties:
    # MCM Helper renders from Data/MCM/Config/MEO/config.json and persists to
    # Data/MCM/Settings/MEO.ini; the DLL reads that INI. modName is derived by
    # MCM Helper from this quest's plugin stem ("MEO"), so no property needed.
    vmad=VMADBuilder(); vmad.add_script("MEO_MCM",[])
    body =subrec('EDID',zstr("MEO_MCMQuest"))+subrec('FULL',zstr("MEO MCM"))+subrec('VMAD',vmad.build())
    body+=subrec('DNAM',qust_dnam(0x0001))+subrec('NEXT',b'')+subrec('ANAM',struct.pack('<I',0))
    return record('QUST',FID_MCM_QUEST,0,body)
def make_qust():
    return group('QUST',make_startup_quest()+make_mcm_quest())

# MEO perks (DESIGN §6). Minimal marker perks — no entry points; the DLL reads
# HasPerk and applies effects. Order/index FROZEN (co-save-independent, but the
# DLL looks them up by these local FormIDs). The C# installer wires these into
# the load order's enchanting tree; interim the DLL auto-grants them by skill.
# (edid, name, desc, skill_req, next_slot). skill_req becomes a CTDA
# GetBaseActorValue(Enchanting) >= req on the record (the skill menu greys the
# perk until it passes) and mirrors the DLL's interim auto-grant thresholds.
# next_slot chains ranked perks via NNAM (Requiem convention) so the five
# Attunements display as one 5-rank node in the installer-written perk tree.
MEO_PERKS = [
    ("MEO_Perk_Attunement1", "Gem Attunement", "Socketed gems are 5% more potent.",   0,  1),
    ("MEO_Perk_Attunement2", "Gem Attunement", "Socketed gems are 10% more potent.", 20,  2),
    ("MEO_Perk_Attunement3", "Gem Attunement", "Socketed gems are 15% more potent.", 40,  3),
    ("MEO_Perk_Attunement4", "Gem Attunement", "Socketed gems are 20% more potent.", 60,  4),
    ("MEO_Perk_Attunement5", "Gem Attunement", "Socketed gems are 25% more potent.", 80, None),
    ("MEO_Perk_GemCutter",   "Gem Cutter",     "Socketed gems earn 50% more Gem XP.", 20, None),
    ("MEO_Perk_SoulFeeder",  "Soul Feeder",    "Soul gems fed to gems at an enchanting station are twice as potent.", 40, None),
    ("MEO_Perk_TwinnedFitting","Twinned Fitting","Chest armor can hold a second linked gem.", 70, None),   # 0x817
    ("MEO_Perk_MasterJeweler", "Master Jeweler", "Weapons can hold a second linked gem.", 100, None),      # 0x818
    ("MEO_Perk_Pyrestone",  "Pyrestone Affinity",  "Fire and Chaos gems are 25% stronger.", 30, None),     # 0x819
    ("MEO_Perk_Froststone", "Froststone Affinity", "Frost and Chaos gems are 25% stronger.", 40, None),    # 0x81A
    ("MEO_Perk_Stormstone", "Stormstone Affinity", "Shock and Chaos gems are 25% stronger.", 50, None),    # 0x81B
    ("MEO_Perk_FacetInsight","Facet Insight","Skill and attribute armor gems are 25% stronger.", 50, None),# 0x81C
]
def ctda_skill_req(av_index, value):
    """CTDA: GetBaseActorValue(av) >= value, run on subject. 32 bytes."""
    return struct.pack('<B3xfH2xiiiii',
                       0x60,        # operator GreaterThanOrEqual (3<<5)
                       float(value),
                       277,         # function index GetBaseActorValue
                       av_index, 0, # param1 = actor value, param2 unused
                       0, 0, -1)    # runOn Subject, reference, param3
def make_perks():
    out=BytesIO()
    data=struct.pack('<BBBBB',0,0,1,1,0)  # trait0 level0 ranks1 playable1 hidden0
    for i,(edid,name,desc,req,nxt) in enumerate(MEO_PERKS):
        body =subrec('EDID',zstr(edid))+subrec('FULL',zstr(name))+subrec('DESC',zstr(desc))
        if req > 0:
            body+=subrec('CTDA',ctda_skill_req(23,req))  # AV 23 = Enchanting
        body+=subrec('DATA',data)
        if nxt is not None:
            body+=subrec('NNAM',struct.pack('<I',FID_PERK_BASE+nxt))
        out.write(record('PERK',FID_PERK_BASE+i,0,body))
    return group('PERK',out.getvalue())

def write_runtime_json(out_dir, gem_form_map):
    """Per-gem runtime data the socket script reads via JsonUtil. Lives under
    Data/SKSE/Plugins/MEO/ so JsonUtil resolves 'MEO/meo_runtime.json'."""
    rt={'_meta':{'count':len(CATALOG),'version':1}}
    for gid,g in CATALOG.items():
        rt[gid]={
            'name':g['name'],'type_index':g['type_index'],'domain':g['domain'],
            'tier':g['power_tier'],'xp_mult':g['xp_mult'],'single_level':g['single_level'],
            'curve':g.get('curve', g.get('tiers', [])),'mgef_refs':g['mgef_refs'],
            # LOCAL FormID (lower 24 bits) for Game.GetFormFromFile(id,"MEO.esp")
            'forms':{str(l):f"0x{fid & 0xFFFFFF:06X}" for l,fid in gem_form_map[gid].items()},
        }
    d=os.path.join(out_dir,'SKSE','Plugins','MEO'); os.makedirs(d,exist_ok=True)
    json.dump(rt,open(os.path.join(d,'meo_runtime.json'),'w'),indent=1)

# ── MCM (MCM Helper): config.json + default settings INI ──
# Single source of truth for the user-facing tunables. Each key's DEFAULT must
# match the matching g_* default in native/plugin.cpp so a fresh install (no
# INI yet) and the INI agree. section = MCM Helper INI section (the DLL ignores
# section headers; keys are globally unique). type: 'f' float slider, 'b' toggle.
MCM_TUNABLES=[
    # (page, section, key, label, help, type, default, min, max, step, fmt)
    ("Loot & Spawns","Loot","fGemDropChance","Gem drop chance per kill",
     "Chance that killing an actor drops a lootable gem on the corpse.",
     'f',0.03,0.0,0.25,0.005,"{2}"),
    ("Loot & Spawns","Loot","fWorldSocketChance","World weapon socket chance",
     "Chance that a weapon found in the world is already socketed with a gem.",
     'f',0.05,0.0,0.25,0.005,"{2}"),
    ("Loot & Spawns","Loot","fGemLevel2Chance","Level II spawn chance",
     "Chance that a spawned gem (corpse drop or world socket) is born at level II instead of I.",
     'f',0.02,0.0,0.20,0.005,"{2}"),
    ("Loot & Spawns","Loot","fNPCSocketChance","Enemy socketed gear chance",
     "Chance that a humanoid enemy spawns wearing one socketed piece, themed to its archetype (mage, warrior, rogue, undead). Rare gems are rarer on enemies than in world drops.",
     'f',0.05,0.0,0.25,0.005,"{2}"),
    ("Loot & Spawns","Loot","fVendorGemChance","Vendor gem chance",
     "Per stock item, the chance a vendor also carries a loose gem for sale this restock (up to 3).",
     'f',0.04,0.0,0.25,0.005,"{2}"),
    # Page is "Loot & Spawns" (this governs what gets converted), but the INI
    # SECTION stays "UI" — the section is the settings key the DLL parses and
    # players' saved MCM.ini already carry, so moving the page must never move it.
    ("Loot & Spawns","UI","bConvertPlayerEnchants","Convert player-enchanted gear to gems",
     "ON (default) turns enchantments you placed yourself into the matching socketed gems, so no enchanted gear slips past MEO. Turn this OFF if you use a mod that MOVES or UPGRADES enchantments between items (MEO cannot tell a transferred enchantment apart from one you made, and would convert it). Only affects items MEO has not already socketed; nothing is ever converted if any effect would be lost.",
     'b',1,None,None,None,None),
    ("Loot & Spawns","UI","bAllowUncoveredGenerics","Allow uncovered enchanted loot",
     "ON (default) lets ordinary enchanted loot MEO has no matching gem for keep its enchantment and appear as normal. Turn this OFF to strip those enchantments so nothing enchanted slips past MEO at all. Unique items, artifacts, and quest items are ALWAYS left untouched either way. Items already stripped stay plain even if you turn this back ON — new drops resume enchanted.",
     'b',1,None,None,None,None),
    ("XP & Balance","XP","fXPPerKill","Gem XP rate",
     "Multiplier on the Gem XP your socketed gems earn from each kill.",
     'f',1.0,0.25,5.0,0.25,"{2}"),
    ("XP & Balance","XP","fBossXPMult","Boss / dragon XP multiplier",
     "Extra Gem XP multiplier applied to boss and dragon kills.",
     'f',10.0,1.0,25.0,1.0,"{0}"),
    ("XP & Balance","XP","fMagnitudeMult","Gem power scale",
     "Master multiplier on every gem's effect magnitude, applied when a gem is socketed or levels up. Existing sockets update on their next re-stamp.",
     'f',1.0,0.5,2.0,0.05,"{2}"),
    ("XP & Balance","XP","fEnchSkillXP","Enchanting skill XP from souls",
     "Multiplier (1.0 = tuned default) on the Enchanting SKILL experience gained per soul fed to a gem (base 11/26/56/94/150 by soul size). Soul feeding is this list's enchanting practice.",
     'f',1.0,0.0,3.0,0.05,"{2}"),
    # v1.0.6: RENAMED from fGemXpSkillXP. Pre-1.0.6 that key was an ABSOLUTE rate
    # (default 0.01, max 0.05); v1.0.6 makes it a 1.0-default ×multiplier. Renaming
    # orphans any persisted legacy value so the DLL can't misread 0.01 as "0.01×".
    # plugin.cpp's ApplyIniFile branch must parse this exact key: "fGemKillXpMult".
    ("XP & Balance","XP","fGemKillXpMult","Enchanting XP from gem kills",
     "Multiplier (1.0 = tuned default) on the tiny Enchanting SKILL experience your socketed gems' kills trickle into Enchanting, so combat slowly trains it. Counted once per kill, not per gem. Toward 0 = slower, up to 10x = faster.",
     'f',1.0,0.0,10.0,0.5,"{1}"),
    ("XP & Balance","XP","fDiscoverSkillXP","Enchanting XP from new gems",
     "Multiplier (1.0 = tuned default) on the one-time Enchanting SKILL experience for studying a NEW gem family. Discovering a stockpile of new families at once gives a burst; lower this if Enchanting jumps too much on a big haul.",
     'f',1.0,0.0,5.0,0.5,"{1}"),
    ("XP & Balance","UI","bXPNotify","Show Gem XP notifications",
     "Show a corner message when your gems gain Gem XP on a kill.",
     'b',1,None,None,None,None),
    ("XP & Balance","UI","bFullGemNames","Full gem names on items",
     "OFF (default) shortens gem names in socketed-item titles so multi-gem names stay readable — drops \"Fortify\", trims \"Damage\" (Fire II, not Fire Damage II), and abbreviates \"Resist\" to \"Res\". ON restores the full effect names. Applies to worn/socketed gear on the next rebuild (reload or re-equip); loose gems in the pouch always keep their full names.",
     'b',0,None,None,None,None),
    ("XP & Balance","UI","bStationTakeover","Gem menu replaces enchanting table",
     "Enchanting stations open the gem menu (soul feeding and gem destruction) instead of the vanilla enchanting menu. MEO replaces enchanting entirely; disable only to overlay the vanilla menu instead.",
     'b',1,None,None,None,None),
    ("XP & Balance","UI","bTemperNoPerk","Temper socketed gear without Arcane Blacksmith",
     "Socketed weapons and armor carry a gem enchantment, which normally makes the grindstone and workbench require the Arcane Blacksmith perk to improve them. ON grants that perk so socketed gear improves freely; since MEO converts generic enchanted loot into sockets, only artifacts still need the perk. OFF revokes MEO's grant (never a perk you earned yourself).",
     'b',1,None,None,None,None),
    # type 'e' = enum dropdown; the options list rides in the min slot.
    ("XP & Balance","UI","iMenuStyle","Gem menu style",
     "Visual skin for the gem socketing menu. Applies the next time the menu opens.",
     'e',0,["Ebony & Brass","Dwemer Parchment","Soul Cairn","Quicksilver"],None,None,None),
    ("Debug","Debug","bDebugAllPerks","Grant all MEO perks (testing)",
     "Testing aid: forces every MEO perk ON — Attunement V, BOTH dual-socket perks (chest + main-hand weapon), all elemental affinities, Facet Insight, Gem Cutter, and Soul Feeder — without grinding Enchanting or spending points. It does not add real perks to your character; toggling OFF reverts to what you actually hold. Applies on menu close; re-socket or reload for worn gear to pick up new magnitudes.",
     'b',0,None,None,None,None),
    ("Debug","Debug","bEnableLogging","Write the MEO log file",
     "Writes MEO.log (in the SKSE logs folder) with MEO's internal diagnostics. ON by default — leave it on if you might report an issue, since the log is what pins down bugs. Turn OFF to stop all MEO file logging. Applies on menu close.",
     'b',1,None,None,None,None),
]

def write_mcm_files(out_dir):
    """config.json (Data/MCM/Config/MEO/) + default settings INI
    (Data/MCM/Settings/MEO.ini). MCM Helper reads the config, persists user
    choices to the INI; the DLL reads the INI (and re-reads it on menu close)."""
    pages={}
    for page,section,key,label,help_,typ,dflt,mn,mx,step,fmt in MCM_TUNABLES:
        pages.setdefault(page,[])
        if typ=='f':
            ctrl={"id":f"{key}:{section}","text":label,"type":"slider","help":help_,
                  "valueOptions":{"min":mn,"max":mx,"step":step,"formatString":fmt,
                                  "sourceType":"ModSettingFloat","defaultValue":dflt}}
        elif typ=='e':
            # MCM Helper binds enum values via the settings INI; defaultValue
            # inside valueOptions is not part of the proven shape (working
            # mods omit it and carry shortNames) — the INI declares defaults.
            ctrl={"id":f"{key}:{section}","text":label,"type":"enum","help":help_,
                  "valueOptions":{"options":mn,"shortNames":mn,"sourceType":"ModSettingInt"}}
        else:
            ctrl={"id":f"{key}:{section}","text":label,"type":"toggle","help":help_,
                  "valueOptions":{"sourceType":"ModSettingBool","defaultValue":bool(dflt)}}
        pages[page].append(ctrl)
    # Version readout at the top of the Debug page (MRO-style: stamped at build
    # time, shown as plain static text — no ModSetting/Papyrus round-trip to render
    # blank). Sourced from kMEOVersion in native/plugin.cpp, so the DLL load log,
    # the console print, and this display all share one constant.
    ver = read_meo_version()
    pages.setdefault("Debug",[]).insert(0,
        {"text":"Version","type":"text",
         "help":"MEO version, stamped from the build. Matches the MEO.dll built from the same commit.",
         "valueOptions":{"value":f"v{ver}"}})
    config={"modName":"MEO","displayName":"marth Enchanting Overhaul",
            "minMcmVersion":9,"cursorFillMode":"topToBottom",
            "pages":[{"pageDisplayName":p,"cursorFillMode":"topToBottom",
                      "content":[{"text":p,"type":"header"}]+ctrls}
                     for p,ctrls in pages.items()]}
    cdir=os.path.join(out_dir,'MCM','Config','MEO'); os.makedirs(cdir,exist_ok=True)
    json.dump(config,open(os.path.join(cdir,'config.json'),'w'),indent='\t')
    # Default settings INI (MCM Helper format: BOM + [Section] + key = value).
    by_sec={}
    for _,section,key,_l,_h,typ,dflt,*_ in MCM_TUNABLES:
        by_sec.setdefault(section,[]).append((key,dflt,typ))
    lines=[]
    for section,items in by_sec.items():
        lines.append(f"[{section}]")
        for key,dflt,typ in items:
            lines.append(f"{key} = {int(dflt)}" if typ in 'be' else f"{key} = {float(dflt):.6f}")
    sdir=os.path.join(out_dir,'MCM','Settings'); os.makedirs(sdir,exist_ok=True)
    with open(os.path.join(sdir,'MEO.ini'),'w',encoding='utf-8-sig') as f:
        f.write("\n".join(lines)+"\n")

def main():
    out_dir=sys.argv[1] if len(sys.argv)>1 else "out"; os.makedirs(out_dir,exist_ok=True)
    misc_bytes, gem_form_map, weapon_fids, armor_fids, next_local = allocate_gems()
    # Mentor gem (unique support: doubles Gem XP; DLL-managed, not in FLSTs).
    mentor=subrec('EDID',zstr("MEO_Gem_mentor"))+subrec('OBND',b'\x00'*12)
    mentor+=subrec('FULL',zstr("Mentor Gem"))+subrec('DATA',struct.pack('<If',750,0.1))
    misc_bytes=record('MISC',FID_MENTOR_GEM,0,mentor)+misc_bytes
    # Phase 3 reserved pool (after curated gems so the MISC group stays in
    # ascending-fid order); HEDR next-id must clear the pool band too.
    pool_bytes, pool_map = allocate_pool()
    misc_bytes+=pool_bytes
    next_local=max(next_local, FID_POOL_BASE + POOL_SLOTS*POOL_LEVELS)
    esp=BytesIO()
    esp.write(make_tes4(next_local & 0xFFFFFF))
    esp.write(make_mgefs())
    esp.write(group('MISC',misc_bytes))
    esp.write(make_cont())
    esp.write(group('SPEL',make_spel()+make_echo_spell()))
    flsts=(make_flst(FID_FLST_ALL,"MEO_AllGems",weapon_fids+armor_fids)
          +make_flst(FID_FLST_WEAPON,"MEO_WeaponGems",weapon_fids)
          +make_flst(FID_FLST_ARMOR,"MEO_ArmorGems",armor_fids))
    esp.write(group('FLST',flsts))
    esp.write(make_perks())
    esp.write(make_qust())
    data=esp.getvalue()
    with open(os.path.join(out_dir,"MEO.esp"),'wb') as f: f.write(data)
    write_runtime_json(out_dir, gem_form_map)
    write_frozen_forms(gem_form_map)  # m35: keep the committed FormID anchor current
    write_mcm_files(out_dir)
    ngems=len(CATALOG); nmisc=len(weapon_fids)+len(armor_fids)
    print(f"Written: {out_dir}/MEO.esp ({len(data):,} bytes)")
    print(f"  MGEF x3  (marker contact+self, pouch)   CONT x1  SPEL x1  QUST x2 (startup+MCM)  FLST x3")
    print(f"  MCM: {out_dir}/MCM/Config/MEO/config.json + {out_dir}/MCM/Settings/MEO.ini ({len(MCM_TUNABLES)} tunables)")
    print(f"  MISC x{nmisc}  ({ngems} gems: {len(weapon_fids)} weapon-domain + {len(armor_fids)} armor-domain forms)")
    print(f"  Reserved pool: {POOL_SLOTS} slots x {POOL_LEVELS} = {POOL_SLOTS*POOL_LEVELS} MISC "
          f"(0x{FID_POOL_BASE & 0xFFFFFF:06X}-0x{(FID_POOL_BASE + POOL_SLOTS*POOL_LEVELS - 1) & 0xFFFFFF:06X}, "
          f"anchor data/pool_forms.frozen.json)")
    print(f"Written: {out_dir}/SKSE/Plugins/{RUNTIME_REL} ({ngems} gems)")

if __name__=="__main__": main()
