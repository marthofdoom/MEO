#!/usr/bin/env python3
"""Draft the per-gem I..V magnitude curve from vanilla tier data + the confirmed
Requiem model (base = vanilla tier-2, V = base x 3.5 for linear effects).

Classifies each single-effect generic family into a scaling class; only the
LINEAR class uses x3.5. The others (resist %, duration, binary, skill) get
proposed custom curves flagged for a human balance decision. Emits
data/gem_balance_draft.json and prints a grouped summary."""
import json, sys
from collections import defaultdict, OrderedDict

cat = json.load(open('data/ench_catalog.json'))
fams = json.load(open('data/gear_families.json'))['generic']

# gather vanilla Skyrim single-effect tier data per mgef
tiers = defaultdict(lambda: {'mags': set(), 'durs': set(), 'arche': None, 'av': None})
for e in cat:
    if e.get('enchType') != 6 or e['origin'] != 'Skyrim.esm' or len(e['effects']) != 1:
        continue
    ef = e['effects'][0]; mg = ef['mgef_edid']
    if not mg: continue
    t = tiers[mg]
    t['mags'].add(round(ef['mag'], 3)); t['durs'].add(ef['dur'])
    t['arche'] = ef['archetype']; t['av'] = ef['primaryAV']

def classify(mgef, t):
    e = mgef.lower()
    mags = sorted(m for m in t['mags'] if m > 0)
    durs = sorted(d for d in t['durs'] if d > 0)
    if any(k in e for k in ('muffle', 'waterbreathing', 'waterwalking')):
        return 'BINARY'
    if 'resist' in e:
        return 'RESIST'
    if 'paralysis' in e or 'paralyze' in e:
        return 'PARALYZE'
    if any(k in e for k in ('banish', 'turnundead', 'fear', 'influence')):
        return 'CONTROL'      # magnitude = affected creature level; duration-based
    if 'soultrap' in e:
        return 'SOULTRAP'
    if 'fortify' in e and 'rate' not in e and not any(a in e for a in
        ('health', 'magicka', 'stamina', 'carry')):
        return 'SKILL'        # fortify a skill
    if 'absorb' in e:
        return 'ABSORB'
    return 'LINEAR'

def base_tier2(mags):
    """Requiem 'base' proxy = 2nd distinct vanilla tier (fallback: 1st)."""
    if len(mags) >= 2: return mags[1]
    return mags[0] if mags else 0.0

def linear_curve(base, ratio=3.5):
    v = round(base * ratio)
    return [round(base + (v - base) * i / 4) for i in range(5)]  # I..V linear

# canonical single-effect gems only (multi-effect families decompose later)
canon = {}
for key, fam in fams.items():
    if '+' in key:  # multi-effect combo -> handled by decomposition, skip here
        continue
    mgef = key
    t = tiers.get(mgef)
    if not t or not (t['mags'] or t['durs']):
        # binary/utility with no positive mag still valid (muffle etc.)
        t = tiers.get(mgef, {'mags': set(), 'durs': set()})
    cls = classify(mgef, t)
    mags = sorted(m for m in t['mags'] if m > 0)
    durs = sorted(d for d in t['durs'] if d > 0)
    domain = 'weapon' if fam['weap'] >= fam['armo'] else 'armor'
    entry = {'class': cls, 'domain': domain, 'vanilla_mags': mags,
             'vanilla_durs': durs, 'weap': fam['weap'], 'armo': fam['armo']}
    if cls == 'LINEAR' and mags:
        b = base_tier2(mags); entry['base'] = b
        entry['curve'] = linear_curve(b); entry['note'] = 'base=tier2, V=basex3.5'
    elif cls == 'ABSORB' and mags:
        b = base_tier2(mags); entry['base'] = b
        entry['curve'] = linear_curve(b); entry['note'] = 'per-hit; x3.5 (review strength)'
    elif cls == 'SKILL' and mags:
        b = base_tier2(mags); v = round(min(b * 2.0, 25))  # gentler, cap ~vanilla max
        entry['curve'] = [round(b + (v - b) * i / 4) for i in range(5)]
        entry['note'] = 'FLAG: skill fortify, gentler x2 & cap 25 (decide)'
    elif cls == 'RESIST':
        entry['curve'] = [12, 22, 32, 42, 50]  # % single-element; respects cap when stacked
        entry['note'] = 'FLAG: % resist, cap-aware curve (decide target)'
    elif cls == 'PARALYZE':
        entry['curve_dur'] = [1, 1.5, 2, 3, 4]  # seconds
        entry['note'] = 'FLAG: duration s; paralyze is strong, keep low (decide)'
    elif cls == 'CONTROL':
        entry['curve_level'] = [6, 12, 18, 25, 40]  # affected creature level
        entry['note'] = 'FLAG: affected-level; scales what it works on (decide)'
    elif cls == 'SOULTRAP':
        entry['curve_dur'] = [3, 3, 4, 4, 5]
        entry['note'] = 'FLAG: duration s; only needs to outlast a kill'
    elif cls == 'BINARY':
        entry['curve'] = 'single-level (no scaling)'
        entry['note'] = 'FLAG: utility on/off; does it level at all? (decide)'
    else:
        entry['note'] = 'no vanilla magnitude data; manual'
    canon[mgef] = entry

json.dump(canon, open('data/gem_balance_draft.json', 'w'), indent=1)

# grouped summary
bycls = defaultdict(list)
for mg, e in canon.items():
    bycls[e['class']].append((mg, e))
order = ['LINEAR', 'ABSORB', 'SKILL', 'RESIST', 'PARALYZE', 'CONTROL', 'SOULTRAP', 'BINARY']
short = lambda m: m.replace('EnchFortify','Ftfy ').replace('ConstantSelf','').replace('FFContact','').replace('Ench','')
for cls in order:
    rows = sorted(bycls.get(cls, []))
    if not rows: continue
    print(f"\n=== {cls} ({len(rows)}) ===")
    for mg, e in rows:
        curve = e.get('curve') or e.get('curve_dur') or e.get('curve_level') or '-'
        print(f"  {short(mg):26s} [{e['domain'][:3]}] van={e['vanilla_mags'] or e['vanilla_durs']} -> {curve}")
print(f"\nWrote data/gem_balance_draft.json ({len(canon)} single-effect gems)")
