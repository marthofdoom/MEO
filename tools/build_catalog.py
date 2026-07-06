#!/usr/bin/env python3
"""Build the canonical MEO gem catalog -> data/gem_catalog.json.

PORTABILITY (dynamic-or-drop): the gem SET and effects derive only from the five
base masters (Skyrim/Update/Dawnguard/HearthFires/Dragonborn) via parse_ench,
which every SE install has -> identical catalog on any load order. The MAGNITUDES
come from data/gem_balance.json (the FOMOD-selectable 'Requiem' baseline) and are
tunable at runtime, never a fixed bake. Load-order-specific enchantments
(Summermyst etc.) are a future dynamic pass (see DYNAMIC_OR_DROP) -- not required
for a portable v1.

Curation layer (small, documented):
  - include: single-effect generic 'Ench*' families + an allowlist
  - merge:   Frost = Frost+Slow, Chaos = DLC2 tri-element (one gem each)
  - exclude: artifact/creature/junk single-effects (non-Ench) + uniques
Assigns each gem a draft POWER TIER -> XP multiplier for the per-gem curve
(the point of this pass). Tiers are a proposal for Marth to adjust.
"""
import json, sys, os
sys.path.insert(0, 'tools')
from parse_ench import build_catalog

bal = json.load(open('data/gem_balance.json'))

ALLOW_NONENCH = {'AbFortifyUnarmedDamage'}           # Pugilist/unarmed is a real gem
EXCLUDE = {                                          # junk that leaked into single-effect scan
    'DraugrMagicAxeStreak','DraugrMagicBowStreak','DraugrMagicSwordStreak',
    'EnchDragonPriestUltraMaskEffect','EnchDragonPriestWoodMaskEffect',
    'DLC2BloodskalEnchGlowEffect','DLC2StaggerBallistaFFContact50',
    'StaggerAttackFFContact','DLC2EnchDragonAbsorbFake',
}

# Curated multi-effect single gems: gem_id -> (component mgef set signature, class, domain)
CURATED_MULTI = {
    'frost': {'name':'Frost','mgefs':['EnchFrostDamageFFContact','FrostSlowFFContact'],
              'class':'LINEAR','domain':'weapon','base_mgef':'EnchFrostDamageFFContact'},
    'chaos': {'name':'Chaos','mgefs':['DLC2EnchFireDamageFFContact50',
              'DLC2EnchFrostDamageFFContact50','DLC2EnchShockDamageFFContact50'],
              'class':'LINEAR','domain':'weapon','base_mgef':'DLC2EnchFireDamageFFContact50'},
}

# ---- power tier assignment (DECIDED, Marth) ----
# S build-defining (slowest), A strong (normal), B minor (fast), U utility (no XP).
XP_MULT = {'S':1.5,'A':1.0,'B':0.6,'U':0.0}
# S = genuinely build-defining (slowest XP). Single-element damage (Fire/Frost/
# Shock) is the everyday weapon gem -> A, not S, so early game isn't a slog.
S_GEMS = {'chaos','absorbHealth'.lower(),
          'fortifyHealth'.lower(),'fortifyMagicka'.lower(),'fortifyStamina'.lower(),
          'fortifyMagickaRate'.lower(),                          # caster-defining regen
          'fortifySmithing'.lower(),'fortifyAlchemy'.lower()}    # crafting loops
B_GEMS = {'fortifyLockpicking'.lower(),'fortifyPickpocket'.lower(),
          'fortifyPersuasion'.lower(),'fortifySpeechcraft'.lower(),
          'fortifyArticulation'.lower()}                          # trivial skills
U_GEMS = {'muffle','waterbreathing','waterwalking','soulTrap'.lower(),
          'fortifyCarry'.lower()}                                 # utilities: no XP
STAGGER_S = True   # Stagger (ex-paralyze) -> S (strong CC)

def slug(mgef):
    s = mgef
    for junk in ('EnchRobes','Ench','ConstantSelf','FFContact','Constant','Self','AbFortify'):
        s = s.replace(junk,'')
    return s[0].lower()+s[1:] if s else s

NAME_OVERRIDE = {
    'EnchParalysisFFContact': 'Stagger',                 # paralyze replaced by stagger
    'EnchInfluenceConfDownFFContactLow': 'Fear',
    'EnchResistShocktConstantSelf': 'Resist Shock',      # vanilla typo 'Shockt'
    'EnchSoulTrapFFContact': 'Soul Trap',
    'AbFortifyUnarmedDamage': 'Unarmed (Pugilist)',
    'EnchFortifyShoutTimerConstantSelf': 'Fortify Shout',
}
def nice(mgef):
    if mgef in NAME_OVERRIDE: return NAME_OVERRIDE[mgef]
    s = slug(mgef)
    import re
    return re.sub(r'(?<!^)(?=[A-Z])',' ', s).title()

def tier_of(gid, cls, domain):
    if cls == 'BINARY' or gid in U_GEMS: return 'U'
    if cls == 'STAGGER' and STAGGER_S: return 'S'
    if gid in S_GEMS: return 'S'
    if gid in B_GEMS or cls == 'CONTROL': return 'B'
    return 'A'

