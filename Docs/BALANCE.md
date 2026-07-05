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

This reproduces Fire `[10,16,22,29,35]` and Fortify Health `[20,32,45,58,70]`.
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
