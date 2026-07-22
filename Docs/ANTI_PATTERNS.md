# Anti-patterns — never repeat these (MEO → MRO/MAO and every sibling)

The portable digest of every class of mistake MEO made or nearly shipped.
Each entry: the rule, then one line on how it bit MEO. Full mechanics live
in INVARIANTS.md / ARCHITECTURE.md / ENGINE_NOTES.md; this file exists so a
sibling project can audit itself against the list in one sitting.

## Engine state

- **Never hand-write state an engine flow produces — call the flow.**
  M2 burned ~6 release cycles rediscovering, field by field, what one
  `SetDisplayName`-equivalent engine call does.
- **Never "refresh" worn abilities by strip/restamp — use the engine's own
  equip cycle.** The blinkless restamp minted a new FF form per pass while
  `Update*Ability` early-outed on teardown: old abilities lingered, effects
  doubled (m35d).
- **Never give an item two enchant sources.** A base `formEnchanting` plus an
  instance `ExtraEnchantment` applies both — "the effect shows as base item
  AND renamed item".
- **Never spawn-and-pickup without `SetOwner`.** Ownerless `PlaceObjectAtMe`
  refs fall back to cell ownership — swapping your own gem in town was
  witnessed theft, 100g per swap (m17b).
- **Never stuff sentinel values into engine pricing fields.** Flat
  `charge=0xFFFF` "free enchant" ballooned weapon prices to 20k+ gold; route
  value through the field the engine isn't spending (m38c).
- **Never trust a convenience reimplementation for engine-visible state.**
  CommonLibSSE-NG's `SetName` hardcodes `temperFactor=1.0` — renames on
  tempered gear silently fell back to base names; NG's
  `TESDataHandler::LookupForm<T>` rejects abstract intermediates — a 100%
  dead conversion table that compiled clean (v0.31.0).
- **Never gate a feature on `LookupByEditorID` without checking the form type
  retains editorIDs at runtime.** The map holds ~15 form types; everything
  else nullptrs — compiled clean, and read as "another mod overrode us".
  Worse: po3 Tweaks caches all editorIDs, so the flagship test order masks
  the failure — a deck pass proves nothing here (m51 F-T1).
- **A vendored-header inline is OUR code at crash time.** A CommonLib bug
  compiles into MEO.dll and faults at an *MEO* offset with no library frame
  on the stack — it reads as "our bug" in every crash log. When a crash RVA
  decodes to a runtime-kind dispatch (a byte at Module+0x118 selecting
  between two offsets) followed by a member **LOAD** (`mov`, 48 8b) where an
  **address-of** (`lea`, 48 8d) belongs, suspect a `RelocateMember<T*>`-vs-`T`
  misuse in the pinned library rather than your own logic. A register full
  of `0x01` bytes is a `bool[]` read as a pointer.

## Iteration & concurrency

- **Never mutate a container/`BSSimpleList` you are iterating — snapshot
  first, act second.** The mastered-gem birth head-inserted into the very
  entryList the kill-XP walker was on: double XP ticks, chainable level-ups
  (build-S3); the follower reapply cycled equips mid-iteration one screen
  below a comment explaining why the player path doesn't (B2).
- **Never do in one code path what a sibling path's own comment forbids.**
  Same B2: the rule was written down and still regrew 300 lines later —
  audit new call sites against existing doctrine, not just the compiler.
- **A single bool is not a cross-instance token — use a generation counter.**
  Two loads inside one 15s fallback window and the second load's reapply was
  stolen; its worn gems stayed dead (S2).
- **Shared UI-toolkit state needs one lock across ALL touching threads.**
  ImGui's IO queue was pushed (input thread), cleared (window thread), and
  drained (render thread) concurrently — a real data race hiding behind
  "it's just events" (xp/hooks-S2).
- **Single-writer for reversible global tweaks, deadline re-checked under the
  lock.** The sound-mute restore raced a concurrent window re-open until the
  restore re-verified the deadline inside the mutex.
- **`press-signal || release-signal` is two events, not one robust event.**
  Gem/slot/soul action rows fired on `IsItemClicked` (press) OR the `Selectable`
  return (release); when the action task finished between those two frames — the
  common case, since the task pump beats the next present — the release echoed a
  second, fully VALID action against the rebuilt snapshot. Only reproduced
  cleanly on stacked rows whose index-keyed ImGui ID survived the rebuild, so for
  a year it masqueraded as misclicks; phase-3 auto-minting made same-type 0-XP
  gem stacks common and beta1 surfaced it as a one-click double-socket (and a
  double soul-feed). Fix: single-shot `IsItemActivated()`. An epoch/idempotence
  check can't help — the echo is stale in intent, not in data.

