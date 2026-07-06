# Marth's Enchanting Overhaul (MEO) — Design Document

Materia-style enchanting for Skyrim SE. Enchantments become **gems**: removable,
socketable, leveling items. This fully replaces vanilla enchanting for weapons
and armor. Staves, wands, and scripted artifacts are detected and left alone.

Target: vanilla SE + SKSE core, with a Requiem/LoreRim compatibility plugin as
a FOMOD option. Built with the toolchain in `MANUAL_MOD_CREATION_GUIDE.md`
(Python ESP generator, Papyrus via Proton wine, FOMOD packaging).

Some of this document will need rewriting as a DLL implementation will be used.

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

Relationship to the §7 marker mechanism: the marker-in-enchantment trick is
the **Papyrus-era** way to be stateless (no side table to bloat). The native
endgame supersedes it — the DLL's co-save index becomes the source of truth
and reapplies real effects on load, so markers may be dropped once native
lands. Both designs need the SAME underlying guarantee: that applying a
runtime enchantment to an item is reliable across save/load, re-equip, and
rebuild. **P0 validates exactly that guarantee in Papyrus**, which is why P0
is worth doing before committing to either the marker path or the native
index.

### Native/DLL plan — reuse MRO's proven toolchain, don't re-derive
The sibling project MRO has a working CommonLibSSE-NG pipeline built entirely
from Linux; copy it rather than reinventing. Load-bearing facts (full detail
in MRO's `docs/NATIVE_REWRITE_PLAN.md` and `docs/DEBUGGING.md`):
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

1. Loot an enchanted sword — its enchantment **is** a gem, sitting in the
   item's socket. Pop it out from the Gem Pouch menu, anywhere, any time.
   Gems removed from found loot are always **level 1** regardless of the
   item's vanilla tier.
2. **Socket** gems into any eligible gear. Free, lossless, reversible.
3. Gems socketed in your worn gear earn **XP from your kills** and level
   I → V; magnitude scales with level. Filled soul gems can be fed to a
   socketed gem for instant XP (with the Soul Feeder perk).
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
  effect. Deriving each effect's base/max programmatically (from the enchant
  formula or a calibrated table) is a P1 data task — see §3.
- Multi-effect generic combos (mage robes = Fortify School + Magicka Regen,
  Chillrend = Frost + Paralyze, ...) are NOT separate gems — they decompose
  into their component gems, one per socket. Robes' double effect is
  reproduced by the dual-slot chest piece.
- `enchType == 12` (staff enchantment, 71 records) → **excluded**; staves and
  wands keep vanilla behavior end to end.
- Junk/system effects (BYOH house parts, traps, shout enchants, bound-weapon
  FX, draugr streaks) never reach the catalog because only enchantments found
  on obtainable ARMO/WEAP with a generic `Ench*` family qualify.

### Weapon gems (~15) — fire-and-forget, on-hit
Fire, Frost (with slow), Shock, Chaos (DLC2 tri-element), Absorb Health,
Absorb Magicka, Absorb Stamina, Damage Magicka, Damage Stamina, Fear,
Banish, Paralyze, Soul Trap, Turn Undead, Sun (Dawnguard, undead-only).

### Armor gems (~30) — constant-effect
- Attributes: Fortify Health / Magicka / Stamina, Regen ×3, Carry Weight.
- Resists: Fire, Frost, Shock, Magic, Poison, Disease.
- Skills: the 18 fortify-skill effects (One-Handed, Two-Handed, Archery,
  Block, Smithing, Heavy/Light Armor, Pickpocket, Lockpicking, Sneak,
  Alchemy, Speech, Alteration, Conjuration, Destruction, Illusion,
  Restoration, plus Fortify Shout (Talos) and Unarmed (Pugilist)).
- Utility: Muffle, Waterbreathing.

Placement rules: weapon gems only socket into weapons, armor gems only into
apparel — EXCEPT via the Added Effect support gem (below). Vanilla slot
restrictions per effect (e.g. Fortify Archery on helmets) are dropped; any
armor gem fits any apparel slot.

### Converting found loot
A vanilla-enchanted item's ENCH is matched against the family map; the first
time the player opens its sockets, the vanilla enchantment is decoded as its
component gems (1–2 for generic families) at **level 1**. Any socket
operation replaces the vanilla ENCH with MEO's rebuilt, marker-carrying
enchantment (§7). Until the player touches it, the item is byte-for-byte
vanilla — zero compatibility surface for unlooted/NPC gear.

