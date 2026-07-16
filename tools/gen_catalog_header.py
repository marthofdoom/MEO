#!/usr/bin/env python3
"""Embed the weapon-gem catalog into the DLL as native/GemCatalog.h.

Reads data/gem_catalog.json (curves/tiers, canonical) and
out/SKSE/Plugins/MEO/meo_runtime.json (the generated ESP's MISC FormID for
each gem x level — authoritative, matches the shipped MEO.esp byte-for-byte).
No runtime JSON parsing in the DLL: no file paths to break, and the catalog
can never drift from the code that consumes it.

Weapon domain only (M3): armor gems are constant/self effects with their own
apply path, shipped in a later milestone. Run whenever gem_catalog.json or
the ESP changes: python3 tools/gen_catalog_header.py
"""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
catalog = json.load(open(os.path.join(ROOT, 'data/gem_catalog.json')))
runtime = json.load(open(os.path.join(ROOT, 'out/SKSE/Plugins/MEO/meo_runtime.json')))

# Effect-item duration by catalog class (magnitude carries the level curve).
DURATION = {'CONTROL': 30, 'SOULTRAP': 5}   # LINEAR/ABSORB/STAGGER = 0 (on-contact)

# Loot rarity curve: rarer = stronger. A gem's power_tier sets how many copies
# it gets in the spawn pool (g_lootableGems), so the DLL's existing uniform pick
# becomes a weighted one. S-tier (Absorb Health, Chaos, Stagger) are rare finds;
# B-tier (situational control effects) are the common drop. Single-level gems
# (Soul Trap) never enter the pool regardless — weight is moot for them.
SPAWN_WEIGHT = {'S': 1, 'A': 3, 'B': 5, 'C': 6}   # default 3 for unmapped tiers

# Gem theme (m19): which enemy archetypes plausibly wear/wield it. The DLL
# crosses these with per-archetype weights to build the themed NPC spawn
# pools (DESIGN §3 "Post-strip gem economy"). Enum order must match
# meo::Theme in the emitted header / plugin.cpp.
GEMCLASS = {'SKILL': 'Skill', 'LINEAR': 'Linear', 'RESIST': 'Resist',
            'BINARY': 'Binary', 'ABSORB': 'Absorb', 'CONTROL': 'Control',
            'STAGGER': 'Stagger', 'SOULTRAP': 'SoulTrap', 'SUPPORT': 'Support', 'OTHER': 'Other'}
THEMES = ['FIRE', 'FROST', 'SHOCK', 'ARCANE', 'DRAIN', 'MARTIAL', 'ROGUISH',
          'HOLY', 'UTILITY']
THEME = {
    'firedamage': 'FIRE',     'resistfire': 'FIRE',
    'weaknessfire': 'FIRE',   'weaknessfrost': 'FROST',
    'weaknessshock': 'SHOCK', 'weaknesspoison': 'ROGUISH',  # poison -> Roguish, as resistpoison
    'frost': 'FROST',         'resistfrost': 'FROST',
    'shockdamage': 'SHOCK',   'resistshockt': 'SHOCK',
    'chaos': 'ARCANE',        'banish': 'ARCANE',
    'influenceconfdownlow': 'ARCANE',  # Fear — illusion
    'soultrap': 'ARCANE',     'resistmagic': 'ARCANE',
    'fortifyalteration': 'ARCANE', 'fortifyconjuration': 'ARCANE',
    'fortifydestruction': 'ARCANE', 'fortifyillusion': 'ARCANE',
    'fortifyrestoration': 'ARCANE', 'fortifymagicka': 'ARCANE',
    'fortifymagickarate': 'ARCANE', 'fortifyalchemy': 'ARCANE',
    'absorbhealth': 'DRAIN',  'absorbmagicka': 'DRAIN',
    'absorbstamina': 'DRAIN', 'staminadamage': 'DRAIN',
    'magickadamage': 'DRAIN',
    'fortifyonehanded': 'MARTIAL', 'fortifytwohanded': 'MARTIAL',
    'fortifyarchery': 'MARTIAL',   'fortifyblock': 'MARTIAL',
    'fortifyheavyarmor': 'MARTIAL','fortifysmithing': 'MARTIAL',
    'fortifyhealth': 'MARTIAL',    'fortifystamina': 'MARTIAL',
    'fortifystaminarate': 'MARTIAL','fortifyhealrate': 'MARTIAL',
    'fortifycarry': 'MARTIAL',     'unarmeddamage': 'MARTIAL',
    'fortifyshouttimer': 'MARTIAL','resistdisease': 'MARTIAL',
    'paralysis': 'ROGUISH',   'muffle': 'ROGUISH',
    'fortifylightarmor': 'ROGUISH','fortifysneak': 'ROGUISH',
    'fortifypickpocket': 'ROGUISH','fortifylockpicking': 'ROGUISH',
    'fortifyarticulation': 'ROGUISH','fortifypersuasion': 'ROGUISH',
    'fortifyspeechcraft': 'ROGUISH','resistpoison': 'ROGUISH',
    'sundamage': 'HOLY',      'turnundead': 'HOLY',
    'waterbreathing': 'UTILITY',
    'focus': 'UTILITY', 'conduit': 'UTILITY', 'echo': 'UTILITY',
}

