# Gem Balance — sourcing model and the initial pass

Produced by `tools/build_balance.py` from `data/ench_catalog.json` +
`data/gear_families.json`. Draft numbers land in `data/gem_balance_draft.json`.
All values are the DEFAULT data set (the "Requiem/LoreRim" FOMOD baseline of
DESIGN §3 "Configuration & tunability"); MCM scalars ride on top, so nothing
here is a hard commitment — it's the shipped starting point.

## The confirmed model (LINEAR effects)

From Marth's two anchors (Fire 10->35, Fortify Health 20->70), both exactly
**×3.5** — the global Requiem skill+perk+Grand-soul crafting multiplier, which
is uniform across effects. So for magnitude-scaling effects:

- **base = vanilla tier-2 magnitude** (proxy for the Requiem skill-0 craft)
- **Level V (unperked) = base × 3.5**; Levels I–V interpolate linearly
- Investment then multiplies further: **× 1.40** (Attunement perks) **× 1.50**
  (optional CSF mastery) — see DESIGN §3.

This reproduced Fire `[10,16,22,29,35]` and Fortify Health `[20,32,45,58,70]`.

**Global -15% pass (Marth, 2026-07-08, shipped v0.13.0):** every LINEAR and
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

## The effect classes that need a human decision

×3.5 is wrong for these; each is a balance call, not a formula:

| Class | Gems | Why ×3.5 fails | Draft proposal (pending decision) |
|---|---|---|---|
| RESIST (%) | Fire/Frost/Shock/Magic/Poison/Disease resist | 30% × 3.5 = 105% → past the resist cap; dual-socket chest = double | Single-gem V well below cap so stacking is the payoff, not one gem |
| SKILL fortify | the 18 fortify-skill gems | +42 skill ≈ 2× Requiem's own ceiling | Cap at/near vanilla max, or deliberately exceed it |
| PARALYZE | Paralyze | duration, not magnitude; 4s paralyze is very strong | Keep V short |
| CONTROL | Fear, Banish, Turn Undead | magnitude = affected creature *level*, duration-based | Scales *what* it works on, not damage |
| SOULTRAP | Soul Trap | only needs to outlast the kill | Near-fixed short duration |
| BINARY | Muffle, Waterbreathing, Waterwalking | no magnitude to scale | Do they level at all? |

Excluded (leaked into the single-effect scan but are soulbound/artifact/junk,
not gems): Draugr weapon streaks, dragon-priest mask effects, Bloodskal,
DLC2 stagger ballista, generic StaggerAttack, DLC2 fake dragon-absorb.

## Status

Model locked for LINEAR/ABSORB. The six classes above await Marth's calls;
once decided, finalize the numbers here and freeze the default data set for the
generator. Per-gem `[base,max]` for non-Requiem FOMOD baselines is a later pass.

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