### World-visible socketed gear is a native capability (not Papyrus)
A weapon lying on the ground showing "Fire I Glass Dagger" — and staying that
way through pickup with **no transform-on-pickup** — is unreachable in Papyrus.
Runtime enchantments (`WornObject.CreateEnchantment`) exist only on the *worn*
slot, so nothing holds a ground item's socket state, and any script approach
would necessarily mutate the item at `OnContainerChanged`/`OnEquip` (i.e.
transform it at pickup), which is exactly the jarring behavior we reject. The
only correct model is a DLL that **paints name + enchant on reference 3D-load**
wherever the item renders (corpse, ground, container, worn), so the item is
never "plain" and never transforms. This is a **native milestone** (M2+), not a
Papyrus feature. Near-term loot policy accordingly: enemies drop **plain gear +
loose gems** (gems are the loot); the player sockets. Player-socketed gear that
is dropped *may* retain its enchant via `ExtraEnchantment` traveling with the
stack — worth a spot-check, but the reliable form is native-painted.

### Compatibility — enchant-visibility mods (loot UX)

Two popular mods depend on how enchanted gear reads before pickup; both must
keep working:
- **See Enchantments SE** (weapon glow / visual effects in the world) — works
  unchanged: found loot retains its real ENCH, so the glow is vanilla. TODO
  verify the runtime enchantment applied after socketing (§7) still carries a
  visual shader when the item is equipped; if not, reattach one.
- **Unknown Enchants**-style "(U) you haven't learned this" indicators — read
  the vanilla known-enchantment (disenchant) list, which MEO never populates,
  so they would flag everything forever. Fix (semantic match): when the player
  first **extracts a gem type**, mark that base enchantment KNOWN, so "(U)" then
  means "a gem type you don't own yet" — exactly the loot-worth signal,
  repurposed. Requires an API to add to the known-enchant list (verify SKSE /
  PO3 in P3); **fallback** if none exists: MEO ships its own lightweight loot
  tag (display-name suffix or on-loot hint) driven by gem ownership. Either way
  the UX survives. (P3 compat task.)

### Unique / artifact enchantments (68 families)
Policy: artifacts with scripted, quest, or one-off enchantments (Ebony Blade,
Spellbreaker, Ring of Namira, nightingale sets, masks, ...) are **soulbound**:
their gems cannot be removed and their sockets are sealed. They keep working
exactly as vanilla. Implemented as an FLST exclusion list of ENCH FormIDs +
a heuristic (any effect whose MGEF archetype is Script and is not in the
generic catalog). A curated subset with clean effects (e.g. Silent Moons)
can graduate into extractable **unique gems** (level normally, cannot birth)
in a later version.

## 3. Levels, XP, birthing

- Gems are MISC items, one form per type × level: `MEO_Gem<Type><1..5>`.
  Support gems level too, but in **3 tiers** (`MEO_Support<Type><1..3>`); see §5.
- **XP accrues only while socketed in player-worn gear.** Fungibility is by
  type+level; partial XP toward the next level is banked per type+level pool
  in the tracker quest, so unsocketing never loses progress.
- XP sources (AP):
  - **Kills** — every kill awards AP to *every* socketed gem in parallel
    (FFVII: AP per battle to all equipped materia): **1 AP per standard enemy,
    10 AP per boss**. Tracked via `Game.QueryStat` on the heartbeat + menu-close.
  - **Soul feeding** — at a bench, consume a filled soul gem to grant AP to one
    gem: petty 5, lesser 12, common 25, greater 60, grand/black 200
    (MCM-tunable). With the **Soul Feeder** perk, soul gems hold **2× AP** (both
    when fed and when reclaimed), so a Grand ≈ 400 AP ≈ one full Level I→II.
  - **Gem destruction** — destroying a gem reclaims **1/10 of its AP** into a
    soul gem (the only sink that recovers investment).

- **Level thresholds (default A-tier, MCM-tunable): cumulative 400 / 1,200 /
  3,600 / 10,000 AP to reach II / III / IV / V.** Reaching **10,000 = Level V =
  Master**, and at Master the gem **births** one fresh Level-1 copy (a
  notification fires); a mastered gem is capped and stops accruing AP (FFVII
  model). Birthing is the only way to replicate a gem.
- **Per-gem XP scales by power tier** ("dependent on rarity and power"): the
  threshold is `base × xp_mult`, with **S ×1.5** (build-defining, e.g. Chaos,
  Stagger, attributes, Magicka Rate, crafting skills), **A ×1.0** (most gems),
  **B ×0.6** (situational: control gems, trivial skills), **U** utilities don't
  level. So an S-tier gem masters at 15,000, a B-tier at 6,000. Each gem's tier
  lives in `data/gem_catalog.json` (see §3 scaling model / BALANCE.md).

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
| MEO Gem V + Attunement perks | 49 | 98 | × **1.40** (rewritten tree, +8%/rank ×5) |
| MEO Gem V + perks + full CSF mastery | 73 | 147 | × **1.50** more (MRO/CSF over-cap) |

