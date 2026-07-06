# Native Plan — MEO.dll (CommonLibSSE-NG)

Goal: one small SKSE DLL alongside the ESP + Papyrus that owns the two things
Papyrus does badly or not at all — **persistent per-item socket state** and the
**per-hit / per-kill logic** — while Papyrus keeps the Gem Pouch menu and the
MCM. This is a from-scratch plan for MEO's systems; only the toolchain and
safety discipline are inherited from MRO (`../Requiem-modification/native/`),
which is a working CI-built CommonLibSSE-NG DLL. MRO's native systems (DR,
absorb, vendor gold, mastery) share NOTHING with MEO — do not port them.

Method (unchanged doctrine): never invent, copy from working open source.
Every mechanism below names a public reference; we adapt it, we don't
reverse-engineer from scratch. **Copy a working thing, diff against it when it
breaks** — the same rule that made the ESP work.

## Why native for MEO (precise — avoid cargo-culting)

The design says native gives "conformance + no save bloat." Be exact about
*which* problem each part solves, because it changes what actually needs a hook:

- **Papyrus save bloat** comes from script instances and growing Papyrus
  variables/arrays. P0's marker mechanism ALREADY avoids it: socket state lives
  in the item's engine-side enchantment extra data (`ExtraEnchantment`), which
  the engine persists per instance in the `.ess` as native data, not Papyrus
  VM data. So "avoid bloat" is not, by itself, a reason the *persistence* must
  move to the co-save.
- The real native wins are: **(1) conformance/stability** (Address-Library
  hooks, engine-version resilient, the strict modern standard); **(2) apply
  gem effects to ANY inventory item**, not just the worn slot WornObject is
  limited to; **(3) real on-hit / on-kill logic** for XP and support-gem procs;
  **(4) a clean home for global data** (XP-per-type pools, config) in the
  co-save instead of a pile of GlobalVariables.
- Therefore the co-save is strictly needed only for data that can't live on
  the item (global XP pools; any per-instance XP if we choose per-instance).
  Per-item socket identity can stay in the item's own enchantment extra data
  (self-describing, engine-persisted) — decide in M1 whether to also mirror it
  into a co-save unique-ID map, or keep the enchantment as the sole record.

## System inventory (MEO feature -> native mechanism -> reference)

