# Changelog — marth's Enchanting Overhaul

Newest first. Every version that reached the game shipped as a complete
standalone zip in `releases/vX.Y.Z/` (tag = release). Grouped by milestone
arc; point fixes are folded into their feature entry unless load-bearing.

## v1.0.1 — conversion blocker + enchanting XP from gameplay (m37, 2026-07-13)
- **Vendor-stock conversion fixed** (the "Major Wielding / Minor Alteration /
  Major Knight don't convert" blocker): `ConvertInventory` used to bail on any
  non-actor holder, so the vendor merchant-container sweep had been a silent
  no-op since m23 — which is exactly where a low-level player meets those
  fortify-skill armors. Container holders now convert too, via the engine's own
  flow (`PlaceObjectAtMe` → stamp → `AddObjectToContainer` with fromRefr).
  Container sweeps log a count so this failure class self-diagnoses.
- **Enchanting XP from regular gameplay** (soul gems are no longer the only
  source): skill XP now also comes from discovering a new gem family (one-time
  each), destroying a gem (× level), each gem level-up (× new level), and a
  tiny per-kill trickle scaled by Gem XP earned. Socketing grants none (it was
  abusable). All knobs INI-tunable (`fDiscover/Destroy/Level/GemXpSkillXP`).
  Co-save bumped to v11; older saves seed already-held families silently.
- **Installer census: turn-undead line recovered** (249 items): the
  "Blessed / Sanctified / Hallowed / Holy …" weapons are prefix-enchanted, so
  their names *end* with the base name and every suffix-shaped rung missed them.
  Added a root-suffix rung with the same shared-ENCH + template-root gate.

## v1.0.0 — first stable release (2026-07-13)
The full system, feature-complete: socketable leveling gems (I–V, birthing),
the hidden Gem Pouch, loot conversion, per-load-order calibration, the rebuilt
enchanting perk tree (elemental affinities as parallel choices; Gem Cutter →
Soul Feeder; Twinned → Jeweler), the support-gem pillar (Focus/Conduit/Echo,
boss-loot + hand-placed acquisition), the 2-of-a-kind stacking cap, gems are
non-sellable, circlets socketable, one-click swap, follower slotted-gear support,
4 menu skins + full controller. Lowercase 'marth' branding throughout.
(Visual-effect coverage beyond the base compatibility is a planned optional add-on.)

## v0.49.x — support gems complete (m36f–m36i, 2026-07-13) — 1.0 candidate
- Support gems as rare level-15+ boss/dragon loot (`fSupportDropChance`,
  `iSupportMinLevel`).
- Hand-placed guaranteed support gems at famous non-respawning containers:
  Focus in Avanchnzel, Conduit in the Midden's Atronach Forge Offering Box,
  Echo in Ustengrav Depths.
- Circlets are socketable (their own biped slot, distinct from helmets).
- Echo armor: linked gem's effect shared with current followers
  (self-expiring, save-safe, guaranteed exactly-once via
  dispel-before-recast).
- Conversion-defer observability; quieter logging.

## v0.48.x — Echo weapon AoE + menu/controller polish (m35d–m36e)
- Echo weapon: linked elemental on-hit effect gains an area — delivered via
  TESHitEvent (the enchantment's own area field is inert engine-side);
  hits hostile/in-combat actors near the target with a bystander filter.
- One-click gem swap: pick a gem to replace a filled socket atomically (m35e).
- Fixed worn effect stacking as base+renamed; retired the blinkless-reapply
  workaround (m35d).
- D-pad left/right deterministically jumps between menu panes (m36e).
- bDebugAllPerks MCM Debug toggle (force-grant all MEO perks for testing).

## v0.47.0 — support gems functional (m36a/m36b, 2026-07-12)
- Support-gem data foundation: Echo/Conduit/Focus records, tiers I–III.
- Linking layer (one support + one normal per dual-slot item), Focus
  (+20/35/50% linked elemental magnitude), support leveling (XP only while
  linked, no birthing), Conduit cross-domain adapter.
- Gems made non-sellable: zero gold value (DLL runtime + ESP source).

## v0.44.3–v0.46.0 — audit + balance guards (m35 series, 2026-07-12)
- v0.46.0: 2-of-a-kind stacking cap — at most the two highest-level copies
  of an effect count across worn gear.
- v0.45.0: per-list gem magnitudes derived by the installer, not hardcoded.
- v0.44.3: audit fixes.

## v0.44.x — the MEO perk tree installed (m34 series, 2026-07-12)
- Elemental affinity perks (Pyrestone/Froststone/Stormstone, +25%) and
  Facet Insight (+25% skill/attribute armor gems); DLL applies the math.
- Perk-tree layout: affinities as a choice-fan off the Attunement spine;
  obsolete vanilla enchanting perks dropped (installer).
- Converted enemy loot no longer flags as theft.

## v0.43.x — temper without Arcane Blacksmith (m33, 2026-07-12)
- Socketed gear tempers without the Arcane Blacksmith perk; made an MCM
  toggle (`bTemperNoPerk`, default on).
- v0.43.2: fixed pouch/inventory uid drift ("gem no longer in inventory").

## v0.39.0–v0.42.0 — portability + pouch resilience + controller (m32 series)
- Absolute-mode riders: the calibration pin list emptied (v0.39.0/v0.39.1).
- The pouch survives: co-save tracks the hidden container, recovery runs
  every load, lost gems come back; same-form duplicate dispel; calibration
  can't fail silently (v0.40.x).
- Real controller support in the gem pouch (v0.41.0).
- Gem records follow the pouch hop; installer reads `Skyrim.ccc` — Creation
  Club coverage (v0.42.0).

## v0.36.0–v0.38.1 — pouch storage + rank ladders + vanilla installs (m27–m30)
- Hidden pouch storage for loose gems, live gem naming, gemstone models
  (v0.36.0); ranked-kin effect matching for conversion (v0.36.1).
- Gem levels climb the list's own enchantment rank ladder (v0.37.0/1).
- The rank ladder explains itself in game (v0.38.0).
- Installer works on vanilla / non-MO2 setups (v0.38.1).

## v0.30.0–v0.35.3 — the installer era: calibration + conversion (m22–m26)
- Per-list rider calibration: installer derives recipes from the load
  order's own records; DLL consumes `meo_calibration.json` (v0.30.0).
- **Loot conversion** (v0.31.0, corrected from the earlier "strip" plan):
  covered enchanted generics convert at spawn/acquire into the plain base
  with the family gem socketed and ACTIVE — actor spawns, containers,
  vendors, pickups, player sweep; worn NPC conversions re-equip live.
- Runtime menu skins — all four, MCM dropdown (v0.33.0); resizable menu.
- Station pane redesign, enchanting skill XP from souls, partial-recipe
  conversions (v0.34.0); convert-miss observability (v0.34.1).
- Player-enchant conversion + in-place world-ref conversion (v0.35.0).
- Soul-feed Gem XP cut ~80% to {1, 2.5, 5, 12, 40} — the shipped balance
  (v0.35.1); no-family enchants dump their effects (v0.35.2).

## v0.28.0–v0.29.0 — installer perk tree + recipe riders (m20–m21)
- C# Mutagen installer replaces the load order's enchanting perk tree with
  MEO's perks, interactive keep/drop per foreign perk (v0.28.0).
- Recipe riders: gems inherit the list's incidental second effects (Requiem
  frost = frost+slow, etc.) (v0.29.0).

## v0.26.0–v0.27.4 — station takeover + the gem economy (m18–m19)
- Enchanting tables open the gem menu instead of the vanilla menu
  (`bStationTakeover`, default on) (v0.26.0).
- Enemies spawn wearing themed socketed gear; vendors sell loose gems;
  container re-key — socketed gear keeps its records across container hops
  (v0.27.0–v0.27.3).
- Load-reactivation SOLVED blinklessly: teardown + rebuild passes anchored
  to loading-screen close (v0.27.4).

## v0.20.0–v0.25.1 — multi-socket + on-load reactivation (m12–m17b)
- Enchant charge/cost fix: created enchants forced free (cost 0, max
  charge) — effects and FX apply instantly, no sheathe/redraw (v0.20.0).
- Multi-socket core: up to 2 gems per item, ONE combined instance enchant,
  gloves dual-linked by default (v0.21.0).
- On-load reapply of worn sockets + armor starter set (v0.22.0); the
  reactivation saga settled via real re-equip (v0.23.0/v0.24.0).
- Socket perks: Twinned Fitting (chest) + Master Jeweler (weapon);
  gem-swap theft fix (v0.25.0/v0.25.1).

## v0.15.0–v0.19.0 — loot, MCM, armor gems, stations, perks (m7–m11)
- Tier rarity curve, corpse/world gem spawns, stack-dup fix (v0.15.0).
- Real MCM (MCM Helper, INI-paired, live re-read) (v0.16.0).
- All 36 armor gems live: constant effects, domain filter (v0.17.0).
- Enchanting stations: soul feeding + gem destruction (v0.18.0).
- Perk effects in the DLL (Attunement, Gem Cutter, Soul Feeder,
  Fortify-Enchanting→XP), interim skill auto-grant (v0.19.0).

## v0.11.0–v0.14.0 — the gem menu (m5–m6b)
- Gem Pouch container menu with atomic swap (v0.11.0).
- Native in-process ImGui gem menu (v0.12.0).
- Global -15% balance pass on LINEAR/ABSORB curves (v0.13.0).
- Sun gem + final balance curves (v0.14.0).

## v0.7.0–v0.10.1 — leveling, XP, economy seeds (m3–m4b)
- Playable socket core (v0.7.0); kill XP + leveling via TESDeathEvent
  (v0.7.1); gem picker (v0.7.2); world gems (v0.8.0).
- Unsocket/swap: every gem owns its own Gem XP for life (v0.9.0).
- Boss XP, follower gems, soul feeding, the Mentor Gem (v0.10.0).

## v0.2.0–v0.6.6 — the native pivot (m0–m2i)
- SKSE DLL skeleton + release tooling (v0.2.0); co-save index (v0.3.0).
- The self-describing instance bundle proven in-game: display names,
  real socket effects, persistence (m2 series, v0.4.0–v0.6.6) — including
  the NO-BRACKETS naming rule (some UI mods strip bracketed tags).

## v0.0.1–v0.1.0 — Papyrus prototype (P0/P1a, 2026-07-05)
- P0: Fire-gem socket loop + WornObject persistence validation — passed,
  green-lighting the design; superseded by the native pivot.
- P1a: catalog-driven ESP generator + runtime JSON pipeline.
