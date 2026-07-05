#!/usr/bin/env python3
"""
MEO_GenerateESP.py  — P0 prototype plugin generator for Marth's Enchanting Overhaul.

P0's single purpose: validate the "item is the database" architecture in-game.
It builds the smallest plugin that lets a Papyrus power write a runtime
enchantment (real Fire effect + a hidden marker effect) onto the worn weapon,
read the marker back, rebuild it, and remove it — so we can test persistence
across save/load, re-equip, and rebuild. See Docs/DESIGN.md P0 and the
validation matrix in Docs/TESTING.md.

Binary helpers forked from MRO's proven generator (../Requiem-modification/).
Every record layout here was checked against a vanilla Skyrim.esm donor:
  - MGEF marker  = clone of AbBlank (0x0EB7EA), inert Script archetype, no VMAD
  - MGEF pouch   = scripted self MGEF (VMAD -> MEO_PouchScript)
  - MISC gem     = GemSapphireFlawless layout (DATA = value u32 + weight f32)
  - SPEL power   = PowerNordBattleCry SPIT (type 2 = Power)
  - Fire effect  = referenced vanilla EnchFireDamageFFContact (0x0004605A)

Run: python3 MEO_GenerateESP.py [output_dir]   (default: ./out)
"""

import struct
import sys
import os
from io import BytesIO

# ──────────────────────────────────────────────────────────────────────────────
# FormIDs
# Single master in P0: Skyrim.esm (master index 0). Our own file index is 0x01.
# Own records carry the 0x01 prefix AND stay in 0x800-0xFFF so the plugin can be
# ESL-flagged later. A 0x00 prefix would inject into Skyrim.esm's FormID space
# and collide with real records (this exact mistake cost MRO days).
# ──────────────────────────────────────────────────────────────────────────────
OWN = 0x01000000

FREF_PLAYER   = 0x00000014  # Skyrim.esm PlayerRef
FREF_FIRE_MGEF = 0x0004605A  # Skyrim.esm EnchFireDamageFFContact (real fire effect)

FID_MARKER_MGEF   = OWN | 0x800
FID_POUCH_MGEF    = OWN | 0x801
FID_POUCH_SPELL   = OWN | 0x802
FID_STARTUP_QUEST = OWN | 0x803
FID_GEM_FLST      = OWN | 0x804
FID_GEM_BASE      = OWN | 0x810  # MEO_GemFire1..5 => 0x810..0x814

NUM_GEM_LEVELS = 5
GEMTYPE_FIRE   = 1              # marker magnitude = gemType*8 + level

# Vanilla fire enchant magnitudes per tier (01..05), seeds the level curve.
FIRE_MAG = {1: 5.0, 2: 10.0, 3: 15.0, 4: 20.0, 5: 25.0}

# ──────────────────────────────────────────────────────────────────────────────
# Binary helpers (forked from MRO_GenerateESP.py — proven byte-for-byte valid)
# ──────────────────────────────────────────────────────────────────────────────
FORM_VERSION = 44  # Skyrim SE

def subrec(rtype: str, data: bytes) -> bytes:
    assert len(rtype) == 4
    return rtype.encode('ascii') + struct.pack('<H', len(data)) + data

def record(rtype: str, formid: int, flags: int, data: bytes) -> bytes:
    assert len(rtype) == 4
    hdr = (rtype.encode('ascii')
           + struct.pack('<I', len(data))
           + struct.pack('<I', flags)
           + struct.pack('<I', formid)
           + struct.pack('<I', 0)             # VCI1
           + struct.pack('<H', FORM_VERSION)
           + struct.pack('<H', 0))            # VCI2
    return hdr + data

def group(label: str, records_data: bytes) -> bytes:
    assert len(label) == 4
    total = 24 + len(records_data)
    hdr = (b'GRUP'
           + struct.pack('<I', total)
           + label.encode('ascii')
           + struct.pack('<i', 0)   # group type 0 (top-level)
           + struct.pack('<I', 0)   # stamp
           + struct.pack('<I', 0))  # unknown
    return hdr + records_data

def zstr(s: str) -> bytes:
    return s.encode('ascii') + b'\x00'

