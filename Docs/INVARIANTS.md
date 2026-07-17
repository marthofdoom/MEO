# Invariants — the load-bearing rules (v1.0.6)

Every rule below is an imperative with the failure mode that violating it
produces — each one either bit MEO in the field or was caught by the 1.0.6
review cycle and fixed. Line refs → `native/plugin.cpp` unless noted.
ARCHITECTURE.md maps the subsystems these rules live in; ANTI_PATTERNS.md is
the portable "never again" digest for sibling projects.

## Engine interaction

1. **Call the engine's own flows, never hand-write the state a flow
   produces** (marth's standing doctrine, ENGINE_NOTES header). Created
   enchants via `BGSCreatedObjectManager::Add*Enchantment`, worn ability via
   `Update*Ability`/`ActorEquipManager`, dispels via `ActiveEffect::Dispel`,
   barter refresh via the engine's inventory-update routine. Violation =
   the M2 lesson: ~6 release cycles rediscovering, field by field, what one
   engine call does. **The two approved hand-write exceptions**, both
   deliberate, scoped, reversible:
   - the windowed SNDR `staticAttenuation` mute (:262-360) — form data, saved
     originals restored exactly, single-writer restore;
   - the Echo share-spell effect rewrite (:4975-4985) — ONE persistent ESP
     spell's effect mutated each tick, self-expiring, so follower saves never
     reference a runtime form.
2. **`Update*Ability` early-outs on teardown; the only complete worn-ability
   teardown is a real equip cycle** (`EquipCycleWorn` :1681 — idempotent:
   unequip drops ALL old abilities, equip installs exactly one). The m19f
   "blinkless" strip/restamp minted a new FF form per pass and left the old
   ability lingering → the player saw the effect twice (m35d).
3. **Socketable bases must carry no `formEnchanting`** (`IsSocketable*`
   :1602/:1632; the load diag :5588-5604 prints the smoking gun). A base
   enchant + instance enchant = the engine applies BOTH ("base item +
   renamed item" double effect).
4. **No square brackets in display names** (ENGINE_NOTES §2). UI suites strip
   a leading `[...]` — the rename looks failed.
5. **`PlaceObjectAtMe` refs have no owner — `SetOwner(player)` before any
   pickup** (:2409). Otherwise picking up your own spawned gem near a guard
   is witnessed theft (100g bounty, m17b).

## Iteration & mutation

6. **NEVER mutate a container/`BSSimpleList` while iterating it — snapshot
   targets first, act second.** Three enforced sites, keep them enforced:
   - `AwardKillXP` (:2275-2333): `GrantGemXP` can synchronously
     `AddObjectToContainer` (mastered birth) and equip-cycle; a head-insert
     makes the walker revisit the head = double XP tick, chainable level-ups.
     Each award also **re-finds the `g_sockets` record by key** — a prior
     target's level-up may have rewritten the map.
   - `ReapplyWornSockets` (:5560-5661) and `ReapplyFollowerSockets`
     (:5496-5542, review B2): equip events are synchronous into follower AI
     and third-party sinks (outfit managers are guaranteed in Lorerim-class
     orders) → node use-after-free on the post-load path. Follower targets
     are held by `ActorHandle` and re-resolved at cycle time.
   - `DispelStaleGemEffects` (:5432-5471): collect the dispel list fully
     before calling `Dispel(true)`.
7. **All engine mutation goes through `SKSE::GetTaskInterface()->AddTask`**
   (main thread). Sinks, sleeper threads, and menu actions only queue;
   the render thread only reads mutex-guarded snapshots.
7b. **Any `ExtraDataList*` handed to an ownership-taking inventory API
   (`AddObjectToContainer`'s `a_extraList`) must be a RELINQUISHABLE HEAP list
   from the engine's own ctor (`MakeEngineXList`, `RELOCATION_ID(11437,
   11583)`) — never a pointer interior to another object.** The API LINKS the
   pointer into the container entry (no deep copy, disasm-proven) and the entry
   then owns it. m44 passed `&placeholderRef->extraList` then deleted the ref =
   the container held a freed list = a use-after-free that rode converted loot
   into player inventory and detonated on the next inventory walk (issue #2,
   fixed m47/v1.0.6c). `new RE::ExtraDataList()` won't even link in NG (ctor
   declared, undefined). The living-actor conversion path is exempt only
   because `PickUpObject` CONSUMES its placeholder ref.

## Co-save (schema `'GEMS'` v11)

8. **Every persisted FormID passes through `ResolveFormID` on load** —
   socket bases (:5904) AND the pouch ref (:5834, dynamic FF refs still go
   through the handle map). The co-save stores raw runtime FormIDs
   (mod-index byte included); any load-order change remaps plugin indices.
   Without resolution every key points into the wrong plugin's FormID space:
   worn gems read record-less (dead), banked XP lost, stale ids can collide
   onto unrelated items. Unresolvable → **drop the record, never guess**.
8b. **Container-transfer re-key disambiguates by FAMILY SIGNATURE, never
   guesses** (`RekeyTransferredSockets`, m49). The engine rewrites uids on
   transfer; when >1 stranded records share a base the true source is the one
   whose gem mgef(s) all appear in the ARRIVING instance's MEO-built enchant
   (pointer / `SameEffectSig`). 0 or >1 signature-survivors → SKIP (a strand is
   recoverable on the next transfer; a mis-assignment corrupts the wrong item —
   §1 doctrine). One stranded record poisons every later transfer of its base
   until a match clears it — the ambiguity is a ratchet, not a rare safe-out.
8c. **Never call NG `ExtraDataList::RemoveByType`; strip extras via
   `SafeRemoveAllByType` only** (m50). The NG version null-derefs the moment a
   removal EMPTIES the list (ENGINE_NOTES §8) — and an emptied list is a REAL
   shape: a converted instance can arrive at a strip site as exactly
   `{kEnchantment, kTextDisplayData}` because the engine's uid rewrite + a
   save/load can cost it its `ExtraUniqueID` node entirely (ENGINE_NOTES §1
   TRAP 2, field-proven load-CTD 2026-07-17). Corollary: no code may assume a
   converted/enchanted instance carries a uid node; the kPostLoadGame sweep's
   in-place re-conversion of such an instance is the sanctioned self-heal.
9. **Bound every count and bail on short read** (N2, :5867): a truncated
   record must stop the read, not fabricate keys from garbage.
10. **Clamp deserialized values at the source**: level → [1,5] (:5891) —
    a corrupt level 0 indexes `kXPThresholds[level-1]` out of bounds in
    `GrantGemXP` (which also guards `level < 1` itself, :2190).
11. **Versioned schema, readers forever, fields append-only** (header
    :32-45). And **SKSE does NOT round-trip unread co-save records** — a
    downgraded DLL that saves DESTROYS newer records; warn loudly
    (:5794-5812), never log a comforting "preserved as unread".
12. **Key on (baseFormID, uniqueID, slot); persist the stable string gid,
    never a catalog index** (`MakeKey` :116; `SocketRecord` :102). Engine
    uids are unique only per base form; indexes don't survive catalog edits.
13. **Mint uids only in MEO's reserved range [0x9000, 0xFFFF] and never
    return a (base,uid) with a live record** (`MintUID` :128). A uint16 wrap
    back into engine range (~28k mints — ConvertInventory/NPC stamping burn
    them fast) would silently clobber a real socket via `operator[]`.

## The stacking cap (until the 1.0.7 tally-cap retires it)

14. **The 2-of-a-kind cap applies ONLY to player-worn gear — every worn-stamp
    caller must thread the real owner** (`applyCap` :1293; `StampInstance`
    owner param :1537→:1559; `MaybeStampNPCGear` passes the NPC :4537).
    `WornActiveEffectKeys()` scans the player's inventory only, so an
    ungated cap on NPC gear strips the just-written enchant and orphans its
    record in the co-save permanently (build-B1 — v1.0.6's worst blocker).
14b. **The m42 living-NPC border has ONE scoped exemption** (m48,
    marth-approved 2026-07-17): a VENDOR's unworn personal sellables convert at
    trade time (`ConvertVendorPersonalStock`, dialogue-open + the deferred
    barter task) via the m47 container recipe ONLY — never the worn/
    `PlaceObjectAtMe`/`PickUpObject`/re-equip path that m42 exists to prevent.
    Worn gear is skipped. Every other living-NPC path (`ContainerSink`,
    `CellAttachSink`) stays gated — do not widen this.
15. **After any `g_sockets` mutation on a worn item: rebuild → activate →
    redistribute the cap iff the gid's worn count exceeds 2** (`WornGidCount`
    :1177; level-up path :2210-2220, socket/unsocket/destroy mirror it).
    Wrong order = stale abilities or a still-capped copy.
16. **Only items that can legitimately produce an active effect vouch in the
    dispel allowance: worn, or base mid-equip-cycle** (`g_equipCyclingBases`
    :5391). Inventory-wide vouching resurrects save-carried orphans (the
    m24b escalated-skill bug); worn-only without the cycling exception races
    `EquipCycleWorn` and kills a live effect (m26e).

## Load & reactivation

17. **Worn instance enchants are delivery-dead after load until a real equip
    cycle** (ENGINE_NOTES §8 — settled mechanism). Reactivation anchors on
    LoadingMenu-CLOSE, not a blind timer (a timer fired during a long load
    screen is swallowed).
18. **One reapply generation per load; only the matching generation
    consumes** (`g_reapplyGen`/`g_reapplyPending` :5696, MenuSink
    `exchange(0)` :1991, fallback CAS :5719). A single bool let a first
    load's 15s fallback steal a second load's reapply → that load's worn
    gems stayed dead (review S2).

## Threading & locks

19. **Every ImGui-IO touch takes `g_imguiIoMx`** (:3140, xp/hooks-S2): input
    thread pushes `Add*Event`, window thread clears keys, render thread
    drains in `NewFrame` — unserialized that's a data race on ImGui's event
    vector. Hold it only around the IO block, never across the engine
    passthrough (no cross-lock with the menu-snapshot mutex).
20. **Sound-mute discipline** (:262-360): deadline is CAS-max'd
    (`OpenEnchHumMuteWindow` :339 — extend, never shorten, except the
    explicit post-load `SetEnchHumMuteDeadline`); only the render-thread
    tick restores, and `RestoreEnchHumMute` re-checks the deadline UNDER
    `g_magMx` (:325) so a concurrent re-open wins. Set the deadline BEFORE
    applying on the load path (:5979) or a present tick between the two
    sees muted+expired and restores early.

## Config & cross-artifact contracts

21. **An INI/MCM key that changes SEMANTICS (raw value → multiplier) MUST be
    renamed** — MCM Helper persists stored values per key name into MO2's
    overwrite, surviving mod updates. `fGemXpSkillXP` (absolute 0.01) became
    `fGemKillXpMult` (×1.0 default, :1813 + generator MCM_TUNABLES): a stale
    0.01 read as a multiplier silently drops the kill-XP trickle ~100×.
    Keep the DLL parse branch and the generator's MCM key in lockstep.
22. **Skip unparseable INI values, never apply 0.0** (`ApplyIniFile` :1789);
    strip MCM Helper's UTF-8 BOM (:1778).
23. **The 0x800–0x8FF block + gem MISCs from 0x900 are frozen DLL contracts**
    (perks 0x810–0x81C :628; pouch 0x803/0x8FE; Echo 0x809; Mentor 0x8FF;
    `data/gem_forms.frozen.json` never recycles fids). Reordering the
    generator's perk list silently rebinds perks to wrong effects; moving a
    gem fid mis-resolves every socketed gem in existing saves.
    **The Phase 3 reserved pool 0xB00–0xC3F (64 slots × 5 levels — grown from
    32 pre-ship, append-only; anchored by
    `data/pool_forms.frozen.json`) is frozen the same way** — installer
    slot-assignments on users' machines bind detected enchant families to
    these exact fids, so a pool slot may be added but never removed,
    renumbered, or recycled once shipped (`allocate_pool` hard-fails both
    directions: base/level drift AND slot shrink). Curated gem allocation
    must stay below 0xB00 (`allocate_gems` hard-fails at the band edge); the
    pool anchor is a SEPARATE file precisely so the curated max-scan can
    never leapfrog the band.
24. **Generator text must equal DLL math** (Attunement "+5%/rank" DESC ↔
    `0.05f * g_attuneRank`). Failure: tooltips lie — and shipped zips cut
    before the last balance commit DID lie (v1.0.6 re-cut required).
25. **`TESDataHandler::LookupForm<T>` takes CONCRETE record classes only**
    (ENGINE_NOTES §9): abstract intermediates fail `Is(FORMTYPE)` 100% at
    runtime — the v0.31.0 dead conversion table.
26. **The minted-gid string and the pool-slot assignment are both forever.**
    `MintGid` (`installer/MEO.Installer/Commands.cs`) emits
    `x_<alnum-lowercase plugin stem, ≤16>_<FNV-1a hex8 of the full lowercase
    filename>_<primary MGEF fid hex6>` — co-saves store this string, so its
    shape for any given input may NEVER change, and the hash + the mgef echo
    in the assignments file (lowercase-normalized like the gid, so a
    case-only plugin rename never reads as a collision) keep it
    collision-guarded (extension-stripped +
    truncated stems plus 12-bit ESL fid spaces make cross-plugin collisions
    realistic; a silent collision merges two families onto one slot).
    Catalog gids must never start with `x_` — the gid namespace is shared.
    `meo_pool_assignments.json` is APPEND-ONLY per-user state: a gid keeps
    its slot forever, vanished gids stay burned. **Exists-but-unreadable must
    hard-fail, never default to empty** — an empty read silently reassigns
    every slot by current-evidence order = loose pool gems change species.
27. **Minted families are conversion-only, pool-guarded, and fail dormant.**
    Every spawn-pool builder (corpse/lootable, themed NPC, `ConduitSibling`)
    must skip `.minted`. A minted family may bind only MISC fids ≥
    `kPoolFloor` (0xB00) — the floor keeps a corrupt calibration from
    capturing curated forms. The `"minted"` parse is PER ENTRY: one bad
    family costs only itself, never the section (deliberately not the
    families block's all-or-nothing posture). A minted family whose MGEF
    **or any pool form** fails to resolve registers DISABLED — never skipped
    from `g_gemByGid`, never live — so co-save records keyed by its gid go
    dormant; a record must never be erased before its gem hand-back is known
    to resolve (the `GiveGemInstance` null-item silent no-op is a destruction
    path, not a fallback — `MenuUnsocket` and the swap-evict both check the
    level's item form BEFORE erasing).
    Minted riders live in the CALIBRATION, never the gid contract: `MintGid`
    derives from the primary MGEF alone, so a re-patch may change a family's
    rider set freely — rebuilds pick it up, co-saves are untouched. Rider cap
    is 4 (`RtRider`); the DLL drops extras loudly at parse. A pool slot is
    assigned only after the pre-slot gate passes — and that gate must test
    what the REAL conversion loop tests (per-item: honored class, StripBase,
    domain), not effect shape alone. Zero-mag companions that carry KEYWORDS
    are never waived as description — they ride at ratio 0 so the keyword's
    presence survives conversion.
