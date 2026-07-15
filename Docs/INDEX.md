# MEO Documentation Index — start here

marth's Enchanting Overhaul: socketable, leveling enchantment
gems for Skyrim SE. This project is designed so any capable model or person
can continue it from these docs alone. Load documents on demand, not all at
once.

CURRENT STATE (v0.49.4, 1.0 release candidate): the native DLL, the ESP
generator, and the C# (Mutagen) installer are ALL SHIPPED and validated
in-game. Full feature set: 54 gems (51 normal I–V + 3 support I–III),
multi-socket with linked pairs, the hidden Gem Pouch + native ImGui menu,
loot conversion (all paths + Creation Club) with per-load-order installer
calibration, the MEO perk tree installed into the load order's enchanting
tree, support gems (Focus/Conduit/Echo) with hand-placed + boss-loot
acquisition, station takeover/feeding/destruction, 4 menu skins, controller
support. Remaining for 1.0.0: docs/portability audit + release engineering.
`CHANGELOG.md` (repo root) has the version history.

## Read order for a fresh session

1. **DESIGN.md** (always) — what MEO is: the player loop, the data-driven gem
   catalog, levels/XP/birthing, sockets and the Gem Pouch, the "item is the
   database" model, support gems, and the perk layer. This is the spec,
   reconciled to the shipped native build. **BALANCE.md** holds the tuning
   math (XP ladder, tier curves, drop rates) behind it.
2. **ENGINE_NOTES.md** (before ANY native instance/extra-data work) — every
   engine mechanism proven in-game: the self-describing instance bundle,
   rename traps (force + temper sync + NO BRACKETS + 0x1C indirection), the
   created-enchantment recipe, validated event sinks, native message boxes,
   co-save discipline, ops traps, known issues (§8). Synced once to
   `../Linux-Native-Tools/instance-data-and-events.md` (this repo is
   canonical). The living truth for behavior is `native/plugin.cpp`.
3. **ARCHITECTURE.md** (before touching `native/plugin.cpp`) — the subsystem
   map with line refs: identity/`g_sockets`, the enchant-build pipeline and
   the owner-gated stacking cap, pouch/transfers, the kill sink + XP economy,
   loot/conversion, perks, the three hooks + thread/lock map, the windowed
   sound mute, co-save + load reactivation, config, and the generator/
   installer contracts (FormID bands, calibration).
4. **INVARIANTS.md** (before ANY code change) — the load-bearing rules, each
   an imperative + the failure mode that violating it produced: engine flows
   + the two approved hand-write exceptions, snapshot-before-mutate,
   ResolveFormID/bounded co-save reads, owner-gated cap, uid minting, the
   ImGui/sound lock protocols, the INI rename rule, frozen FormID contracts.
5. **ANTI_PATTERNS.md** (for siblings MRO/MAO, and before repeating history)
   — the portable "never again" catalog distilled from the 1.0.6 four-review
   cycle: one rule + one line on how each bit MEO.
6. **MANUAL_MOD_CREATION_GUIDE.md** (when creating/altering RECORDS) — the
   binary format reference: record/group/subrecord encoding, verified recipes
   (GLOB, QUST, VMAD, MGEF, SPEL, PERK, FLST, LVLI, GMST, SEQ), the PO3 event
   rules, Papyrus-on-Linux compilation, FOMOD rules.
7. **DEBUGGING.md** (when something misbehaves) — symptom → cause → fix for
   every failure class hit across both this project and its sibling MRO, plus
   the universal diff-against-vanilla method and native crash-log guidance.
8. **TEST_GUIDE.md** (before/after each release) — the 1.0 in-game test
   matrix: sockets, leveling, conversion, perks, support gems, the pouch,
   stations, MCM.
9. **P0_TESTING.md** (HISTORICAL) — the pre-native Papyrus prototype gate.
   Passed 2026-07-05; kept for the record.

## Forward plans (designed, not yet built)

- **ROADMAP-1.0.7-tally-cap.md** — replaces the build-time 2-of-a-kind stacking
  cap with a runtime tally over the active-effect list (edge-triggered reconcile
  on cell-change + optional guarded equip sink). Closes the equip-swap over/
  under-apply gap and retires the `applyCap`/owner-gating machinery that caused
  v1.0.6's `StampInstance` NPC-strip blocker. Ships after Phase 3 deck-testing.

## Archived (Docs/archive/ — superseded, kept for history)

- **NATIVE_REWRITE_PLAN.md** — the plan the native layer was built from
  (co-save socket/XP index, per-instance identity, kill hooks). The DLL
  shipped; useful only for rationale.
- **TESTING.md** — MRO-specific console test procedures; never ported.
- **DYNAMIC_OR_DROP.md** — the portability rule ("nothing baked from THIS
  machine's load order ships"), with stale MRO examples. The rule itself
  WON and is now enforced structurally: the installer derives calibration,
  riders, and conversions from the user's own load order at install time.

## Sibling project

MRO (`../Requiem-modification/`) shares this exact toolchain and was built
first. Its `docs/` — especially `NATIVE_REWRITE_PLAN.md` (CI-built DLL,
hook doctrine) and `docs/DEBUGGING.md` — remain the proven cross-reference
for MEO's native layer. Reuse, don't re-derive.

## Tools (use instead of re-deriving)

- `tools/parse_ench.py` — ENCH/MGEF/gear catalog parser (produces
  `data/ench_catalog.json`, `data/gear_families.json`).
- `tools/gen_catalog_header.py` — generates `native/GemCatalog.h` from the
  catalog JSONs.
- `tools/compile.sh` — Papyrus compilation via Proton wine.
- `tools/release_native.sh` — the ONLY way a build reaches the game: full
  standalone zip (DLL + ESP + MCM + scripts + runtime JSON), tag, immutable
  `releases/vX.Y.Z/`.
- Installer record-inspector commands (`installer/` README + memory notes)
  for dumping any record vs its vanilla twin.

## The one principle

When touching the binary format: **copy a working vanilla record, never trust
documentation** — including this documentation. If a record misbehaves, dump
it and its vanilla twin and diff subrecords. Every multi-day bug across both
projects (TES4 flags, FOMOD wrapper, SPIT type, PERK layout, FormID prefix,
SEQ, MGEF fortify archetype) ended the moment we compared bytes against
something that worked.