# ── VMAD (Skyrim SE object format 2) ──
VMAD_VERSION = 5
OBJECT_FORMAT = 2

class VMADBuilder:
    def __init__(self):
        self.scripts = []
    def add_script(self, name: str, props: list):
        self.scripts.append((name, props))
    def build(self) -> bytes:
        buf = BytesIO()
        buf.write(struct.pack('<H', VMAD_VERSION))
        buf.write(struct.pack('<H', OBJECT_FORMAT))
        buf.write(struct.pack('<H', len(self.scripts)))
        for name, props in self.scripts:
            enc = name.encode('ascii')
            buf.write(struct.pack('<H', len(enc))); buf.write(enc)
            buf.write(struct.pack('<B', 0))          # status
            buf.write(struct.pack('<H', len(props)))
            for pname, pval in props:
                penc = pname.encode('ascii')
                buf.write(struct.pack('<H', len(penc))); buf.write(penc)
                buf.write(bytes([pval[0]]))          # type byte
                buf.write(struct.pack('<B', 1))      # status: edited
                buf.write(pval[1:])
        return buf.getvalue()

def prop_obj(formid: int) -> bytes:
    return bytes([1]) + struct.pack('<H', 0) + struct.pack('<h', -1) + struct.pack('<I', formid)
def prop_str(s: str) -> bytes:
    enc = s.encode('ascii'); return bytes([2]) + struct.pack('<H', len(enc)) + enc
def prop_int(v: int) -> bytes:
    return bytes([3]) + struct.pack('<i', v)
def prop_float(v: float) -> bytes:
    return bytes([4]) + struct.pack('<f', v)
def prop_bool(v: bool) -> bytes:
    return bytes([5]) + struct.pack('<B', 1 if v else 0)
def prop_obj_array(formids: list) -> bytes:
    # Array of Object (type 11 = Object array in objformat 2)
    body = struct.pack('<I', len(formids))
    for f in formids:
        body += struct.pack('<H', 0) + struct.pack('<h', -1) + struct.pack('<I', f)
    return bytes([11]) + body

# ──────────────────────────────────────────────────────────────────────────────
# TES4 header
# ──────────────────────────────────────────────────────────────────────────────
def make_tes4() -> bytes:
    masters = ["Skyrim.esm"]
    next_id = (FID_GEM_BASE + NUM_GEM_LEVELS) & 0xFFFFFF
    hedr = struct.pack('<f', 1.70) + struct.pack('<I', 100) + struct.pack('<I', next_id)
    body  = subrec('HEDR', hedr)
    body += subrec('CNAM', zstr("Marth"))
    body += subrec('SNAM', zstr("Marth's Enchanting Overhaul - P0 prototype"))
    for m in masters:
        body += subrec('MAST', zstr(m))
        body += subrec('DATA', struct.pack('<Q', 0))
    return record('TES4', 0x00000000, 0x00000200, body)

# ──────────────────────────────────────────────────────────────────────────────
# MGEF
# ──────────────────────────────────────────────────────────────────────────────
def mgef_marker_data() -> bytes:
    """Inert marker: Script archetype (no VMAD), Fire-and-forget + Contact so it
    rides a weapon enchant. Field-for-field equivalent to vanilla AbBlank
    (0x8000 flags, archetype 1, AVs -1, DualCastScale 1.0) with cast/delivery
    set for a weapon effect. Verified against AbBlank's real bytes at build."""
    d = bytearray(152)
    struct.pack_into('<I', d, 0, 0x8000)       # flags (matches AbBlank)
    struct.pack_into('<I', d, 12, 0xFFFFFFFF)  # MagicSkill none
    struct.pack_into('<I', d, 16, 0xFFFFFFFF)  # MinSkill none
    struct.pack_into('<I', d, 64, 1)           # archetype 1 = Script
    struct.pack_into('<i', d, 68, -1)          # primary AV none
    struct.pack_into('<I', d, 80, 1)           # cast: FireForget
    struct.pack_into('<I', d, 84, 1)           # delivery: Contact
    struct.pack_into('<i', d, 88, -1)          # secondary AV none
    struct.pack_into('<f', d, 112, 1.0)        # DualCastScale
    return bytes(d)

