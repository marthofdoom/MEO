# Roadmap — v1.0.7: runtime tally-cap (stacking cap v2)

**Status:** DESIGNED, not built. Ships after Phase 3 is deck-tested (marth's
sequencing). v1.0.6 ships with the current build-time cap and the equip-swap
limitation documented (see DESIGN.md §"Stacking cap").

## Why replace the build-time cap

The 2-of-a-kind stacking cap (DESIGN §"Stacking cap", m35c) is currently baked
into each item's built enchant at *rebuild* time (socket / unsocket / level-up /
load) via `WornActiveEffectKeys()` + the `applyCap` skip in
`RebuildInstanceEnchant` (`native/plugin.cpp`). Two structural problems:

1. **It only reconciles on a rebuild.** A plain equip/unequip of an
   already-socketed item fires no rebuild, so the cap is transiently wrong
   (over-apply exploit / under-apply staleness — see DESIGN). Closing that on the
   build path would require an equip-triggered **equip cycle**, which reintroduces
   the exact iterate-while-equip re-entrancy class v1.0.6 spent a whole review
   removing (B2/S1/S3).
2. **The build-time cap needs `applyCap`/owner-gating.** Because the cap scans
   the *player's* worn set (`WornActiveEffectKeys` reads `PlayerCharacter`
   inventory only), every non-player worn stamp must be owner-gated or it strips
   the enchant. That coupling **caused v1.0.6's worst blocker** (the
   `StampInstance` NPC-gear strip + permanent record leak, build-B1). The cap and
   the NPC-stamp path should not be entangled.

## The design

**Enforce the cap on the active-effect list, not on the built item.** The engine
already creates a constant-effect ActiveEffect on equip and removes it on
unequip — so the actor's active-effect list *is* a live, always-current tally of
the worn set. We already walk it: `DispelStaleGemEffects()` fingerprints our
effects (FF-form enchant + `kCostOverride` signature + gem MGEF) and dispels
copies past an allowance via `ae->Dispel(true)` — an engine function, M2-clean,
already proven in the field. This is a re-point of existing machinery, not new
machinery.

### Pieces

1. **Build every item full-strength.** Drop the `applyCap` skip in
   `RebuildInstanceEnchant` and delete `WornActiveEffectKeys()`. NPC/follower/
   player gear all build the same way — which **removes the `applyCap`/owner
   plumbing entirely** (and with it the build-B1 bug class). Side benefit: a
   3rd copy keeps its gem in the item **name + gold premium** (only its active
   effect is suppressed), fixing the "cap-skipped gem vanishes from its name"
   nit and easing the gem-naming UXR thread.

2. **Tally by gid+level, not by (FF-form, MGEF).** The engine gives each *level*
   of a gid a different FF-form (magnitude differs → different created enchant),
   and the current dispel key is `(FF-form, MGEF)` — which would tally each level
   separately. Record a small `FF-form → (gid, level)` map at enchant creation in
   `RebuildInstanceEnchant` (we already have both values there). The reconcile
   groups active effects by **gid**, sorts by **level desc**, keeps the top 2.

3. **Reconcile = suppress + promote (symmetric).** For each capped gid, the
   top-2-by-level worn instances should have live active effects:
   - **Suppress:** any live active effect beyond the top 2 → `ae->Dispel(true)`.
     Cheap, no equip cycle, no re-entrancy.
   - **Promote:** any top-2 instance whose active effect is missing (previously
     dispelled, now un-capped because a higher/equal copy was removed) → its
     active effect must be *re-created*. `Dispel` is one-way for a constant
     effect, so promotion needs a **deferred, guarded re-equip** of that one item
     (or `ApplyWornAbility` if it proves sufficient to re-instantiate from the
     intact `ExtraEnchantment` without a full cycle — investigate first).

### Triggers (edge-triggered, reuse what exists)

The tally is *derived state* — compute it on demand, don't maintain it
continuously. Reconcile only when a capped gid could cross the 2-boundary:

- **Baseline (free): `CellAttachSink`** (already registered). On cell attach,
  reconcile. Coarse, but a *balance* exploit that evaporates at the next area
  transition can't be meaningfully banked — this alone may suffice.
- **Optional (immediate): a guarded `TESEquipEvent` sink.** Only checks whether
  the equipped/unequipped item's gid crossed the cap boundary; if not, does
  nothing. If yes, schedules the deferred reconcile.

### Why the equip-triggered re-equip is safe (it worried v1.0.6, it shouldn't)

A re-equip fired from an equip event is fine **when deferred + guarded**, and we
already carry both guards. The three failure modes and their mitigations:

| Risk | Mitigation (already in the codebase) |
|---|---|
| Event storm / recursion (our cycle fires equip events → handler re-fires) | `g_equipCyclingBases` guard (skip bases mid-cycle) + edge-trigger (post-cycle the gid sits *at* cap → no crossing → no re-fire) |
| Synchronous mid-equip conflict | defer to `SKSE::GetTaskInterface()` (next frame; engine equip settled) — the reapply pattern we already use |
| Cost on a ~1500-plugin load | cheap early-out before any work: player/follower? item has our `ExtraUniqueID` + a `g_sockets` record? capped gid? |

The B2/S1/S3 bugs were a *different* class — mutating `entryList` **while
iterating it**. An equip handler processes one `(actor, base)` and iterates
nothing, so that hazard doesn't apply.

### Design rules (make it oscillation-proof)

- **Reconcile reads fresh live state at task time, never the event payload** — so
  rapid swaps settle to truth idempotently.
- **Edge-trigger + `g_equipCyclingBases` guard** — our own reconcile actions
  can't feed back.
- Reconcile is idempotent: running it twice in a row is a no-op.

## Scope / process

Core-path change (enchant build + the sweep + a bookkeeping map + optional sink)
— gets its own Fable review + deck test, not a graft into a finished release.
Deck-test matrix: over-apply (socket-unworn → equip 3rd copy), under-apply
(unequip a top-2), rapid gear swaps (no oscillation/flicker), follower gear,
save/load across the change, and confirm the `applyCap`/`WornActiveEffectKeys`
deletion didn't regress NPC/follower stamping.

## What this retires

- The equip-swap over/under-apply limitation (DESIGN §"Stacking cap").
- `applyCap` / owner-gating in `RebuildInstanceEnchant` + `StampInstance`
  (the build-B1 bug class).
- The "cap-skipped gem vanishes from its name/premium" nit.