def display_name(name: str) -> str:
    """'Fire Damage' -> 'Fire' — the gem word that prefixes item names."""
    return name[:-7] if name.endswith(' Damage') else name

rows = []
for gid, d in sorted(catalog.items(), key=lambda kv: kv[1]['type_index']):
    r = runtime[gid]
    is_support = bool(d.get('support'))
    ref = d['mgef_refs'][0] if d['mgef_refs'] else {'plugin': 'Skyrim.esm', 'fid': '0x000000'}
    single = bool(d['single_level'])
    tiers = d.get('tiers', [0.0, 0.0, 0.0])
    curve = ([1.0] * 5 if single
             else [float(x) for x in (d['curve'] if 'curve' in d else tiers)])
    curve = (curve + [curve[-1]] * 5)[:5]   # pad to 5 (capped gems never reach the pad)
    # forms exist only for minted levels; single = one form, short curves cap.
    avail = {int(k): int(v, 16) for k, v in r['forms'].items()}
    top = max(avail)
    forms = [avail[1] if single else avail[min(lv, top)] for lv in range(1, 6)]
    if gid not in THEME:
        sys.exit(f'ERROR: no THEME mapping for gem "{gid}" — add it to THEME in this script')
    riders = d.get('riders', [])
    if len(riders) > 2:
        sys.exit(f'ERROR: gem "{gid}" has {len(riders)} riders; GemDef caps at 2')
    rider_lit = ', '.join(
        '{{ "{p}", 0x{f:06X}, {r}f, {du}.0f }}'.format(
            p=x['plugin'], f=int(x['fid'], 16), r=float(x['ratio']), du=int(x['dur']))
        for x in riders) or '{}'
    rows.append(
        '    {{ "{gid}", "{name}", "{plugin}", 0x{fid:06X}, '
        '{{ {curve} }}, {dur}.0f, {xp}f, {single}, {{ {forms} }}, {weight}, {armor}, Theme::k{theme}, '
        'GemClass::k{gclass}, {issup}, SupportType::k{sup}, {{ {tp} }}, {nr}, {{{{ {riders} }}}} }},'.format(
            gid=gid, name=display_name(d['name']), plugin=ref['plugin'],
            fid=int(ref['fid'], 16),
            curve=', '.join(f'{v}f' for v in curve),
            dur=DURATION.get(d['class'], 0),
            xp=float(d['xp_mult']),
            single='true' if single else 'false',
            forms=', '.join(f'0x{f:06X}' for f in forms),
            weight=SPAWN_WEIGHT.get(d.get('power_tier'), 3),
            armor='true' if d['domain'] == 'armor' else 'false',
            theme=THEME[gid].capitalize(),
            gclass=GEMCLASS[d.get('class', 'OTHER')],
            issup='true' if is_support else 'false',
            sup=(d.get('support_type', 'NONE').capitalize() if is_support else 'None'),
            tp=', '.join(f'{float(x)}f' for x in (tiers + [0.0, 0.0, 0.0])[:3]),
            nr=len(riders), riders=rider_lit))