- Multipliers **stack multiplicatively** on the unperked Level V value:
  `V × 1.40 (perks) × 1.50 (mastery) = V × 2.10`. (35×2.1≈73, 70×2.1=147.)
- The **+40% Attunement** tier is MEO's own perk tree (§6) — available on
  vanilla-core.
- The **+50% mastery** tier is an OPTIONAL top layer gated on a CSF enchanting
  mastery (shared with / modeled on the sibling MRO). Without CSF present, the
  ceiling is the perked Level V (49 / 98). Keep MEO standalone-playable; the
  mastery tier is synergy, not a hard dependency.
- **Levels II–IV** interpolate between I and V. Default: linear (Fire
  12/17.75/23.5/29.25/35; Fortify Health 20/32.5/45/57.5/70), a per-gem curve
  shape is MCM/data-tunable. Each of the ~45 gem families needs its own
  [base, max] pair; sourcing those is the P1 data task flagged in §2.

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
- Aspiration ("would be nice"): make enemies literally *beholden to the system*
  — their enchanted gear IS MEO gems at the capped level, so the whole world
  runs one ruleset. This is a larger feature (runtime conversion/cap of NPC
  gear, or record-level edits) deferred to P4+; the light-touch cap above is
  the v1 behavior. Until then NPC gear keeps its vanilla ENCH (see §2
  "Converting found loot") and simply reads as a low-level gem's worth of power.

### Configuration & tunability (portability contract)

Every balance number in this section is load-order-dependent (the Requiem
anchors differ on vanilla / Adamant / Enderal / etc.). Per DYNAMIC_OR_DROP, a
baked number is mistuned everywhere but this machine, so tuning is exposed, not
frozen. The architecture already permits it: gem magnitudes are computed at
socket/rebuild time in script (the same path perk multipliers already use), so
NOTHING magnitude-related is baked into MGEF/ENCH records.

- **MCM (runtime fine tuning)** — global and per-category scalars, not 90
  per-gem sliders: master magnitude x, weapon-gem x and armor-gem x (and/or
  per-family-group), the Attunement-perk % (default 40), the mastery % (default
  50), the I->V curve shape, XP rates/thresholds, soul-feed values, and the
  enemy cap level. Changing any re-applies worn gems on the next heartbeat (S6).
- **FOMOD (install-time baseline)** — pick a balance baseline that seeds the
  per-gem [base, max] data set: e.g. "Requiem/LoreRim", "Vanilla-ish",
  "Hardcore". FOMOD only chooses which data installs; the MCM scalars ride on
  top for per-playthrough adjustment.
- Per-gem [base, max] anchors ship as DATA (the P1 sourcing task, S2), chosen by
  the FOMOD baseline; the MCM never edits them individually, it scales them.
  Sane knob count, yet any load order can land an appropriate absolute range.

## 4. Sockets and the Gem Pouch