def mgef_scripted_self_data() -> bytes:
    """Script archetype, Constant, Self — a 'cast to run script' effect for the power."""
    d = bytearray(152)
    struct.pack_into('<I', d, 12, 0xFFFFFFFF)  # MagicSkill none
    struct.pack_into('<I', d, 16, 0xFFFFFFFF)  # MinSkill none
    struct.pack_into('<I', d, 64, 1)           # archetype 1 = Script
    struct.pack_into('<i', d, 68, -1)          # primary AV none
    struct.pack_into('<I', d, 80, 1)           # cast: FireForget (power delivery)
    struct.pack_into('<I', d, 84, 0)           # delivery: Self
    struct.pack_into('<i', d, 88, -1)          # secondary AV none
    struct.pack_into('<f', d, 112, 1.0)        # DualCastScale
    struct.pack_into('<I', d, 0, 0x8000)       # flags: Hide-in-UI-ish (matches AbBlank base flag)
    return bytes(d)

def make_mgefs() -> bytes:
    out = BytesIO()

    # ── Marker MGEF: clone of AbBlank, Fire-and-forget + Contact so it rides a
    #    weapon enchant. Inert (Script archetype, no VMAD). Its EFIT magnitude
    #    carries gemType*8+level and is read back via GetNthEffectMagnitude. ──
    body  = subrec('EDID', zstr("MEO_MarkerMGEF"))
    body += subrec('FULL', zstr(""))                     # empty name = invisible
    body += subrec('MDOB', struct.pack('<I', 0))
    body += subrec('DATA', mgef_marker_data())
    body += subrec('SNDD', b'')
    body += subrec('DNAM', struct.pack('<I', 0))
    out.write(record('MGEF', FID_MARKER_MGEF, 0, body))

    # ── Pouch MGEF: scripted self effect; the power casts it, script opens menu ──
    vmad = VMADBuilder()
    vmad.add_script("MEO_PouchScript", [
        ("PlayerRef",  prop_obj(FREF_PLAYER)),
        ("FireEffect", prop_obj(FREF_FIRE_MGEF)),
        ("Marker",     prop_obj(FID_MARKER_MGEF)),
        ("GemForms",   prop_obj_array([FID_GEM_BASE + i for i in range(NUM_GEM_LEVELS)])),
    ])
    # Subrecord order copied from vanilla scripted MGEFs (EnchDragonPriestUltraMaskEffect):
    # EDID, VMAD, FULL, MDOB, DATA, SNDD, DNAM.
    body  = subrec('EDID', zstr("MEO_PouchMGEF"))
    body += subrec('VMAD', vmad.build())
    body += subrec('FULL', zstr("Gem Pouch"))
    body += subrec('MDOB', struct.pack('<I', 0))
    body += subrec('DATA', mgef_scripted_self_data())
    body += subrec('SNDD', b'')
    body += subrec('DNAM', struct.pack('<I', 0))
    out.write(record('MGEF', FID_POUCH_MGEF, 0, body))

    return group('MGEF', out.getvalue())

# ──────────────────────────────────────────────────────────────────────────────
# MISC — the Fire gems (I..V). Layout copied from GemSapphireFlawless.
# ──────────────────────────────────────────────────────────────────────────────
def make_miscs() -> bytes:
    out = BytesIO()
    roman = {1: "I", 2: "II", 3: "III", 4: "IV", 5: "V"}
    for lvl in range(1, NUM_GEM_LEVELS + 1):
        body  = subrec('EDID', zstr("MEO_GemFire%d" % lvl))
        body += subrec('OBND', b'\x00' * 12)
        body += subrec('FULL', zstr("Fire Gem %s" % roman[lvl]))
        body += subrec('DATA', struct.pack('<If', 100 * lvl, 0.1))  # value, weight
        out.write(record('MISC', FID_GEM_BASE + (lvl - 1), 0, body))
    return group('MISC', out.getvalue())

