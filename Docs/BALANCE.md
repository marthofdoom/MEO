# Gem Balance — sourcing model and the initial pass

Produced by `tools/build_balance.py` from `data/ench_catalog.json` +
`data/gear_families.json`. Draft numbers land in `data/gem_balance_draft.json`.
All values are the DEFAULT data set (Requiem/LoreRim-anchored compiled
defaults of DESIGN §3 "Configuration & tunability"); on an installed list
the installer's calibration pass derives per-list values over these, and
MCM scalars ride on top — nothing here is a hard commitment, it's the
shipped fallback.

Current shipped roster: **54 gems = 51 normal (15 weapon + 36 armor) + 3
support (Focus/Conduit/Echo)**. The "50-gem" counts in the dated decisions
below are historical (the Sun gem and supports landed after that review).

## The confirmed model (LINEAR effects)

From marth's two anchors (Fire 10->35, Fortify Health 20->70), both exactly
**×3.5** — the global Requiem skill+perk+Grand-soul crafting multiplier, which
is uniform across effects. So for magnitude-scaling effects:

- **base = vanilla tier-2 magnitude** (proxy for the Requiem skill-0 craft)
- **Level V (unperked) = base × 3.5**; Levels I–V interpolate linearly
- Investment then multiplies further: **× 1.40** (Attunement perks) **× 1.50**
  (optional CSF mastery) — see DESIGN §3.

This reproduced Fire `[10,16,22,29,35]` and Fortify Health `[20,32,45,58,70]`.

**Global -15% pass (marth, 2026-07-08, shipped v0.13.0):** every LINEAR and
ABSORB curve ×0.85 (rounded to 1 decimal) — effective V = base × 2.975. Fire
is now `[8.5, 13.6, 18.7, 24.6, 29.8]`, Fortify Health
`[17, 27.2, 38.2, 49.3, 59.5]`. Strict scaling at all levels (level I lands
below the Requiem craft value; the gem's edge is growth, not its floor).
RESIST / SKILL / CONTROL / STAGGER / SOULTRAP / BINARY draft curves are
deliberately untouched — those classes still await their own per-class calls
(table below).
Applies as-is to: Fire, Frost, Shock, Damage Magicka/Stamina, Fortify
Health/Magicka/Stamina, the three Regen effects, Carry Weight, and (per-hit)
Absorb Health/Magicka/Stamina.

## Per-class decisions (marth, 2026-07-08 — full 50-gem review)

×3.5 was wrong for these; each was a balance call, now made. Investment
multipliers (×1.40 perks, ×1.50 CSF) apply to ALL of these unless noted —
the spreads below were reviewed with that in mind.

| Class | Decision |
|---|---|
| RESIST (%) elemental/poison/disease | `[8,13,17,21,25]` — invested V = 53%; capping (75) takes a second gem or another source |
| RESIST (%) magic | keep `[6,9,12,16,20]` — scarcest stat in Requiem, invested V = 42% |
| SKILL fortify (21 gems) | keep drafts, V≈32 — already conservative vs vanilla ~40; deliberately NOT given the -15% pass |
| STAGGER (was Paralyze) | `[0.3,0.4,0.5,0.6,0.75]` — V capped below full stagger to avoid Requiem stunlock loops |
| CONTROL (Banish/Fear/Turn Undead) | `[6,10,15,22,30]` (max creature level) — invested V ≈ 63; stays off bosses |
| SOULTRAP | single level, fixed 5 s — only needs to outlast the kill |
| Muffle | **two levels**: 0.5 → 1.0; level II priced at the normal level-III cumulative total (`xp_mult 2.25`, `max_level 2`) — full silence is earned |
| Fortify Carry | now levels: `[17,27.2,38.2,49.3,59.5]`, health-family anchors, A-tier |
| Waterbreathing | binary, single level, unchanged |

Excluded (leaked into the single-effect scan but are soulbound/artifact/junk,
not gems): Draugr weapon streaks, dragon-priest mask effects, Bloodskal,
DLC2 stagger ballista, generic StaggerAttack, DLC2 fake dragon-absorb.

## Status

**All 50 gems decided (2026-07-08).** LINEAR/ABSORB: model ×3.5 then the
global -15% (= ×2.975). The special classes: table above. Default data set
is frozen for the generator; future changes are deliberate balance patches,
not open questions. Per-gem `[base,max]` for non-Requiem FOMOD baselines is
a later pass. Weapon-side changes (Stagger, the three CONTROL gems) ride
the next DLL build; armor-side numbers activate with the armor-gem
milestone.

## Content budget — how the level thresholds were derived (2026-07-07)

Model: an A-tier gem socketed early and worn throughout. Gem XP: 1/kill,
10/boss, soul feeding as §3, **Mentor gem doubles from mid-Dawnguard on**.
Kill counts are planning estimates for a normal (non-completionist) run —
revisit against real playthrough logs.

| Content block | Std kills | Bosses | Feeding | Gem XP | Running total |
|---|---|---|---|---|---|
| Main quest (full) | ~300 | ~20 | ~100 | ~600 | **600** |
| + one guild questline | ~250 | ~8 | ~50 | ~380 | ~980 |
| + Dawnguard (Mentor mid-line) | ~250 | ~15 | ~100 | ~500→~700 dbl | ~1,700 |
| + misc clearing (10 dungeons) ×2 | ~300 | ~10 | ~100 | ~1,000 | ~2,700 |
| + second guild line ×2 | ~250 | ~8 | ~50 | ~760 | ~3,500 |
| + Daedric/civil war/rest ×2 | ~600 | ~25 | ~400 | ~3,400 | ~6,900 |

Targets → thresholds (A-tier, cumulative):
- **II = 400** — lands mid-main-quest.
- **III = 900** — main quest ends ~⅔ of the way (600/900); needs one extra
  questline. "Near but not without extra quests."
- **IV = 2,800** — the first plateau: MQ + Dawnguard + clearing + guild(s).
- **V = 7,000** — the rest of vanilla minus Dragonborn, Mentor running.

S-tier ×1.5 → 600/1,350/4,200/10,500. B-tier ×0.6 → 240/540/1,680/4,200.
Followers earn their own Gem XP (their kills, their worn gems) — this is
extra throughput for players who arm followers, intentionally not modeled
in the ladder (it accelerates, never gates).
