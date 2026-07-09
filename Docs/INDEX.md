# MEO Documentation Index — start here

Marth's Enchanting Overhaul: socketable, leveling enchantment
gems for Skyrim SE. This project is designed so any capable model or person
can continue it from these docs alone. Load documents on demand, not all at
once.

## Read order for a fresh session

1. **DESIGN.md** (always) — what MEO is: the player loop, the data-driven gem
   catalog, levels/XP/birthing, sockets and the Gem Pouch, the "item is the
   database" model, and the perk layer. This is the spec. **BALANCE.md**
   holds the tuning math (XP ladder, tier curves, drop rates) behind it.
   CURRENT STATE: the native DLL is SHIPPED and validated in-game through
   v0.25.x — 51 gem families (weapon + armor), multi-socket with perk-gated
   second sockets, station soul-feeding, perk effects, MCM, on-load
   reactivation. Next milestone: the install-time C# (Mutagen) perk-tree
   installer.
2. **MANUAL_MOD_CREATION_GUIDE.md** (when creating/altering RECORDS) — the
   binary format reference: record/group/subrecord encoding, verified recipes
   (GLOB, QUST, VMAD, MGEF, SPEL, PERK, FLST, LVLI, GMST, SEQ), the PO3 event
   rules, Papyrus-on-Linux compilation, FOMOD rules.
3. **DEBUGGING.md** (when something misbehaves) — symptom → cause → fix for
   every failure class hit across both this project and its sibling MRO, plus
   the universal diff-against-vanilla method.
4. **TEST_GUIDE.md** (before/after each release) — the current in-game test
   matrix for the native systems (menu, sockets, XP, stations, perks, MCM).
   P0_TESTING.md is HISTORICAL (the pre-native prototype gate — passed).
5. **TESTING.md** (before claiming anything works) — console procedures.
   (NOTE: still MRO-specific; port to MEO systems as they land.)
6. **DYNAMIC_OR_DROP.md** (before shipping) — the portability rule: anything
   whose behavior is baked from THIS machine's load order at generation time
   must become runtime-dynamic or be dropped before 1.0. MEO generates its gem
   catalog from the load order's ENCH records, so this rule bites here.
   (NOTE: this file still carries stale MRO examples — vendor gold — pending a
   pass to replace them with MEO's own catalog-generation cases.)
7. **NATIVE_REWRITE_PLAN.md** (HISTORICAL — the DLL is shipped) — the plan the
   native layer was built from: co-save socket/XP index, per-item-instance
   identity, kill hooks; toolchain from MRO. Useful for rationale; the living
   truth is `native/plugin.cpp` + ENGINE_NOTES.md.
8. **ENGINE_NOTES.md** (before ANY native instance/extra-data work) — every
   engine mechanism the M2–M4 arc proved in-game: the self-describing instance
   bundle, rename traps (force + temper sync + NO BRACKETS + 0x1C indirection),
   the created-enchantment recipe, validated event sinks, native message boxes,
   co-save discipline, ops traps. Synced once to
   `../Linux-Native-Tools/instance-data-and-events.md` (this repo is canonical).

## Sibling project

MRO (`../Requiem-modification/`) shares this exact toolchain and was built
first. Its `docs/` — especially `NATIVE_REWRITE_PLAN.md` (CI-built DLL,
hook doctrine) and `docs/DEBUGGING.md` — remain the proven cross-reference
for MEO's native layer. Reuse, don't re-derive.

## Tools (use instead of re-deriving)

- `tools/parse_ench.py` — ENCH/MGEF/gear catalog parser (produces
  `data/ench_catalog.json`, `data/gear_families.json`).
- Bring over MRO's `tools/dump_record.py` (inspect any record vs its vanilla
  twin — THE diagnostic) and `tools/audit_esp.py` (wiring + FormID audit)
  when record generation starts.

## The one principle

When touching the binary format: **copy a working vanilla record, never trust
documentation** — including this documentation. If a record misbehaves, dump
it and its vanilla twin and diff subrecords. Every multi-day bug across both
projects (TES4 flags, FOMOD wrapper, SPIT type, PERK layout, FormID prefix,
SEQ, MGEF fortify archetype) ended the moment we compared bytes against
something that worked.
