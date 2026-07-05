# Marth's Enchanting Overhaul (MEO) — Design Document

Materia-style enchanting for Skyrim SE. Enchantments become **gems**: removable,
socketable, leveling items. This fully replaces vanilla enchanting for weapons
and armor. Staves, wands, and scripted artifacts are detected and left alone.

Target: vanilla SE + SKSE core, with a Requiem/LoreRim compatibility plugin as
a FOMOD option. Built with the toolchain in `MANUAL_MOD_CREATION_GUIDE.md`
(Python ESP generator, Papyrus via Proton wine, FOMOD packaging).

Some of this document will need rewriting as a DLL implementation will be used.

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
- Group ENCH records by their **set of MGEFs** → one family per set.
- Vanilla magnitude tiers (`...01`–`...06`) collapse into **gem levels I–V**
  (tier 6 folds into V). Vanilla's own tier magnitudes seed the level curve.
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

- Gems are MISC items, one form per type × level: `MEO_Gem<Type><1..5>`,
  plus support gems (single form each — they don't level).
- **XP accrues only while socketed in player-worn gear.** Fungibility is by
  type+level; partial XP toward the next level is banked per type+level pool
  in the tracker quest, so unsocketing never loses progress.
- XP sources:
  - Player kills: the tracker polls `Game.QueryStat` kill stats on the
    heartbeat (30 s) plus on menu-close/`OnPlayerLoadGame`; every kill awards
    AP to every socketed gem (FFVII: AP per battle to all equipped materia).
  - **Soul feeding**:at a bench consume a filled soul gem
    from the Gem Pouch menu to grant XP to one gem in inventory — petty 5,
    lesser 12, common 25, greater 60, grand/black 200 (MCM-tunable).
    By default, id likje one Grand soul gem to encompass the XP from Lvel I to II
    thats the total extractable amount by destroying a gem of any level when a
    gem is destroyed you can only reclaim 1/10th of the ap into a soul gem.
     (Soul Feeder perk, skill 40): Causes Soul Gems(petty through grand to contain(from both feeding and extracting) 
     to conatain twice the amount of AP.
- Default level thresholds (MCM-tunable globals): cumulative 400 / 1200 / 3600 /
  100000 AP to reach II/III/IV/V(dependent on rarity and power), then +150000 to Master → **birth**: a new
  level-1 gem of the same type appears in inventory with a notification.


## 4. Sockets and the Gem Pouch

| Gear | Sockets |
|---|---|
| Cuirass / chest (slot 32) | **2 (linked)** |
| Helmet, gloves, boots, shield, ring, amulet, weapons | 1 |
| Gloves + boots with **Twinned Fitting** perk | 2 (linked) |
| Weapons + rings + amulets with **Master Jeweler** perk | 2 (linked) |

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

## 5. Support gems (dual-slot linked only)

Support gems are inert alone; when sharing a dual-slot item with a normal gem
they modify it. One support + one normal per item (two supports = both inert).
They don't level and cannot be birthed — acquisition is the scarcity model:

- **Hand-placed, FFVII style**: each support gem has one fixed, famous
  location (a Dwemer ruin's master-locked vault, a College questline reward,
  a dragon-priest hoard, ...) — injected into specific existing containers /
  quest rewards by the startup quest. Final location list chosen in P2.
- **Rare boss-chest loot**: very low-weight entries in boss leveled lists as
  a repeatable RNG backstop.

Roster (FFVII support/independent materia, adapted):

| Gem | In a weapon (linked gem) | In armor (linked gem) |
|---|---|---|
| **All** | On-hit effect gains an area — AoE proc around the target | Constant effect is shared with current followers (PO3 `GetPlayerFollowers`, refreshed on heartbeat) |
| **Added Effect** | An ARMOR gem rides the weapon: its stat becomes an on-hit debuff (Fortify Health gem → hits damage max health) | A WEAPON gem rides the armor as protection: **immunity/strong resist to that effect** (Paralyze gem → paralysis immunity; Fire gem → fire resist) — faithful FFVII semantics |
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
| Corpus Enchanter (70) | **Twinned Fitting** (penultimate) | Gloves and boots gain a second, linked socket |
| Extra Effect (100) | **Master Jeweler** (final) | Weapons, rings, and amulets gain a second, linked socket |

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
