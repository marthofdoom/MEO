# marth's Enchanting Overhaul (MEO) — Design Document

Socketed-gem enchanting for Skyrim SE. Enchantments become **gems**: removable,
socketable, leveling items. This fully replaces vanilla enchanting for weapons
and armor. Staves, wands, and scripted artifacts are detected and left alone.

Target: vanilla SE + SKSE core. Built with the toolchain in
`MANUAL_MOD_CREATION_GUIDE.md` (Python ESP generator, Papyrus via Proton wine)
plus the native layer (`native/plugin.cpp`, CommonLibSSE-NG, CI-built) and the
C# Mutagen installer (`installer/`).

**STATUS (v0.49.4, 1.0 candidate): this spec is reconciled to the SHIPPED
NATIVE build.** The DLL + SKSE co-save are the source of truth for socket
state; the Papyrus-era marker mechanism (§7) and the P0–P4 phase plan (§10)
are retired history, kept below only as marked. Per-load-order numbers come
from the installer's calibration pass, not from baked records.

### Why native (the two reasons that drive it)
1. **Conformance.** A CommonLibSSE-NG SKSE plugin is the strictest, most
   stable modern standard — Address-Library-based, engine-version-resilient,
   the ecosystem norm reviewers and modlist authors trust. Papyrus-only
   socket/hit logic would be the fragile, non-conforming path.
2. **No save bloat via native indexing.** The socket index (which gem, which
   level, which XP, per item instance) lives in the DLL and serializes to the
   **SKSE co-save** (`.skse`), NOT the Papyrus VM in the `.ess`. Per-instance
   script bookkeeping in Papyrus is exactly what bloats and orphans saves;
   owning the index natively sidesteps that class of problem entirely.

Relationship to the §7 marker mechanism (HISTORICAL): the marker-in-enchantment
trick was the **Papyrus-era** way to be stateless. The native build superseded
it exactly as planned — the DLL's co-save index is the source of truth and
reapplies real effects on load; markers were never shipped. P0 (the Papyrus
prototype that validated runtime-enchant persistence) passed 2026-07-05 and
its guarantee carried over to the native path.