Socket layout (Marth, DECIDED — supersedes the earlier "cuirass dual by
default" sketch):

| Gear | Unperked | Perked |
|---|---|---|
| Head | 1 | 1 |
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
  materia is still an endgame payoff — by scarcity, not by perk lock.

- The **Gem Pouch** — a lesser power granted at startup
  (plus an MCM-bindable hotkey) — opens the socket menu anywhere: pick a worn
  item → view sockets → insert / remove. Feeding and extracting are done at En chanting stations
- Socket operations target *worn* gear (a WornObject API constraint, §7);
  the menu offers to temp-equip an inventory item being edited.
- Effects are applied with SKSE `WornObject.CreateEnchantment()` on the worn
  instance and persist on that instance in the save — gems travel with the
  item when dropped, sold, or stored, no tracking needed.
- Item names get a suffix via PO3 `SetDisplayName` ("Iron Sword
  [Fire II • Soul Trap I]") purely as UI sugar.
- NPCs never interact with sockets.

### Onboarding — note + contextual hint (DECIDED)

Low-intrusion, no forced quest:
- Startup grants the Gem Pouch power AND adds a readable primer to the player's
  inventory ("A Jeweler's Primer") covering: the Gem Pouch power, that found
  enchantments extract as gems, gloves being the dual/link slot, and gems
  leveling through use.
- A **one-time contextual hint** fires the first time the player acquires an
  enchanted item ("Its enchantment can be extracted as a gem — open the Gem
  Pouch"). Latched via a GLOB so it never repeats. Fail-open if the flag is
  missing.
- Nothing is forced: no intro quest, no pre-socketed items. The primer + hint
  point at the Gem Pouch and the player explores from there.

### Stacking cap — no more than 2 of the same effect (DECIDED)

At most **2 gems of the same effect** contribute across all worn gear; the
3rd+ copy of that effect is **fully inert** (DECIDED — only the two
highest-level instances count). This is the primary balance guard: it caps any
single stat at `2 × V` (× perks × mastery) regardless of total socket count, so
the socket layout adds build *breadth*, not single-stat runaway. It also keeps
resists sane — 2× elemental-resist V (30%) = 60%, under the ~85% cap. Enforced
at socket/rebuild time.

## 5. Support gems (dual-slot linked only)

Support gems are inert alone; when sharing a dual-slot item with a normal gem
they modify it. One support + one normal per item (two supports = both inert).

**Support gems level — 3 tiers, I–III (DECIDED).** The three tiers span the
same power range as normal gems' I–V (support III ≈ normal V). They earn XP the
same way — while socketed AND linked to a working normal gem (an inert, unlinked
support gem earns nothing). They do **not** birth: scarcity is preserved by
acquisition, not replication.

- **How a tier expresses depends on the gem's function:**
  - **All** — the top tier fires *every hit*; lower tiers fire only sometimes.
    All I = occasional proc, All II = frequent, All III = every hit (full AoE /
    full follower share). This makes All meaningfully worse until mastered,
    which is the point of it being rare and leveled.
  - **Elemental / Counter / Absorption / etc.** — the tier scales that gem's own
    lever: Elemental +% (e.g. +20/+35/+50), Counter proc chance/magnitude,
    Absorption heal %, Final Stand threshold/burst. Each gem's I–III curve is
    tuned per function (P2 detail).

Acquisition (scarcity model — **not found before ~level 15**):
- **Hand-placed, FFVII style**: each support gem has one fixed, famous
  location (a Dwemer ruin's master-locked vault, a College questline reward,
  a dragon-priest hoard, ...) — injected into specific existing containers /
  quest rewards by the startup quest. Final location list chosen in P2.
- **Rare boss-chest loot**: very low-weight entries in **level-15+** boss
  leveled lists as a repeatable RNG backstop, so they cannot show up early.

Roster (FFVII support/independent materia, adapted):

| Gem | In a weapon (linked gem) | In armor (linked gem) |
|---|---|---|
| **All** | On-hit effect gains an area — AoE proc around the target | Constant effect is shared with current followers (PO3 `GetPlayerFollowers`, refreshed on heartbeat) |
| **Added Effect** | An ARMOR gem rides the weapon: its stat becomes an on-hit debuff (Fortify Health gem → hits damage max health) | A WEAPON gem rides the armor as protection: **immunity/strong resist to that effect** (Fire gem → fire resist; Stagger gem → stagger/knockdown resist) — faithful FFVII semantics |
| **Elemental** | Linked Fire/Frost/Shock/Chaos gem +50% magnitude and hits sunder the target's matching resistance | Linked elemental gem grants resistance equal to 2× its damage magnitude |
| **Counter** | Linked gem's proc also fires on targets who hit you in melee while the weapon is drawn | Linked armor gem's magnitude doubles for 10 s whenever you're struck (stacking refresh) |
| **Absorption** (HP/MP Absorb merged) | Linked gem's on-hit proc also heals you (health if the gem damages health/stamina, magicka if it damages/absorbs magicka) for 25% of magnitude | Linked constant gem also grants slow regen of the matching attribute |
| **Final Stand** (Final Attack) | Dropping below 20% health triggers the linked gem's effect as a burst around you (60 s cooldown) | Dropping below 20% health triples the linked gem's magnitude for 15 s (60 s cooldown) |

V1 ships All, Added Effect, Elemental; Counter, Absorption, Final Stand
follow in P2 once the linking machinery is proven.

## 6. Perk tree rework

Vanilla node positions and skill requirements are kept (we override PERK
records in place — no AVIF tree surgery). Enchanting skill still exists and
now levels from socketing, feeding, and gem level-ups.

| Vanilla perk (req) | Becomes | Effect |
|---|---|---|
| Enchanter 1–5 (0/20/40/60/80) | Gem Attunement 1–5 | Socketed gems +8% potency per rank (script-applied multiplier) |
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

### "Don't touch what isn't ours" rules
- ENCH `enchType == 12` (staff/wand) → never convertible, never modified.
- Soulbound FLST (artifacts, quest items, scripted effects) → sealed.
- Items with keyword `MagicDisallowEnchanting` or the quest-item flag → sealed.
- Untouched loot keeps its literal vanilla ENCH record (§2) — conversion only
  happens on first player socket interaction.
- The perk overrides only touch the 13 vanilla enchanting PERK records; the
  Requiem patch plugin overrides the equivalent Requiem perk records instead
  and drops the vanilla overrides (chosen at install time via FOMOD).

## 7. Socket state is stateless — the marker mechanism

Papyrus cannot durably identify an item *instance* (an inventory is
`(base form, count, extra data)`; three Iron Swords are indistinguishable to
a script). Side-table designs desync on drop/sell/stack. MEO instead derives
socket contents from the item itself:

- Every MEO-built enchantment carries, alongside its real effects, one
  hidden **marker effect** per socketed gem: a zero-cost, no-visual MGEF
  whose magnitude encodes `gemTypeIndex × 8 + level`.
- Reading an item = SKSE `WornObject.GetEnchantment()` →
  `GetNthEffectMagicEffect/Magnitude` → exact integer decode of the markers.
  Immune to perk multipliers rescaling the real effects, and to any future
  balance retune.
- Insert / remove / level-up are all "decode → edit gem list → rebuild
  enchantment (real effects at perk-adjusted magnitudes + markers)".
- The ONLY mod state is the XP pool per gem type+level in the tracker quest.

## 8. Runtime architecture

Per the field guide (§7):
- **MEO_StartupQuest** (SGE + Run Once): grants the Gem Pouch power, injects
  support gems into their hand-placed containers and boss LVLIs, version init.
- **MEO_TrackerQuest** (SGE, not Run Once, in SEQ): 30 s heartbeat — kill
  stat polling → XP awards → level-up/birth processing → perk-multiplier
  refresh → follower "All" sync.
- **MEO_MCMQuest** (SGE, not Run Once, in SEQ): SkyUI MCM — XP curve and
  soul-feed sliders, hotkey binding, notifications toggle, uninstall
  (unsocket everything) button.
- ESP contents: MISC gems (~45 types × 5 levels + support gems ≈ 235
  records), MGEF pool (real effects + marker MGEFs), GLOBs (tuning), FLSTs
  (gem form ladders, MGEF map, soulbound list), QUSTs, SPEL (Gem Pouch
  power), PERK overrides, LVLI edits, SEQ file.

Open engineering risks (validate in P0, per guide philosophy):
1. `WornObject.CreateEnchantment` semantics: persistence across
   unequip/save/load; whether re-creating replaces cleanly (removal path);
   behavior on stacked identical base items.
2. Menu UX without a station: message-box chains scale poorly past ~8
   options — evaluate UIExtensions/SkyUILib as an optional soft dependency.
3. On-hit AoE proc for the All gem (may need a dummy explosion SPEL cast at
   the target).

## 9. Save safety

- **Install mid-save: safe.** Only new records + in-place PERK overrides;
  loot stays literally vanilla until first socket interaction. No new game
  required.
- **Uninstall mid-save: mitigated, not magic.** Runtime enchantments and
  display-name suffixes persist on item instances, and quest script
  instances persist like any scripted mod. The MCM uninstall button
  (unsocket everything → strip name suffixes → stop quests → save → remove
  plugin) reduces residue to the ecosystem-normal minimum; ReSaver for the
  rest. Verify in P0 that a full uninstall pass leaves items usable.
- **Dev/test workflow:** keep one read-only baseline save that has never
  seen the plugin; reload it for every test of a build whose *records*
  changed (FormIDs, VMAD properties, quest flags). Script-logic-only
  changes may be iterated on a dirty save (Papyrus links .pex by name at
  load), but when in doubt, clean reload — persisted quest state lies.
- P0 explicitly validates: enchantment persistence across save/load and
  unequip, clean re-creation on rebuild, and post-uninstall item health.

## 10. Build phases

1. **P0 prototype**: catalog builder + minimal ESP (Fire gem I–V), Gem Pouch
   menu, socket/unsocket/readback, WornObject validation in-game.
2. **P1 core**: full generic catalog, loot conversion, XP/leveling/birthing,
   Soul Feeder, perk overrides, MCM.
3. **P2 support gems**: All / Added Effect / Elemental + dual-slot linking;
   hand-placement locations; then Counter / Absorption / Final Stand.
4. **P3 packaging**: FOMOD (core + Requiem patch page), LoreRim patch pass
   (Requiem perk records, Summermyst coexistence check).
5. **P4 uniques**: curated unique gems from artifact families.