# ──────────────────────────────────────────────────────────────────────────────
# SPEL — Gem Pouch power (type 2 = Power; SPIT verified vs PowerNordBattleCry)
# ──────────────────────────────────────────────────────────────────────────────
def spit_power() -> bytes:
    return (struct.pack('<f', 0.0)   # cost
            + struct.pack('<I', 0)    # flags
            + struct.pack('<I', 2)    # type 2 = Power
            + struct.pack('<f', 0.0)  # charge time
            + struct.pack('<I', 1)    # castType 1 = FireForget
            + struct.pack('<I', 0)    # delivery 0 = Self
            + struct.pack('<f', 0.0)  # cast duration
            + struct.pack('<f', 0.0)  # range
            + struct.pack('<I', 0))   # perk

def make_spels() -> bytes:
    body  = subrec('EDID', zstr("MEO_GemPouchPower"))
    body += subrec('FULL', zstr("Gem Pouch"))
    body += subrec('SPIT', spit_power())
    body += subrec('EFID', struct.pack('<I', FID_POUCH_MGEF))
    body += subrec('EFIT', struct.pack('<fII', 0.0, 0, 0))
    return group('SPEL', record('SPEL', FID_POUCH_SPELL, 0, body))

# ──────────────────────────────────────────────────────────────────────────────
# FLST — gem form ladder handed to the script
# ──────────────────────────────────────────────────────────────────────────────
def make_flsts() -> bytes:
    body = subrec('EDID', zstr("MEO_FireGemList"))
    for i in range(NUM_GEM_LEVELS):
        body += subrec('LNAM', struct.pack('<I', FID_GEM_BASE + i))
    return group('FLST', record('FLST', FID_GEM_FLST, 0, body))

# ──────────────────────────────────────────────────────────────────────────────
# QUST — startup quest (Start Game Enabled + Run Once). Grants the pouch power.
# Run Once quests start without a SEQ file, so P0 needs no SEQ.
# ──────────────────────────────────────────────────────────────────────────────
QUST_FLAGS_STARTUP = 0x0001 | 0x0004  # SGE + Run Once

def qust_dnam(priority=20, flags=QUST_FLAGS_STARTUP) -> bytes:
    return (struct.pack('<B', priority)
            + b'\x01\x00\xff'
            + struct.pack('<H', flags)
            + struct.pack('<H', 0)
            + struct.pack('<I', 0))

def make_qusts() -> bytes:
    vmad = VMADBuilder()
    vmad.add_script("MEO_StartupQuest", [
        ("PlayerRef",     prop_obj(FREF_PLAYER)),
        ("GemPouchPower", prop_obj(FID_POUCH_SPELL)),
    ])
    body  = subrec('EDID', zstr("MEO_StartupQuest"))
    body += subrec('FULL', zstr("MEO Startup"))
    body += subrec('VMAD', vmad.build())
    body += subrec('DNAM', qust_dnam())
    body += subrec('NEXT', b'')
    body += subrec('ANAM', struct.pack('<I', 0))
    return group('QUST', record('QUST', FID_STARTUP_QUEST, 0, body))

# ──────────────────────────────────────────────────────────────────────────────
def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "MEO.esp")

    esp = BytesIO()
    esp.write(make_tes4())
    esp.write(make_mgefs())
    esp.write(make_miscs())
    esp.write(make_spels())
    esp.write(make_flsts())
    esp.write(make_qusts())

    data = esp.getvalue()
    with open(out_path, 'wb') as f:
        f.write(data)

    print(f"Written: {out_path} ({len(data):,} bytes)")
    print("Records:")
    print(f"  TES4  header (master: Skyrim.esm)")
    print(f"  MGEF  x2   (MEO_MarkerMGEF [inert, contact], MEO_PouchMGEF [scripted self])")
    print(f"  MISC  x{NUM_GEM_LEVELS}   (MEO_GemFire1..{NUM_GEM_LEVELS})")
    print(f"  SPEL  x1   (MEO_GemPouchPower, type 2 Power)")
    print(f"  FLST  x1   (MEO_FireGemList)")
    print(f"  QUST  x1   (MEO_StartupQuest, SGE + Run Once)")

if __name__ == "__main__":
    main()