| # | Feature | Native mechanism | Reference (verify before use) |
|---|---|---|---|
| 0 | DLL skeleton + logging | SKSEPlugin_Load, spdlog | Copy MRO `native/` (plugin.cpp, CMake, vcpkg, PCH) + `.github/workflows/native.yml`; upstream [libxse/commonlibsse-ng-template](https://github.com/libxse/commonlibsse-ng-template) |
| 1 | Global XP pools + config in co-save | `SKSE::GetSerializationInterface` Save/Load/Revert callbacks, unique record tags | Serialization example in the NG template; po3 mods use this pattern |
| 2 | Identify an item *instance* | `ExtraDataList` on the `InventoryEntryData`; engine `ExtraUniqueID`, or a custom `BSExtraData` tag | Fenix's Equipment Durability System (per-instance item health + co-save) — VERIFY it's open/MIT; else po3 PapyrusExtender item-extradata code (MIT) |
| 3 | Apply / rebuild / remove a gem enchantment on any inventory item | Construct a runtime `EnchantmentItem` (or reuse the engine's user-enchant path) and set it on the instance's `ExtraEnchantment`; refresh on equip | po3 PapyrusExtender `Object`/enchantment functions; engine's own player-enchant flow (RE'd in CommonLibSSE headers) |
| 4 | Award gem XP per kill | `RE::TESDeath` event sink; credit kills to the player, distribute AP to socketed gems | Standard CommonLibSSE event-sink pattern (documented everywhere) |
| 5 | Support-gem on-hit procs (All AoE, Counter, ...) | `RE::TESHitEvent` sink, or a weapon-hit call hook if the event is too coarse | [D7ry/valhallaCombat](https://github.com/D7ry/valhallaCombat) hit-site thunk (maintained post-1.6.1170); po3 `RegisterForHit` semantics |
| 6 | Config / Papyrus bridge | INI (`SKSE/Plugins/MEO.ini`) + GlobalVariables the DLL reads; MCM writes them | MRO M4 pattern (this repo's sibling) |

Not native (stays Papyrus): the Gem Pouch menu/UX and the MCM. The DLL exposes
native Papyrus functions (e.g. `MEO_Native.OpenSocketApply(...)`) the menu calls.

## Relationship to P0 and the marker mechanism

P0 validates, in pure Papyrus, that a runtime enchantment (real effect + marker)
persists across save/load, re-equip, and rebuild. If P0 passes, the native
layer keeps that exact persistence substrate (item enchantment extra data) and
simply *manages it in C++* with more reach (any item, not just worn) and
correct hit/XP logic. **Marker effects may be retired** once the DLL can read
socket identity another way (co-save unique-ID map, or decoding the real
effects directly) — this is an M1/M2 decision, not a prerequisite. If P0
*fails* (persistence unreliable), the co-save unique-ID index becomes
mandatory, and that failure is what justifies fronting the native work earlier.

## Toolchain (decided by constraints — same as MRO)

- No Linux cross-build: CommonLibSSE-NG needs the MSVC ABI + Microsoft linker.
- **Primary: GitHub Actions `windows-latest`.** Reuse MRO's `native.yml`
  verbatim (rename artifact MRO.dll -> MEO.dll); it already carries the vcpkg
  binary-cache setup that keeps runs ~2-3 min. Pin the vcpkg registry baseline
  or a stale `fmt` breaks against modern MSVC.
- Fallback: msvc-wine locally (heavier; only if CI iteration is too slow).
- Remote already exists: `github.com/marthofdoom/MEO` (private). CI needs it.

## Milestones — each user-tested in game before the next

- **M0 skeleton**: MRO's `native/` + `native.yml` copied and renamed; CI green;
  DLL loads, logs its version and the game version to `MEO.log`. Zero hooks.
  Proves the whole pipeline before any risk.
- **M1 co-save index**: serialize the REAL per-instance map (instance ID ->
  {gem signature, level, XP}) through Save/Load/Revert; prove it survives
  save/load and a New Game reverts it. Per the per-instance decision this is the
  load-bearing persistence proof (not a throwaway). Stress-test the instance-ID
  mechanism against stacking/merging. M1 and M2 ship together as the first
  genuinely working native socket (apply + unique persistence), per Marth's
  "working version on the next run".
- **M2 native socket apply**: from C++, apply / rebuild / remove a gem
  enchantment on an inventory item *instance* (including non-worn); repoint the
  Gem Pouch menu to call the DLL instead of `WornObject`. Removes P0's
  worn-only constraint. One item type (Fire) first, mirroring P0.
- **M3 XP + procs**: `TESDeath` sink awards gem XP per kill; `TESHitEvent`
  drives the first support-gem proc (All AoE). Flag-gated, one at a time.
- **M4 cleanup**: retire markers if M1/M2 made them redundant; Papyrus reduced
  to Gem Pouch UI + MCM; MCM toggles write INI/globals the DLL reads.

## Safety rules (non-negotiable — carried from MRO)

- Address Library IDs only — never raw offsets. Version-gate to the runtime we
  target (1.6.1170) and no-op with a log line on mismatch.
- **Validate the Address Library `.bin`** against crash-log ground truth before
  trusting any address; the disk EXE is Steam-DRM encrypted, so verify hook
  sites against `/proc/<pid>/mem` of the running game (MRO's
  `tools/verify_hook_site_live.py`). Instruction-cave / fixed-offset patches
  are banned — they broke on the 1.6.1130+ recompile. Prefer vtable/event
  hooks; use call-site thunks only with a self-verifying install (bail on
  opcode mismatch).
- **GlobalVariable values are save-persisted**: a value the DLL writes at
  `kDataLoaded` is overwritten when a save loads. Re-assert DLL-owned globals
  on `kPostLoadGame` and `kNewGame` (MRO lost a day to exactly this).
- Every hook individually toggleable via `MEO.ini`; ship defaults matching what
  has been play-tested.
- One hook per release. A CTD bisects to exactly one change.
- Papyrus fallback stays in the ESP until its native replacement has survived
  real play; features swap via config, not deletion.
- Crash logs checked after every session (`Docs/DEBUGGING.md`).

## Decisions (settled 2026-07-06, Marth)

1. **Per-instance XP — DECIDED.** Each socketed gem is a unique instance with
   its own level/XP in the co-save; NOT per-type pools ("the design falls apart
   when every gem isn't tracked uniquely"). This makes the co-save unique-ID map
   the core of M1 (not a throwaway structure) and supersedes DESIGN §3's pooled
   model. M1 is now the real per-instance persistence proof.
2. **Socket-identity record — DECIDED (co-save).** The co-save instance entry
   holds the full socket state {gem signature, level, XP}. Markers are retired
   as the identity record (they were the Papyrus-era self-describing tag); keep
   a marker only if we later want the gem TYPE readable without the co-save.
3. **Gem identity = effect SIGNATURE, not enumeration order — DECIDED.**
   Enchant detection becomes a runtime `TESDataHandler` scan of the merged load
   order (mod-added enchants included, correct override order) — the true
   dynamic-or-drop. But per-instance references must survive load-order changes,
   so a gem's stable identity is its effect signature (archetype + actor-value +
   delivery), NOT a load-order index. Split: repo holds the curated
   signature->balance table (curves/tier/xp_mult, portable); the DLL matches
   present enchants to it by signature at runtime, co-save stores the signature.
   Unmatched mod enchants get a default classification or drop.

## Still open

4. **Item-instance ID mechanism**: reuse engine `ExtraUniqueID` vs our own
   `BSExtraData` tag. Prototype `ExtraUniqueID` first in M1 and stress-test
   against stacking/merging (where per-instance schemes usually break); fall back
   to a custom tag if it doesn't survive.
5. Target runtime 1.6.1170 only, or a multi-runtime NG build?