header = f"""// GENERATED by tools/gen_catalog_header.py — DO NOT EDIT.
// Source of truth: data/gem_catalog.json + out/.../meo_runtime.json (ESP forms).
#pragma once

#include <array>
#include <cstdint>

namespace meo {{

// Thematic bucket for NPC/vendor spawn pools (m19). Order is frozen —
// generator THEMES list and plugin.cpp weight tables index by it.
enum class Theme : std::uint8_t {{
    kFire, kFrost, kShock, kArcane, kDrain, kMartial, kRoguish, kHoly,
    kUtility, kCount
}};

// Catalog class (m34): mechanical family. Facet Insight boosts skill (SKILL)
// and attribute (LINEAR) armor gems; the rest are elemental/utility/control.
enum class GemClass : std::uint8_t {{
    kSkill, kLinear, kResist, kBinary, kAbsorb, kControl, kStagger, kSoulTrap, kSupport, kOther
}};

// m36: support gems (Echo/Conduit/Focus) modify a normal gem sharing their
// dual-socket item; they have no standalone effect (mgef null = inert alone).
// tierParam[0..2] carries the per-tier lever (Focus magnitude bonus, Conduit
// transferred ratio, Echo follower-share fraction).
enum class SupportType : std::uint8_t {{ kNone, kFocus, kConduit, kEcho }};

// Secondary effect riding on the gem's primary (m21, marth: gems follow the
// load order's elemental recipe — frost carries slow, shock carries magicka
// bite, chaos carries all three elements). magnitude = primary × ratio.
struct GemRider {{
    const char*   plugin;
    std::uint32_t mgefID;
    float         ratio;     // rider magnitude / primary magnitude
    float         duration;  // seconds (0 = instant)
}};

struct GemDef {{
    const char*          gid;        // stable identity, stored in the co-save
    const char*          name;       // display word: "<name> <I..V> <base item>"
    const char*          plugin;     // master file owning the effect MGEF
    std::uint32_t        mgefID;     // MGEF FormID local to that master
    std::array<float, 5> magnitude;  // per level I..V
    float                duration;   // effect duration (0 = on-contact instant)
    float                xpMult;     // level-threshold multiplier (0 = never levels)
    bool                 singleLevel;
    std::array<std::uint32_t, 5> gemItem;  // MEO.esp-local MISC FormID per level
    std::uint8_t         spawnWeight;  // copies in the loot pool (rarer = stronger)
    bool                 isArmor;      // true = constant self effect on worn armor
    Theme                theme;        // thematic bucket for NPC/vendor pools (m19)
    GemClass             gclass;       // catalog class — Facet Insight targets skill/attribute (m34)
    bool                 isSupport;    // m36: modifies a linked normal gem; inert alone
    SupportType          supportType;  // m36
    std::array<float, 3> tierParam;    // m36: per-tier lever (I..III)
    std::uint8_t         nRiders;      // 0..2 secondary effects
    std::array<GemRider, 2> riders;
}};

inline constexpr GemDef kGems[] = {{
{chr(10).join(rows)}
}};

// Cumulative XP to reach level II..V, scaled per gem by xpMult (DESIGN §3).
// KEEP IN SYNC with the shipped native/GemCatalog.h — these were retuned in
// v1.0.6b (BALANCE.md: multiples of 20 so threshold x xpMult never shows a
// fraction). This template is the source the header is regenerated from; if
// it lags, regeneration silently reverts the balance change.
inline constexpr std::array<float, 4> kXPThresholds = {{ 500.0f, 1000.0f, 3000.0f, 8000.0f }};

inline constexpr const char* kRoman[5] = {{ "I", "II", "III", "IV", "V" }};

}}  // namespace meo
"""
out = os.path.join(ROOT, 'native/GemCatalog.h')
open(out, 'w').write(header)
narmor = sum(1 for gid, d in catalog.items() if d['domain'] == 'armor')
print(f'wrote {out}: {len(rows)} gems ({len(rows)-narmor} weapon + {narmor} armor)')