# build magnitude lookup from base masters (portable) for curated multi gems
mgefs_data, enchs = build_catalog()

# edid -> (plugin, localFormID) so the runtime script can resolve each effect MGEF
MGEF_BY_EDID = {}
for (origin, fid), info in mgefs_data.items():
    ed = info.get('edid')
    if ed and ed not in MGEF_BY_EDID:
        MGEF_BY_EDID[ed] = {'plugin': origin, 'fid': f"0x{fid:06X}"}
def linear_from_base(base_mgef):
    mags = set()
    for e in enchs:
        if e['origin']=='Skyrim.esm' and len(e['effects'])==1: pass
    # find any ENCH whose single effect is base_mgef, collect tier mags
    for e in enchs:
        for ef in e['effects']:
            mo, mfid = ef['mgef']
            edid = mgefs_data.get((mo,mfid),{}).get('edid')
            if edid == base_mgef:
                mags.add(round(ef['mag'],3))
    m = sorted(x for x in mags if x>0)
    b = m[1] if len(m)>=2 else (m[0] if m else 5.0)
    v = round(b*3.5)
    return [round(b+(v-b)*i/4) for i in range(5)]

catalog = {}
# 1) single-effect gems from balance data
for mgef, e in bal.items():
    if mgef in EXCLUDE: continue
    if not (mgef.startswith('Ench') or mgef in ALLOW_NONENCH): continue
    gid = slug(mgef).lower()
    cls = e['class']
    tier = tier_of(gid, cls, e['domain'])
    # Fortify Shout is a % cooldown reduction, not skill points -> manual curve
    if mgef == 'EnchFortifyShoutTimerConstantSelf':
        e = dict(e); e['curve'] = [10, 15, 20, 25, 30]; e['note'] = 'shout cooldown -%'
    single = e.get('single_level', cls == 'BINARY') or tier == 'U'
    catalog[gid] = {
        'name': nice(mgef), 'mgefs':[mgef],
        'mgef_refs':[MGEF_BY_EDID.get(mgef, {'plugin':'?','fid':'?'})],
        'domain':e['domain'], 'class':cls,
        'curve': 'Level 1 only' if single else (e.get('curve') or e.get('curve_dur')
                 or e.get('curve_level') or e.get('curve_stagger')),
        'single_level': single,
        'power_tier': tier, 'xp_mult': XP_MULT[tier],
    }
# 2) curated multi-effect single gems (Frost, Chaos)
for gid, c in CURATED_MULTI.items():
    tier = tier_of(gid, c['class'], c['domain'])
    curve = linear_from_base(c['base_mgef'])
    note = None
    if gid == 'chaos':                                   # DECIDED: nerf each element to ~40%
        curve = [round(x * 0.4) for x in curve]
        note = 'per-element ~40% of single-element; edge is coverage vs mixed resist'
    catalog[gid] = {
        'name': c['name'], 'mgefs': c['mgefs'],
        'mgef_refs': [MGEF_BY_EDID.get(m, {'plugin':'?','fid':'?'}) for m in c['mgefs']],
        'domain': c['domain'], 'class': c['class'],
        'curve': curve, 'single_level': False,
        'power_tier': tier, 'xp_mult': XP_MULT[tier],
    }
    if note: catalog[gid]['note'] = note

# Stable type_index per gem (drives FormID allocation + the marker encoding
# typeIndex*8+level). Assigned in sorted gid order; NEW gems must APPEND (keep
# existing indices frozen post-release, per DEBUGGING 'never change FormIDs').
for i, gid in enumerate(sorted(catalog), start=1):
    catalog[gid]['type_index'] = i

catalog = {gid: catalog[gid] for gid in sorted(catalog)}
json.dump(catalog, open('data/gem_catalog.json','w'), indent=1)

# summary by tier
from collections import defaultdict
byt = defaultdict(list)
for gid,g in catalog.items(): byt[g['power_tier']].append((gid,g))
BASE = [400,1200,3600,10000]  # to II-V; V=Master=births
print(f"CANONICAL GEMS: {len(catalog)}  (weapon {sum(1 for g in catalog.values() if g['domain']=='weapon')}, armor {sum(1 for g in catalog.values() if g['domain']=='armor')})")
for t,label in [('S','build-defining x1.5'),('A','strong x1.0'),('B','situational x0.6'),('U','utility no-XP')]:
    rows = sorted(byt.get(t,[]))
    if not rows: continue
    print(f"\n=== TIER {t} ({label}) : {len(rows)} ===")
    for gid,g in rows:
        cv = g['curve'] if isinstance(g['curve'],str) else str(g['curve'])
        print(f"  {g['name']:20s} [{g['domain'][:3]}] {g['class']:8s} curve={cv}")
print(f"\nBase XP II-V = {BASE} (V=Master, births); per-gem = base x xp_mult. Wrote data/gem_catalog.json")