### Native/DLL toolchain notes (reused from MRO — all applied, kept as doctrine)
The sibling project MRO has a working CommonLibSSE-NG pipeline built entirely
from Linux; MEO copied it rather than reinventing. Load-bearing facts (full
detail in MRO's `docs/NATIVE_REWRITE_PLAN.md` and `docs/DEBUGGING.md`):
- **No Linux cross-build.** CommonLibSSE-NG needs the MSVC linker → build on a
  GitHub Actions `windows-latest` runner; the artifact is the DLL. vcpkg
  binary cache keeps runs ~2–3 min. Pin the vcpkg registry baseline or a stale
  `fmt` breaks against modern MSVC.
- **Every DLL/ESP build that reaches the game goes through the release script**
  (tag + immutable `releases/vX.Y.Z/`, version stamped into the MCM About
  page). Ad-hoc "just copy the file in" installs cost a session to untangle in
  MRO — the running game and the archive disagreed about what version was live.
- **Hooks: Address Library IDs only, one hook per release, INI-gated with a
  default matching what has been play-tested, self-verify the code site at
  install (bail on opcode mismatch).** Instruction-cave/fixed-offset patches
  are banned (they broke on the 1.6.1130+ recompile). The disk EXE is
  Steam-DRM encrypted — verify hook sites against `/proc/<pid>/mem` of the
  running game, and validate the Address Library `.bin` against crash-log
  ground truth before trusting any address.
- **GlobalVariable values are save-persisted:** a DLL that writes a global at
  data-load will be overwritten when a save loads — re-assert on
  `kPostLoadGame`/`kNewGame`. This is the natural Papyrus↔DLL bridge for MEO
  (e.g. the DLL owns socket/hit logic, Papyrus owns the Gem Pouch UI).

Core principles:
- **No friction, no loss.** Socketing and unsocketing happen anywhere, need no
  station, never destroy anything, and never lose progress.
- **The item is the database.** Socket state lives in the item's own
  enchantment (see §7); the mod keeps no per-item bookkeeping.
- **Only the player levels gems.** NPC gear participates only as a gem source.

---

## 1. Player loop

1. Loot an enchanted sword — it arrives **converted**: the plain base item
   with the matching gem already socketed and active (see §2 "loot
   conversion", SHIPPED m23). Pop the gem out from the Gem Pouch menu,
   anywhere, any time. Converted-loot gems are **level I** (level II at the
   same low roll as loose drops) regardless of the item's vanilla tier.
2. **Socket** gems into any eligible gear. Free, lossless, reversible.
3. Gems socketed in your worn gear earn **XP from your kills** and level
   I → V; magnitude scales with level. Filled soul gems can be fed to a
   socketed gem at any enchanting station for instant XP (the Soul Feeder
   perk doubles the yield).
4. A gem that fills its bar past level V is **Mastered** — it keeps working
   at level V and **births** a fresh level-1 copy of itself. Birthing is the
   only way to replicate a gem.
5. Dual-slot items link their two sockets: **support gems** (§5) only
   function when linked to a normal gem.

## 2. Gem catalog (data-driven)

Generated from ENCH/MGEF/ARMO/WEAP records of the five masters — see
`data/ench_catalog.json` (743 ENCH records) and `data/gear_families.json`
(151 enchantment families that actually appear on obtainable gear).

Collapse rules ("smart dedup"):
- Group ENCH records by their **set of MGEFs** → one family per set. The
  vanilla magnitude tiers (`...01`–`...06`) are NOT the gem's I–V curve — they
  are only used to identify the family. The I–V magnitudes come from the
  scaling model in §3 (I = Requiem base, V = Requiem max), which we anchor per
  effect. Per-effect base/max shipped in the catalog data, and on installed
  lists the installer's calibration derives them from the list's own tier
  magnitudes — see §3.
- Multi-effect generic combos (mage robes = Fortify School + Magicka Regen,
  Chillrend = Frost + Paralyze, ...) are NOT separate gems — they decompose
  into their component gems, one per socket. Robes' double effect is
  reproduced by the dual-slot chest piece.
- `enchType == 12` (staff enchantment, 71 records) → **excluded**; staves and
  wands keep vanilla behavior end to end.
- Junk/system effects (BYOH house parts, traps, shout enchants, bound-weapon
  FX, draugr streaks) never reach the catalog because only enchantments found
  on obtainable ARMO/WEAP with a generic `Ench*` family qualify.

**Shipped roster: 54 gems total = 51 normal (15 weapon + 36 armor, levels
I–V) + 3 support (Focus/Conduit/Echo, tiers I–III, §5).**

### Weapon gems (15) — fire-and-forget, on-hit
Fire, Frost (with slow), Shock, Chaos (DLC2 tri-element), Absorb Health,
Absorb Magicka, Absorb Stamina, Damage Magicka, Damage Stamina, Fear,
Banish, Paralyze, Soul Trap, Turn Undead, Sun (Dawnguard, undead-only).

### Armor gems (36) — constant-effect
- Attributes: Fortify Health / Magicka / Stamina, Regen ×3, Carry Weight.
- Resists: Fire, Frost, Shock, Magic, Poison, Disease.
- Skills: the 18 fortify-skill effects (One-Handed, Two-Handed, Archery,
  Block, Smithing, Heavy/Light Armor, Pickpocket, Lockpicking, Sneak,
  Alchemy, Speech, Alteration, Conjuration, Destruction, Illusion,
  Restoration, plus Fortify Shout (Talos) and Unarmed (Pugilist)).
- Utility: Muffle, Waterbreathing.

Placement rules: weapon gems only socket into weapons, armor gems only into
apparel — EXCEPT via the Conduit support gem (below). Vanilla slot
restrictions per effect (e.g. Fortify Archery on helmets) are dropped; any
armor gem fits any apparel slot.

### Converting found loot (SHIPPED — m23 conversion model)
The original lazy design ("decode on first socket touch") was superseded by
marth's m23 ruling further down this section: a covered enchanted generic
**converts at spawn/acquire** into its unenchanted base with the matching
family's gem socketed and ACTIVE. The installer emits the per-list
`conversions` table; the DLL swaps instances via engine flows on actor
spawn, container arrival, vendor stock, pickups, and a player sweep.
Coverage on the dev list is ~87% of generic enchanted gear; uniques,
artifacts, and named sets are intentionally kept enchanted with sealed
sockets. Creation Club content is covered (the installer reads
`Skyrim.ccc`). See "CORRECTED to CONVERSION" below for the mechanism.

### World-visible socketed gear — self-describing item extra data (native)
A weapon lying on the ground showing "Fire I Glass Dagger" — and staying that
way through pickup with **no transform-on-pickup** — is achieved the same way
vanilla enchanted loot is: the socket state is stored as **engine extra data on
the item instance itself**, which the engine renders everywhere the item appears
(worn, dropped, chest, boss corpse) with no hooks and no repaint. The three
pieces we attach to the instance:
- **`ExtraTextDisplayData`** — the display name ("Fire I Glass Dagger").
- **`ExtraEnchantment`** — the actual runtime effect.
- **`ExtraUniqueID`** — stable per-instance identity; also stops it stacking
  with plain copies.

All are engine-serialized and travel with the instance, so the item is
self-describing and **never transforms** — the world name and the picked-up name
are the same because it is the same instance carrying the same data (exactly why
a vanilla "Iron Sword of Sparks" shows its name on the floor).

This requires **native**: Papyrus can only put a runtime enchantment on the
*worn* slot (`WornObject.CreateEnchantment`); C++ can set these extra-data
records on ANY item instance (native-plan reason #2). It is NOT "paint on
3D-load" — that clunkier model is only needed if socket state lives off the
item, which it does not.

The **co-save** map holds only the leveling layer (per-instance XP/level, keyed
by `uniqueID`); display does not depend on it. On level-up the DLL rewrites the
magnitude + name on the instance.

**Loot:** items the player sockets and drops display correctly in the world
immediately (above). Items a boss/chest pre-drops **already socketed** must be
stamped with this extra data at loot-generation time — a later milestone
(leveled-list injection or a container-init hook), separate from the display
mechanism itself. Untouched vanilla loot stays byte-for-byte vanilla until first
socket.

### Compatibility — enchant-visibility mods (loot UX; post-1.0 pass)

Status update for the conversion era: generic enchanted loot now converts to
socketed gear on acquire, so "how enchanted gear reads before pickup"
applies mainly to the uncovered tail and to artifacts (which keep their real
ENCH — those mods work unchanged on them).
- **See Enchantments SE** (weapon glow in the world) — artifacts and
  unconverted loot are vanilla ENCH, glow intact. Socketed gear's combined
  created-enchantment carries the winning MGEF's own shaders (m12 made it
  permanently active, so FX apply immediately).
- **Unknown Enchants**-style "(U)" indicators — read the vanilla
  known-enchantment list, which MEO never populates; with conversion in
  place they mostly see artifacts. A semantic-match pass ("(U)" = a gem
  family you don't own yet) remains a post-1.0 compat candidate.

### Unique / artifact enchantments (68 families)
Policy: artifacts with scripted, quest, or one-off enchantments (Ebony Blade,
Spellbreaker, Ring of Namira, nightingale sets, masks, ...) are **soulbound**:
their gems cannot be removed and their sockets are sealed. They keep working
exactly as vanilla. Implemented as an FLST exclusion list of ENCH FormIDs +
a heuristic (any effect whose MGEF archetype is Script and is not in the
generic catalog). A curated subset with clean effects (e.g. Silent Moons)
can graduate into extractable **unique gems** (level normally, cannot birth)
in a later version.

### Installer prime directive (marth 2026-07-09, RULE)
The installer **tracks what is actually happening in the load order — never
assumes values, never hardcodes matches**. Nearly every list carries author
tweaks (LoreRim's fire MGEF was last touched by Big Tweaks, not Requiem).
Concretely, per install the tool derives from winning records:
- **Effect identity**: each gem's MGEF ref remapped to the list's canonical
  implementation (signature match), so gems inherit the list's tuned
  archetype/AV/description behavior automatically.
- **Recipe structure**: riders (extra effects + magnitude ratios + durations)
  read from the list's own generic tier lines — the same family-covered
  ≥4-item detection the strip uses. On a Thaumaturgy list the elemental
  recipe differs from Requiem's, and the gems follow it.
- **Magnitude anchors**: curve endpoints calibrated from the list's tier I /
  tier max enchant magnitudes (generalizes §3's "I = Requiem base, V =
  Requiem max").
Delivery: the installer writes a calibration JSON next to meo_runtime.json;
the DLL applies it over the compiled GemCatalog.h defaults at kDataLoaded.
The compiled defaults (hand-matched to Requiem, e.g. m21 riders) remain the
fallback for installs that never ran the tool.

**Rider derivation SHIPPED (m22, v0.30.0).** `write-calibration` (also run
automatically by the post-install exe) matches every strip-classified ENCH
to the family whose full catalog-ref multiset it contains (identity before
signature; chaos's 3-ref set outranks fire's 1 on a tri-element enchant),
normalizes companion magnitudes against the family primary, and the
dominant recipe by item count wins. Semantics: a family **named** in
`meo_calibration.json` gets exactly that rider set — an empty list clears
the compiled default (LoreRim's shock line has no magicka rider; the bite
is inherent in its winning MGEF, so the compiled default would
double-dip); absent families keep compiled defaults. Riders with
MGEF-level conditions self-gate (ENGINE_NOTES §12). Effect-level-conditioned
companions are skipped with a note (they'd fire unconditionally as riders).
Duration-anchored primaries (paralysis, soultrap: magnitude 0) can't
ratio-normalize — deferred, noted in the JSON. LoreRim validation: derived
frost rider Slow ×2.0/3s reproduces the m21 hand-read exactly.

**New-family rarity (marth 2026-07-09, RULE):** families added from
list-discovered recipes (Force, Spellbreaking, burst variants,
armor-penetration packages) enter at the **second-rarest tier (A)**.

### Install-time load-order scanner (SHIPPED as the installer's scan/calibration passes; catalog EXTENSION remains post-1.0)
The catalog ships vanilla effects; a mod load order carries dozens more
(Lorerim scan: ~267 single-effect enchant MGEFs not in our catalog). An
install-time tool remaps and extends against the *actual* load order. Two jobs:
1. **Remap** each existing gem to the load order's *winning* MGEF implementation
   (signature identity), so e.g. Requiem's own Fortify Unarmed MGEF (23 items) is
   the one our Pugilist gem points at.
2. **Extend** the catalog with qualifying mod enchant families → freeze-aware
   extension ESP + runtime JSON; the DLL loads the extension catalog at
   kDataLoaded (revisits "no runtime JSON in the DLL").

Decisions (marth): **inclusion = threshold ≥4 wearable items + a maintained
blacklist** (drops the 1–2-item artifact tail automatically; blacklists MGEFs
that duplicate effects we already cover, e.g. Requiem's tier-2 fortifies).

Strip classification (marth 2026-07-09):
- **Artifact-class → KEEP enchanted, socket-sealed**: true artifacts/uniques;
  multi-effect enchantment packages (mage robes = school + regen, etc.);
  named themed sets even when single-effect (Silent Moons/Lunar — keep
  blacklist, substring match on MGEF/ENCH edid); new-family effects no gem
  covers (Spellbreaker ward, Thaumaturgy families on non-Requiem lists).
- **Strip → replace with plain item in LVLIs**: single-effect, family-covered
  (signature match against the gem catalog), generic (≥4 items), not
  blacklisted.
- **Never socketable**: anything with an existing ability — base `EITM`
  (formEnchanting) or a foreign ExtraEnchantment. Already enforced in the DLL
  (IsSocketable*Base + apply-path + menu listing + NPC stamper); artifacts
  stay enchanted AND cannot take gems.
- RULED (marth 2026-07-09): Requiem's tiered 2-effect generics ("of
  Freezing" = frost+slow tiers; ~560 enchants / ~4,351 items in LoreRim)
  are **stripped**. Artifact-class means distinctive named *packages* only
  (mage robes, Silent Moons, uniques) — generic "of X" tier lines go even
  with 2 effects, so gems fully replace generic enchanted loot. Practical
  test for the tool: an enchantment is a generic tier line if its effects
  are all family-covered AND it appears on ≥4 items across material tiers.
- Doctrine refinement (marth, same day): those second effects are
  **incidental riders**, not packages — they're what the element *means* in
  the load order (Requiem frost = frost + slow 2M/3s; shock = shock +
  magicka bite 2M; fire = fire alone). **Gems follow the recipe**: catalog
  rows carry `riders` (mgef ref + ratio + duration), GemDef/DLL build the
  full effect list (m21). Chaos gets its frost+shock components (always
  fire — no 50% proc conditions on built instances; flagged for balance
  review). Explicit doubles (deliberate distinct-effect packages like
  robes) remain artifact-class.
- **CORRECTED to CONVERSION (marth 2026-07-09, m23 SHIPPED v0.31.0)**: the
  "strip" never removes loot — a covered enchanted generic **converts, at
  spawn/acquire, into its unenchanted base with the matching family's gem
  socketed and ACTIVE** (level I/II rolled with the same fGemLevel2Chance
  as loose drops). No LVLI patch: the installer emits a `conversions`
  table in meo_calibration.json (enchanted item → base + family; LoreRim:
  10,146 rows) and the DLL swaps instances via engine flows (RemoveItem +
  AddObjectToContainer with a pre-stamped ExtraDataList). Worn NPC
  conversions re-equip and ApplyWornAbility — an enemy holding a Fire I
  weapon deals Fire I damage. Hooks: actor spawn sinks (before the m19
  bless roll), ContainerSink on any arrival (chests, corpse loot,
  purchases, pickups — covers world refs at pickup), vendor dialogue
  sweep, player PostLoadGame sweep. Idempotent: converted bases aren't in
  the table. Duration-anchored recipes (paralysis, soultrap companions —
  not yet expressible as riders) keep spawning enchanted until they are.
**Packaging = standalone Mutagen CLI** (self-contained .exe, any mod manager, we
own the UX; not a Synthesis patcher). `tools/scan_loadorder.py` is the python
executable spec / recon prototype for this tool (no dotnet on the dev machine —
prototype in python, build the C#/Mutagen tool via CI).

## 3. Levels, XP, birthing

- Gems are MISC items, one form per type × level: `MEO_Gem<Type><1..5>`.
  Support gems level too, but in **3 tiers** (`MEO_Support<Type><1..3>`); see §5.
- **Terminology (marth, 2026-07-07): the currency is "Gem XP"** — never "AP"
  (banned term; all naming must be original to MEO). Code/serialization keep `xp`.
- **Every unique gem owns its own Gem XP pool — no pooling, no fungibility
  of any kind (marth, 2026-07-07).** A gem is a unique individual for its
  whole life: socketed or loose, it keeps its own {level, Gem XP}. A Fire I
  can be slotted alongside a Fire III; two Fire I gems with different
  partial progress stay distinct items. **Unsocketing returns THE gem**
  (key rule — investment is never destroyed): the same instance comes back
  with its exact level and partial XP. Mechanically: a loose leveled gem
  carries its own instance identity (ExtraUniqueID + co-save record, the
  same {gid, level, xp} record shape) — the record follows the gem between
  "socketed in weapon X" and "loose item" states, so distinct gems never
  merge into a stack. The pouch UI must therefore list each unique gem
  individually, with its progress (e.g. "Fire I — 300/500").
  *M5 (v0.11.0)*: the Gem Pouch is a native two-pane **ContainerMenu**
  (hidden CONT form + temp ref, activated by the pouch power; all mutation
  in one reconcile pass on menu close). The socketed gem sits inside as a
  live instance — take it out to unsocket; put a gem in to socket; both at
  once is an **atomic swap** (the old gem leaves the weapon only when the
  new one arrives — fixing the M4b message-box Swap, which unsocketed
  before a choice was made). The M4a/M4b message-box menus are retired.
- **Followers earn Gem XP too (marth, 2026-07-07):** kills made by a player
  teammate award Gem XP to the gems socketed in *that follower's* worn gear —
  each actor levels their own gems, so equipping followers with gems is never
  a waste. (Fallback if per-follower proves hard: share the player's awards.)
- Gem XP sources:
  - **Kills** — every kill by the player (or a follower, for their own gems)
    awards Gem XP to *every* socketed gem in parallel: **1 per standard
    enemy, 10 per boss**. Native `TESDeathEvent` sink (implemented v0.7.1).
  - **Soul feeding** (SHIPPED m10) — at an **enchanting station** (SETTLED,
    marth 2026-07-07: feeding and destruction are station interactions, NOT
    pouch ones — the pouch/socket menu never grows a feeding zone), consume
    a filled soul gem to grant Gem XP to one gem:
    **petty 1, lesser 2.5, common 5, greater 12, grand/black 40**
    (`kSoulFeedXP` in plugin.cpp). These values are THE balance choice
    (marth, m26b v0.35.1): the original 5/12/25/60/200 table power-leveled
    gems — a grand soul was half a level-I threshold — so the soul→XP rate
    was cut ~80% while gem POWER stayed untouched. With the **Soul Feeder**
    perk the yield is **2×** (both when fed and when reclaimed), so a Grand
    = 80 Gem XP. Feeding also grants Enchanting SKILL xp (MCM-tunable).
  - **Gem destruction** (SHIPPED m10) — at an **enchanting station** (same
    2026-07-07 decision), destroying a gem reclaims **1/10 of its Gem XP**
    into the largest filled soul gem it can afford, using the same
    `kSoulFeedXP` table so the soul↔XP exchange stays symmetric (the only
    sink that recovers investment).
  - **The Mentor gem** (SHIPPED) — a unique gem that **doubles Gem XP**,
    granted once on first arrival in the Soul Cairn (`DLC01SoulCairn`
    worldspace) — thematically between Chasing Echoes and Beyond Death,
    exactly the calibration window. The calibration below assumes it's
    active for the back half of a playthrough. Shipped behavior: doubles
    ALL Gem XP earned by whoever carries it (whole-carrier aura; making it
    a true linked support gem is a possible later refinement). Grant is
    latched in the co-save.
  - **Gems are NOT sellable (marth, RULE):** gold value 0, enforced both in
    the ESP source and at runtime by the DLL; loose gems live in the hidden
    pouch so vendors never even list them. Gems are progression items, not
    currency.

- **Level thresholds (default A-tier, MCM-tunable): cumulative
  500 / 1,000 / 3,000 / 8,000 Gem XP to reach II / III / IV / V.**
  Reaching 8,000 = **Level V = Master**: the gem **births** one fresh
  Level-1 copy (notification) and stops accruing. Birthing is the only way
  to replicate a gem.
- **Calibration ladder (marth, 2026-07-07)** — thresholds are derived from
  content budgets, not vibes; see BALANCE.md "Content budget" for the model:
  - Main quest alone → **Level II comfortably, ~⅔ toward III, cannot reach
    III** without extra content.
  - III → main quest + one guild questline (or equivalent clearing).
  - **IV = first plateau** → main quest + Dawnguard + misc clearing + one or
    two guild questlines (Mentor gem doing its work from mid-Dawnguard).
  - **V = the long haul** → achievable within remaining vanilla quests and
    dungeons, **excluding Dragonborn** — Daedric quests, civil war, the rest
    of the guilds, sustained clearing and soul feeding.
- **Per-gem XP scales by power tier** ("dependent on rarity and power"): the
  threshold is `base × xp_mult`, with **S ×1.5** (build-defining, e.g. Chaos,
  Stagger, attributes, Magicka Rate, crafting skills), **A ×1.0** (most gems),
  **B ×0.6** (situational: control gems, trivial skills), **U** utilities don't
  level. So an S-tier gem masters at 10,500, a B-tier at 4,200. Each gem's
  tier lives in `data/gem_catalog.json` (see §3 scaling model / BALANCE.md).

### Magnitude scaling model (the balance spine)

A gem's I→V curve is anchored to the **Requiem** enchanting range for that
effect, NOT to vanilla loot tiers: **Level I = Requiem base** (what a fresh,
skill-0, no-perk, Grand-soul enchant gives) and **Level V (unperked) = Requiem
max** (skill 100 + max perks + Grand soul — the Requiem ceiling). Gem
investment then pushes MEO *past* that ceiling, which is the whole power
fantasy: a maxed, perked, mastered gem is ~2.1× anything Requiem can craft.

Worked anchors (two example gems):

| Stage | Fire (weapon) | Fortify Health (armor) | Source of the number |
|---|---|---|---|
| Requiem base | 10 | 20 | skill 0, Grand soul, no perks |
| Requiem max | 35 | 70 | skill 100, max perks, Grand soul (the ceiling) |
| **MEO Gem I** | **12** | **20** | universal early-game drop baseline (≈ Requiem base) |
| **MEO Gem V** (unperked) | **35** | **70** | pure gem mastery = Requiem ceiling |
| MEO Gem V + Attunement perks | 44 | 88 | × **1.25** (rewritten tree, +5%/rank ×5) |
| MEO Gem V + perks + full CSF mastery | 66 | 131 | × **1.50** more (MRO/CSF over-cap) |

- Multipliers **stack multiplicatively** on the unperked Level V value:
  `V × 1.25 (perks) × 1.50 (mastery) = V × 1.875`. (35×1.875≈66, 70×1.875≈131.)
- The **+25% Attunement** tier is MEO's own perk tree (§6) — available on
  vanilla-core.
- The **+50% mastery** tier is an OPTIONAL top layer gated on a CSF enchanting
  mastery (shared with / modeled on the sibling MRO). Without CSF present, the
  ceiling is the perked Level V (49 / 98). Keep MEO standalone-playable; the
  mastery tier is synergy, not a hard dependency.
- **Levels II–IV** interpolate between I and V. Default: linear, per-gem
  curves shipped in `data/gem_catalog.json` (see BALANCE.md for the final
  per-class decisions, including the global -15% pass). On an installed
  list, per-gem magnitudes are additionally **derived from the list's own
  records** by the installer's calibration pass (m35b) — the compiled
  defaults are only the fallback for installs that never ran the tool.

### NPC / enemy participation and level caps

Only the **player** levels gems (core principle). Enemy enchanted gear is
capped low so player investment always outscales it:

- **Normal enemies ≤ Level II** (and Level II only rarely); **bosses ≤ Level
  III**. Against a maxed player Fire V (49–73) an enemy's Fire II (~18) is
  meant to feel outclassed — that is the intended progression payoff.
- Because Level V is pinned to the Requiem ceiling, most vanilla/Requiem enemy
  enchantments **already** land at or below the Level II–III magnitude band, so
  the cap is largely self-enforcing via existing loot distribution — we do not
  need to touch NPC gear to hit it in the common case.
- The aspiration LANDED via conversion (m23): enemies whose enchanted gear
  is family-covered spawn holding the *converted* socketed piece (worn
  conversions re-equip and apply the worn ability — an enemy with a Fire I
  weapon deals Fire I damage), plus themed worn-socket rolls (m19). The
  whole world runs one ruleset for covered generics; only the uncovered
  tail and artifacts keep vanilla ENCH — and the uncovered tail only while
  `bAllowUncoveredGenerics` stays ON (default). OFF strips tagged uncovered
  generics to their plain bases at the same sweep points conversion uses
  (base-swap only; instance enchants and artifacts/uniques/quest
  untouchable; already-stripped instances stay plain if the toggle returns
  to ON).

### Configuration & tunability (portability contract)

Every balance number in this section is load-order-dependent (the Requiem
anchors differ on vanilla / Adamant / Enderal / etc.). The portability rule
(archived DYNAMIC_OR_DROP doc): a baked number is mistuned everywhere but
this machine, so tuning is exposed, not frozen. The architecture enforces
it: gem magnitudes are computed by the DLL at stamp/rebuild time (the same
path perk multipliers use), so NOTHING magnitude-related is baked into
MGEF/ENCH records. How it shipped:

- **Installer calibration (install-time baseline)** — replaced the old
  FOMOD-baseline idea entirely: per-gem magnitudes, riders, and rank
  ladders are DERIVED from the user's own load order's winning records
  (`meo_calibration.json`, m22/m35b); the compiled Requiem-anchored
  defaults are only the never-ran-the-tool fallback.
- **MCM (runtime fine tuning)** — global scalars on top: gem power scale,
  XP rates, boss multiplier, drop/spawn/vendor rates, notification and
  behavior toggles. The MCM scales the calibrated data, never edits it
  per-gem. Settings apply on menu close (live INI re-read).

### Gem spawns into the world — rates & rarity curve (v0.15.0)

MEO seeds gems without touching leveled lists (§7): a player/follower kill rolls
a lootable corpse gem, and world weapon references roll a pre-socketed gem on
cell attach. Rates are `MEO.ini` keys (code defaults in parens), portable per
DYNAMIC_OR_DROP:

- `fGemDropChance` (0.03) — chance a kill drops a corpse gem.
- `fWorldSocketChance` (0.05) — chance a world weapon ref is born socketed
  (deterministic per-refID hash: a given reference decides once, forever).
- `fGemLevel2Chance` (0.02) — a spawned gem is born **level II** instead of I;
  applies to both corpse drops and world sockets. (Vanilla-loot *conversion* per
  §1 is still always level I — this is only MEO's own spawns.)

**Which gem** is weighted by `power_tier`: each gem gets `spawnWeight` copies in
the spawn pool (`gen_catalog_header.py` `SPAWN_WEIGHT`: S=1, A=3, B=5), so rarer
= stronger. On the 14 lootable weapon gems that lands S-tier (Absorb Health,
Chaos, Stagger) at ~2.4% each, A-tier elementals ~7% each, B-tier control
effects ~12% each. Single-level gems (Soul Trap) never spawn. The weighted pool
keeps the DLL's pick uniform — only pool construction changes.

### Post-conversion gem economy (SHIPPED — m19 enemies/vendors, m23 conversion)

With conversion live, gems are the enchantment source for covered generics.
The existing rates above stay UNCHANGED; two additional sources round out
the economy — roughly **doubling gem accrual while keeping it varied**:

1. **Enemies spawn wearing socketed equipment.** Each enemy class has a
   chance of at least one worn socketed piece, with BOTH the rate and the
   gem pool adjusted **thematically per enemy type** (mage → magicka/
   destruction-flavored gems, warrior → weapon/stamina, undead → frost/
   drain, etc.). The tier rarity curve still applies ON TOP, and harder:
   rare (S-tier) gems appear on enemies at rates even LOWER than world drop
   rates — farming a mob type technically works but is deliberately
   unprofitable for rare gems. Kill → loot the socketed piece → unsocket
   the gem free (unsocketing is lossless, §4).
2. **Shops sell gems** at a similar per-item roll (~`0.04`), and the
   enchanted equipment vendors would have stocked is now **socketed**
   equipment instead — same shelves, gem-flavored.
   *Implementation status (m19b)*: loose gem sales BUILT (per-item roll at
   barter open, deterministic per vendor per game day, cap 3, tier-weighted).
   *Enemy worn gems STAY IN THE GEAR* (marth 2026-07-09) — the corpse's
   socketed item is itself the loot; loose corpse gems are a separate pool.
   That requires surviving container transfers, so m19 adds the
   **container re-key** (`TESContainerChangedEvent` sink): when a base with
   socket records moves containers, the arriving orphan xList (enchanted,
   record-less) and the stranded record (uid in neither container) are
   matched and the record moves to the new uid — looted/bought/stored
   socketed gear keeps living records (ENGINE_NOTES §1 trap neutralized;
   ambiguous multi-instance transfers log + skip). VALIDATED in the field
   2026-07-09 (`[rekey]` log-proven). Vendor shelves get gem-flavored stock
   via conversion of their enchanted inventory (m23 covers vendor
   acquisition paths).

Implementation notes: both are DLL-side and runtime-dynamic (per
DYNAMIC_OR_DROP — no leveled-list edits): enemy worn-socket rolls stamp the
actor's worn instance on load/attach (same deterministic-per-ref discipline
as world weapons); vendor rolls stamp/insert at barter-menu open. Rates as
INI/MCM keys like the rest. (Shipped m19/m19b, before conversion landed, so
the economy never dipped.)

## 4. Sockets and the Gem Pouch

Socket layout (marth, DECIDED — supersedes the earlier "cuirass dual by
default" sketch):

| Gear | Unperked | Perked |
|---|---|---|
| Head (helmets AND circlets — circlets use their own biped slot, added m36f) | 1 | 1 |
| **Gloves** | **2 (linked)** | **2 (linked)** |
| Chest | 1 | **2 (linked)** |
| Ring | 1 | 1 |
| Amulet | 1 | 1 |
| Main-hand weapon | 1 | **2 (linked)** |
| Off-hand (2nd weapon or shield) | 1 | 1 |
| Boots | 0 | 0 |

- **Gloves are the natural dual slot — dual-linked from the start.** This is how
  the mechanic is introduced: the player's first real socketing is a linked
  pair on gloves, teaching the dual/link concept early (two normal gems until a
  support gem shows up).
- **Totals: 7 unperked** (8 dual-wielding or with a shield) → **9 perked** (10
  dual-wielding). Jewelry is socketed by default (thematically it must be);
  boots have no sockets.
- Two socket perks each add a 2nd, **linked** socket: the penultimate unlocks
  the chest, the final unlocks the main-hand weapon (see §6).
- **Support gems can function early but are rarity-gated.** They work in any
  linked slot (gloves from the start), but per §5 they are not found before
  ~level 15, so early game the dual gloves just hold two normal gems. Support
  gems are still an endgame payoff — by scarcity, not by perk lock.

- The **Gem Pouch** — a lesser power granted at startup — opens the socket
  menu anywhere: a native in-process ImGui menu (ENGINE_NOTES §9), items
  pane + gems pane, full mouse/keyboard AND controller navigation (m32f,
  m36e), 4 runtime skins (m24). Any inventory item is socketable (native
  reach — no worn-only constraint); one-click gem swap replaces a filled
  socket (m35e). Feeding and destruction happen at enchanting stations,
  which by default open this same menu (m18 station takeover).
- Loose gems are STORED in a hidden pouch container, not the player
  inventory (m27); the co-save tracks the pouch ref and recovers gems if
  it's ever lost (m32c/m32d).
- Effects are applied natively: the DLL builds ONE combined
  created-enchantment from all filled sockets and sets it as instance
  `ExtraEnchantment` + `ExtraTextDisplayData` + `ExtraUniqueID`
  (ENGINE_NOTES §1/§3). State persists on the instance and in the co-save —
  gems travel with the item when dropped, sold, or stored.
- Item names are rewritten on the instance ("Fire II Iron Sword" style —
  **NO BRACKETS**: some UI mods strip bracketed tags, the M2i lesson).
- NPCs never level gems. Enemies can spawn *wearing* socketed gear (m19,
  themed per enemy archetype) — the socketed piece is itself the loot.

### Onboarding — CUT (marth, 1.0 decision)

**No in-game onboarding ships.** The earlier "Jeweler's Primer" readable +
one-time contextual hint design was cut for 1.0: no tutorials, no guides, no
hints — the README's "How to play" section is the onboarding. Nothing is
forced either way: no intro quest, no pre-socketed starter items beyond the
starter gem grants; the player gets the Gem Pouch power and explores from
there.

### Stacking cap — no more than 2 of the same effect (SHIPPED m35c)

At most **2 gems of the same effect** contribute across all worn gear; the
3rd+ copy of that effect is **fully inert** (DECIDED — only the two
highest-level instances count). This is the primary balance guard: it caps any
single stat at `2 × V` (× perks × mastery) regardless of total socket count, so
the socket layout adds build *breadth*, not single-stat runaway. It also keeps
resists sane — 2× elemental-resist V (30%) = 60%, under the ~85% cap. Enforced
at socket/rebuild time.

**Known limitation as of v1.0.6 — the cap settles at the next socket-action or
load, not on a plain equip swap.** The cap is baked into each item's built
enchant during a *rebuild* (socket / unsocket / level-up / load). A plain
equip/unequip of an already-socketed item fires no rebuild (there is no
`TESEquipEvent` sink), so two edge transitions are transiently wrong until the
next rebuild: (1) an item socketed while **unworn** builds full-strength, so
equipping it as a 3rd copy mid-session over-applies (a `2×V → 3×V` balance
bypass) until a socket action or reload; (2) unequipping one of the top-2 leaves
a capped 3rd copy inert (under-applied) until the same. Both self-correct on the
next socket action or load. The over-apply is a knowable-but-obscure balance
exploit (socket-unworn, then equip), not a crash or data-loss issue. The proper
fix is the **1.0.7 runtime tally-cap** — see
[ROADMAP-1.0.7-tally-cap.md](ROADMAP-1.0.7-tally-cap.md) — which moves cap
enforcement off the build path onto the active-effect list and also retires the
`applyCap`/owner-gating machinery that caused v1.0.6's worst blocker.

## 5. Support gems (dual-slot linked only) — SHIPPED (m36 series, v0.47–v0.49)

**Focus, Conduit, and Echo ship and work in-game.** Support gems are inert
alone; an item is LINKED when exactly one support gem shares it with exactly
one normal gem — then the support transforms the normal. Two supports on one
item = both inert; a lone support = the item reverts to plain.

**Support gems level in 3 tiers, I–III (SHIPPED).** The three tiers span the
same power range as normal gems' I–V (support III ≈ normal V). They earn XP
the same way — but ONLY while socketed AND linked to a working normal gem (an
inert, unlinked support gem earns nothing). They do **not** birth: scarcity
is preserved by acquisition, not replication.

Shipped behaviors (the source of truth is `RebuildInstanceEnchant` +
the Echo sinks in `native/plugin.cpp`; per-tier levers are `tierParam` in
the catalog):

| Gem | Shipped behavior | Tier lever (I/II/III) |
|---|---|---|
| **Focus** | Linked **elemental** gem (Fire/Frost/Shock themes — damage *and* resist gems — plus Chaos) gains bonus magnitude | +20% / +35% / +50% |
| **Conduit** | **Cross-domain adapter**: an off-domain gem works through its same-theme sibling of the ITEM's domain (e.g. a weapon-domain Fire gem in armor expresses as the armor-domain Fire gem), at the sibling's own calibrated magnitude × the transfer ratio. Same-domain gems pass through unchanged; an off-domain gem with no clean sibling stays inert (units stay honest — no raw damage-vs-resist transfer) | ×0.50 / ×0.75 / ×1.00 |
| **Echo** (weapon) | Linked elemental on-hit effect gains an **area** — delivered via `TESHitEvent` (the enchantment's area field is inert engine-side), hitting hostile/in-combat actors around the target with a bystander filter | area scales with tier (fraction 0.34/0.67/1.0) |
| **Echo** (armor) | Linked gem's constant effect is **shared with current followers** — a self-expiring, save-safe spell, dispel-before-recast so it never stacks | share fraction 0.34 / 0.67 / 1.0 |

Acquisition (SHIPPED, scarcity model — **not found before player level 15**):
- **Rare boss/dragon loot** (m36h): a low `fSupportDropChance` (default 3%,
  INI-tunable) roll on boss/dragon kills at player level ≥ `iSupportMinLevel`
  (default 15).
- **Hand-placed guaranteed copies** (m36i), one each in famous
  non-respawning containers:
  - **Focus** — Avanchnzel (Boilery boss chest; the Dwemer/Lexicon ruin).
  - **Conduit** — The Midden: the Atronach Forge Offering Box
    (transmutation theme; persistent ref).
  - **Echo** — Ustengrav Depths boss chest (Jurgen Windcaller; the Voice).

Future roster (designed, NOT shipped — post-1.0 candidates once wanted):
**Reprisal** (linked proc fires back at melee attackers / magnitude doubles
when struck), **Siphon** (proc heals a fraction / slow matching regen),
**Final Stand** (low-health burst / magnitude spike on a cooldown). The
per-gem support interaction matrix (`data/support_matrix.json`) remains the
deliverable gating any roster expansion — the shipped three sidestep it by
construction (Focus/Echo restrict to elemental linkage; Conduit maps only
clean theme siblings).

## 6. Perk tree rework — SHIPPED (m20 installer wiring + m34 affinities)

The **installer** rewrites the load order's winning enchanting perk tree
(whatever won the AVEnchanting override — vanilla, Requiem, Ordinator, ...)
into MEO's perks, writing `MEO - Patch.esp`; other perks it finds in the
tree are offered keep/drop interactively. The DLL applies all perk math at
stamp/re-stamp time via `HasPerk` checks — the PERK records themselves are
plain flag perks. The installer performs full perk-tree surgery: it deletes
the winning tree's enchanting-craft nodes and inserts MEO's own nodes with
fresh grid positions and connections — node *positions* are NOT kept (on
vanilla, zero nodes survive). Only the SKILL REQUIREMENTS carry over, as
`GetBaseActorValue(Enchanting) >= req` conditions baked into MEO.esp's PERK
records. The three elemental affinities are PARALLEL CHOICES off the
Attunement hub — the trio sits adjacent at the same depth, never chained one
behind another (m36k); Attunement (5 ranks) and Feeder→Twinned→Jeweler are
the sequential branches; Gem Cutter and Facet Insight are single choices.
Enchanting skill still exists and levels from socketing, feeding, and gem
level-ups; an MCM Debug toggle (`bDebugAllPerks`) force-grants every MEO
perk for testing. The table below lists each MEO perk and the vanilla-era
skill requirement it inherits:

| Vanilla perk (req) | Becomes | Effect |
|---|---|---|
| Enchanter 1–5 (0/20/40/60/80) | Gem Attunement 1–5 | Socketed gems +5% potency per rank (script-applied multiplier) |
| Soul Squeezer (20) | Gem Cutter | Gems earn +50% XP |
| Soul Siphon (40) | **Soul Feeder** | Unlocks feeding filled soul gems to socketed gems for instant XP |
| Fire Enchanter (30) | Pyrestone Affinity | Fire/Chaos gems +25% |
| Frost Enchanter (40) | Froststone Affinity | Frost/Chaos gems +25% |
| Storm Enchanter (50) | Stormstone Affinity | Shock/Chaos gems +25% |
| Insightful Enchanter (50) | Facet Insight | Skill & attribute armor gems +25% |
| Corpus Enchanter (70) | **Twinned Fitting** (penultimate) | Chest gains a second, linked socket (gloves are already dual by default) |
| Extra Effect (100) | **Master Jeweler** (final) | Main-hand weapon gains a second, linked socket |

Weapon charge / recharging is obsolete: gem procs are free, balanced by
magnitude; soul gems' economy role is Soul Feeder fuel.

Perk math is applied in script when (re)applying enchantments, so all perks
are plain flag perks — no fragile entry-point records. When any relevant perk
changes, the heartbeat re-applies worn enchantments with new magnitudes.
(Native era: "script-applied" = DLL-applied at stamp/re-stamp time via
`HasPerk` checks; the ESP overrides only rename/redescribe the PERK records.)

### Non-perk enchanting boosters (2026-07-07, SHIPPED m11)
Everything in the game that boosts enchanting must boost something relevant
to MEO. The perk table above covers the tree; the remaining boosters —
**Fortify Enchanting potions, Seeker of Sorcery (black book), Ahzidal's set
bonus** — all raise the Fortify Enchanting actor value. Mapping: while the
carrier's Fortify Enchanting AV is non-zero, **Gem XP gain scales by
`1 + AV/100`** (kill and soul-feed alike) — the "enchant better while
buffed" fantasy, timed like enchanters' potion sessions, trivially native
(read the AV in GrantGemXP). Potency stays perk-driven so gear/potion
stacking can't inflate magnitudes mid-combat.

### "Don't touch what isn't ours" rules
- ENCH `enchType == 12` (staff/wand) → never convertible, never modified.
- Soulbound FLST (artifacts, quest items, scripted effects) → sealed.
- Items with keyword `MagicDisallowEnchanting` or the quest-item flag → sealed.
- Loot the conversion table doesn't cover keeps its literal vanilla ENCH
  record (§2) — MEO never rewrites base records at runtime, only swaps
  covered instances via engine flows.
- Perk-tree changes live in the installer-generated `MEO - Patch.esp`,
  built against whatever tree actually wins in the user's load order
  (vanilla, Requiem, Ordinator, ...) — no FOMOD variants, no hand-made
  compatibility patches.

## 7. Socket state — the shipped native model (marker mechanism RETIRED)

The item is still the database, but natively. Each socketed item instance
carries a self-describing engine bundle (ENGINE_NOTES §1):

- **`ExtraUniqueID`** — stable per-instance identity (also prevents
  stacking with plain copies). The uid is re-keyed when an item hops
  containers (`TESContainerChangedEvent` re-key, m19).
- **`ExtraEnchantment`** — ONE combined created-enchantment built from all
  filled sockets, forced free (cost 0, `kCostOverride`, charge `0xFFFF`) so
  it never depletes and applies instantly (ENGINE_NOTES §3).
- **`ExtraTextDisplayData`** — the live display name (no brackets).

The **SKSE co-save** holds the socket/leveling index: per-instance records
`{base, uid, slot, gid, level, xp}` plus globals (pouch ref, grant latches),
versioned and self-migrating (v3 → current). Loose leveled gems carry the
same record shape so a gem's identity survives socket ↔ loose transitions.
On level-up/socket/unsocket the DLL rebuilds the combined enchant and name
in place.

HISTORICAL: the Papyrus-era design encoded socket state as hidden zero-cost
"marker" effects inside the enchantment so a script could decode state from
the item alone (Papyrus cannot durably identify instances). P0 validated it;
the native build made it unnecessary and it never shipped.

## 8. Runtime architecture (shipped)

Everything lives in **`MEO.dll`** (CommonLibSSE-NG; `native/plugin.cpp`):

- **Startup/setup** (`EnsurePlayerSetup` at load): grants the Gem Pouch
  power, starter gems, hand-placed support gems (direct container placement
  into non-respawning refs, m36i), creates/recovers the hidden pouch
  container, applies the temper-perk toggle.
- **Event sinks** (ENGINE_NOTES §4): `TESDeathEvent` (kill XP, corpse gem
  drops, support-gem boss rolls), `TESCellAttachDetachEvent` +
  `TESObjectLoadedEvent` (world-weapon pre-socket rolls, NPC worn-gear
  stamps, Soul Cairn Mentor grant, conversion), `TESContainerChangedEvent`
  (uid re-key + conversion on arrival), `MenuOpenCloseEvent` (station
  takeover, MCM live re-read, load-anchored reapply), `TESHitEvent` (Echo
  AoE), plus a follower-share heartbeat for Echo armor.
- **The gem menu**: in-process ImGui drawn inside the game's DX11 present
  (ENGINE_NOTES §9) — two panes, station mode (feed/destroy), 4 skins,
  mouse/keyboard + controller.
- **Co-save serialization** (§7 above; ENGINE_NOTES §6).
- **Calibration consumption**: at `kDataLoaded` the DLL overlays
  `meo_calibration.json` (per-list magnitudes, riders, rank ladders, the
  conversion table) onto the compiled `GemCatalog.h` defaults.

**`MEO.esp`** (generated, FormIDs frozen): gem MISC records (51 normal × 5
levels + 3 support × 3 tiers + Mentor), marker/pouch MGEFs and the pouch
SPEL/CONT, MEO PERK records (0x810–0x81C), the MCM quest, SEQ. **`MEO -
Patch.esp`** (installer-generated per user): the enchanting perk tree
rewrite. Papyrus surface is ONE compile-time MCM stub; there are no runtime
Papyrus systems, no tracker quest, no heartbeat script.

## 9. Save safety

- **Install mid-save: safe.** New records + the installer's perk-tree patch;
  no vanilla record is edited destructively. No new game required.
- **Update in place: safe by design.** FormIDs are frozen post-release; the
  co-save migrates itself forward across versions; recovery passes run every
  load (pouch/gem recovery m32c/m32d, duplicate-effect dispel m32e/m24b).
- **Uninstall mid-save: mitigated, not magic.** Instance enchantments and
  display names persist on item instances like any runtime-enchanted gear;
  without the DLL they simply stop leveling (the combined enchant keeps
  working as a normal enchantment). Removing the ESPs orphans converted/
  socketed instances' base forms like any content mod. Ecosystem-normal
  residue; ReSaver for the rest.
- **Dev/test workflow:** keep one read-only baseline save that has never
  seen the plugin; reload it for every test of a build whose *records*
  changed. When in doubt, clean reload — persisted state lies.

## 10. Build history (the P0–P4 phase plan is COMPLETE/superseded)

The original phases shipped, in more steps than planned — `CHANGELOG.md`
(repo root) is the authoritative history. Milestone arc: P0 Papyrus
validation (v0.0.x) → native pivot (m0–m2, co-save + socket bundle) →
playable socket core + leveling (m3–m5) → ImGui menu (m6) → loot/spawns,
MCM, armor gems, stations, perk effects (m7–m11) → multi-socket + on-load
reactivation (m13–m17) → station takeover + gem economy (m18–m19) →
installer: perk tree, calibration, riders, conversion (m20–m26) → pouch
storage, rank ladders, portability, controller (m27–m32) → temper toggle
(m33) → affinity perks + Facet Insight (m34) → audit fixes, per-list
magnitudes, stacking cap, gem swap (m35) → support gems + circlets (m36).

Compatibility baseline (verified 2026-07-07, still true): `MEO.esp` declares
a single master (Skyrim.esm) and references NO external records — the DLL
resolves effect MGEFs from the live load order at runtime and disables gems
whose master is absent; the installer, not FOMOD variants, adapts MEO to
the list.

Post-1.0 candidates: curated unique gems from artifact families (old P4);
Reprisal/Siphon/Final Stand support gems (§5); follower gem testing at
scale; the Unknown-Enchants-style compat pass (§2); Summermyst coexistence
check.