## Persistence

- **Never store raw runtime FormIDs across sessions — resolve on load, drop
  what doesn't resolve.** One plugin add/remove and every socket record
  pointed into the wrong plugin's FormID space: "the mod ate my save" on any
  Nexus install (B1).
- **Never persist enumeration indexes — persist stable string identities.**
  Catalog reorder would scramble every save's gems.
- **A monotonic uint16 counter is not an ID allocator.** After ~28k mints the
  uid wrapped into engine-assigned range and `operator[]` would silently
  clobber a live record (S3).
- **Never read serialized data unchecked — bound counts, bail on short read,
  clamp fields at ingestion.** A truncated record fabricated garbage keys; a
  level-0 record indexed `thresholds[-1]` (N2, xp/hooks-S4).
- **Never log a comforting falsehood about data safety.** "Records preserved
  as unread" — SKSE destroys unread co-save records on the next save; the log
  itself would have walked a user into bricking their gems (S1).
- **Key per-instance data on (base, uid), never uid alone — and expect the
  engine to REWRITE uids on container transfer.** The pouch-container menu
  orphaned every banked-XP record in one day (v0.11.0); the rekey sink is
  the only cure.
- **Never assume a library mutator is total over its input shapes — read the
  impl before feeding it a boundary case.** NG's `ExtraDataList::RemoveByType`
  null-derefs when the removal empties the list; it sat compiled into every
  MEO build and detonated only when the first `{ench, text}`-only xlist (a
  bought item whose uid node didn't survive save/load) reached a strip site —
  a deterministic every-load CTD on that save (m50, MEO.dll+0x7C4EB). The
  crash was in library code, but the SHIP decision that mattered was ours:
  strip sites must own their empty-list behavior (`SafeRemoveAllByType`).
- **Two sequential ID allocators must never share one freeze anchor.** A
  `max+1` scan that can see a reserved band's fids leapfrogs the band on the
  next regen; MEO keeps `pool_forms.frozen.json` separate from
  `gem_forms.frozen.json` and hard-fails the curated allocator at the band
  edge instead. And freeze guards must trip BOTH directions — shrinking a
  reserved pool passes a base/drift check silently while dropping shipped
  forms out of users' saves.
- **Per-user append-only identity state must fail LOUD when unreadable,
  never fall back to empty.** An empty fallback re-runs first-time
  assignment and silently re-keys everything downstream (MEO pool slots →
  saved gems change species). And a raw parse exception that names the file
  invites the user to delete it — same reset. Catch, explain, refuse.
- **Never hand an ownership-taking engine API a pointer you also own and
  destroy.** m44 stamped `&placeholderRef->extraList`, passed it to
  `AddObjectToContainer` (which LINKS, not copies — the entry now owns it),
  then deleted the placeholder → the container held a freed list → a
  use-after-free that rode converted loot into inventory and crashed at an
  engine offset with MEO nowhere on the stack (issue #2). Give the API a heap
  object built by the engine's own ctor that it is meant to keep. Two owners
  of one allocation, one of them freeing it, is the whole bug class.
- **Review the API's ownership CONTRACT, not just the visible symptom.** The
  m44 review signed off because the observable bug (a duplicate under the
  counter) was gone and no leak showed — an inference about copy-vs-link that
  was never checked against the engine. The contract was only knowable by
  disassembly, and the wrong inference shipped a UAF to every user. When a fix
  hands memory across an API boundary, prove who owns it, don't infer it from
  behavior.
- **A verification fallback must clean up what the failed primary path
  already committed.** m51 F-A1's pickup-refusal re-mint initially left the
  placeholder's freshly-minted socket record orphaned in the co-save — the
  fix for one leak class almost shipped another (the m49 stranded-record
  ratchet).

## Player-relative logic

- **Never apply a player-relative filter to non-player state.** The stacking
  cap's player-inventory scan, reached with a nullptr owner from the NPC
  stamp path, stripped every NPC gem enchant and leaked its record into the
  co-save permanently — v1.0.6's worst blocker (build-B1). Thread the owner
  through every layer that gates on it.
- **Vouch for runtime allowances only from state that can produce the
  effect.** Counting unworn backpack spares kept save-carried orphan effects
  alive (m24b); first-wins dedup killed the cap's legitimate 2nd copy because
  the engine coalesces identical created enchants into ONE form (m38e).

## Config & release engineering

- **Never change an INI/MCM key's meaning under the same name — rename it.**
  MCM Helper persists old values into MO2 overwrite across updates; an
  absolute 0.01 reread as a multiplier silently cut an XP stream ~100×
  (fGemXpSkillXP → fGemKillXpMult — the stale 0.010000 was found LIVE in the
  deployed LoreRim MCM settings, which is what made it a blocker).
- **Never let a failed parse silently become 0.0.** `strtof` on a malformed
  MCM value zeroed whatever it fed (boss kills awarding nothing); warn and
  keep the default instead.
- **Never cut release artifacts before the version's last commit.** The
  v1.0.6 zip was built at 09:58, the balance commits landed that evening:
  shipped tooltips claimed 8–40% where the DLL did 5–25%.
- **Generated text and code math are one contract — derive or cross-check.**
  Same incident: generator DESC strings vs `0.05f * g_attuneRank`.
- **Don't ship VMADs for scripts you don't ship.** Orphaned script
  references spammed Papyrus errors every load in every 1.0.x zip.
- **Any linked-record walk needs a cycle guard.** Mutually-pointing NNAM
  perk chains hung the installer inside an innocuous `.Any()`.
- **Never index by another record's FormKey link without a guard.** One
  dangling ENCH (missing master) crashed the entire Synthesis run.
- **Setup/interactive code outside the top-level try = invisible death.**
  The Wine double-click console vanished with no message — the exact m32e
  failure the code's own comment promised "never again".
- **Mixed-type schema fields are landmines.** `"curve": "Level 1 only"` (a
  string among float arrays) survived only by accident of consumer ordering.
- **Don't build on the host's load order when it silently omits plugins.**
  Synthesis's order drops Creation Club — ~24% of conversions and whole gem
  families vanished until the patcher built its own from `Skyrim.ccc`.
- **Success-only logging makes a live path indistinguishable from a dead one.**
  MEO's barter sweep logged only when it converted something, so "ran and found
  nothing" and "never ran" looked identical — a real vendor bug hunt was blind
  for two visits. Guarded/periodic sweeps must log their zero-case too. Related:
  `[convert-miss]` diagnostics were player-gated, so container/vendor misses
  were structurally silent — a miss you can't see reads as "never examined."
- **Presence is not integrity — never gate on a plugin's NAME being loaded;
  resolve one of its known records.** A Synthesis GROUP named "MEO" outputs
  `MEO.esp`, overwriting/shadowing the real plugin; the name check passed
  while the impostor held none of MEO's records, so the run reported success
  and the DLL then couldn't find the Gem Pouch spell ("mod stopped working",
  "spells disappeared from MEO.esp"). Resolve a frozen record (perk 0x810)
  and refuse if it's absent. Also refuse if OUR patch output would itself be
  named `MEO.esp`.
- **Don't set properties on a shared output you don't own.** `IsSmallMaster`
  was set on `state.PatchMod` — which in a multi-patcher Synthesis group is
  the GROUP's shared output; ESL-flagging that can make another patcher's
  plugin non-compliant. Gate it to our own standalone output name.
- **Mutagen decodes embedded plugin strings as Windows-1252 by default —
  UTF-8/CJK plugins garble.** A Japanese Unslaad rendered mojibake in the
  Synthesis log (cosmetic there), but the same strings would bake into
  phase-3 minted gem NAMES. Pass `NonLocalizedEncodingOverride`
  (`MutagenEncoding._utf8_1252`) so genuine UTF-8 decodes and legacy Western
  falls back; mojibake in a log is an ENCODING signal, never a structural
  failure (System.Text.Json keeps output valid JSON regardless).
- **Tagging by KEYWORD what a later patch can override.** The
  uncovered-strip tag is a calibration DATA ROW (fid + target), not a
  keyword on the record: a keyword dies silently to any later override (the
  m51b "presence is not integrity" class), while a calibration row
  re-resolves per session and misses LOUD (the `[strip]` resolve counters).
  Ship marks on foreign records beside the DLL, never inside the contested
  record.

## Process

- **UI-visible bugs deserve a zero-code control.** The "[MEO] tag stripped"
  mystery fell to a table-renamed control item, not to more diagnostics.
- **A stale binary voids every in-game test.** Check the log's version
  header before believing any result (bitten twice).
- **Fix stale landmark comments in save-critical code as if they were bugs.**
  "schema v3" (was v11) and "blinkless refresh" (retired) were actively
  steering decisions.
