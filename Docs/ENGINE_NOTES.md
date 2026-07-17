# Engine Notes — proven native mechanisms (1.6.1170 / AE)

Everything below was **validated in-game** during MEO's M2–M4 arc (v0.5.0 →
v0.7.2) on a heavy load order (Lorerim). Reference implementation:
`native/plugin.cpp`. Cross-project copy lives in
`../Linux-Native-Tools/instance-data-and-events.md`.

**Standing doctrine (marth):** replicate engine features by CALLING the
engine's own flow, the way SKSE's Papyrus natives do — never hand-write the
state a flow produces. Before mutating instance/extra data, read the SKSE64
source (github.com/ianpatt/skse64) for the equivalent Papyrus native and
replicate *those* engine calls. Distrust CommonLibSSE-NG's C++
reimplementations for engine-visible state (they cut corners; see SetName
below). M2 burned ~6 release cycles rediscovering, field by field, what one
engine flow does in one call.

---

## 1. The self-describing item instance (zero hooks)

A per-instance "socket" is three extra-data entries on the instance's
`ExtraDataList` (inventory entry's list, or the world reference's
`extraList`). The engine serializes them in the `.ess` natively — the item IS
the database; it travels through drop/pickup/containers/saves with no script:

| Extra | Role |
|---|---|
| `ExtraUniqueID` (0x9F) | instance identity — **only unique per base form**; any map must key on `(baseFormID, uniqueID)` |
| | **TRAP (validated 2026-07-07, v0.11.0):** the engine REWRITES `uniqueID` when the item transfers between containers (per-container bookkeeping, reassigned from 1). uid-keyed identity is only stable while the item stays in ONE container. Player-inventory round trips (drop/pickup of world refs, equip/unequip) preserve it; `player -> chest -> player` does NOT — MEO's pouch-container menu (v0.11.0) orphaned banked-XP records this way and was scrapped. **SOLVED in v0.27.0-m19 (proven in-game 2026-07-09):** a `TESContainerChangedEvent` sink re-keys the record on every transfer — match the arriving orphan xList (enchanted, record-less, in the new container) with the stranded record (uid in neither container) and move the record to the new uid. **The event's `uniqueID` field names the ARRIVING (new-container) uid** (field-proven: `[rekey] uid 36933 -> 42 (evUid=42)`). Ambiguous multi-instance transfers log + skip. Socketed gear now survives corpse loot, vendors, and storage with living records. |
| `ExtraTextDisplayData` (0x99) | display name (see §2) |
| `ExtraEnchantment` (0x9B) | the *created* enchantment (see §3) |

## 2. Renaming an instance — the four traps

1. **Force, never add-if-absent.** The engine lazily creates a *blank*
   `ExtraTextDisplayData` for any TEMPERED item (health ≠ 1.0) to render
   "Fine <name>" — add-if-absent silently skips all tempered gear. Mirror
   SKSE `SetDisplayName(force=true)`: reuse the record, null
   `displayNameText`/`ownerQuest`, then `SetName`.
2. **temperFactor must equal ExtraHealth.health.** NG's `SetName` reimpl
   hardcodes `temperFactor = 1.0`; on tempered gear the mismatch makes the
   pickup notification (and other UIs) fall back to the base name. Fix by
   the engine's own builder: `xText->GetDisplayName(baseObj, health)` —
   real engine fn, `RELOCATION_ID(12626, 12768)` — reconciles
   suffix/customNameLength/temperFactor.
3. **NO SQUARE BRACKETS in display names.** UI suites (moreHUD / BTPS /
   Simple Activate render the activate rollover via Scaleform `htmlText`)
   strip a leading `[...]` as an icon/categorization tag. `[MEO] Glass
   Dagger` rollover-rendered as "Glass Dagger" — indistinguishable from a
   failed rename; proven with a table-renamed control (`[pookie] X` → "X").
   Phase 3 widens this surface: minted item names are composed from
   *arbitrary modded MGEF names* at runtime — a list that tags effects
   `[Tag] Name` re-trips this trap, so the minted registration strips a
   leading `[...]` group before composing gem item names (installer-side
   MintName strips ALL bracket groups from the calibration name too).
   m46 relabel guards: the m27 follow-the-winning-record relabel is for
   single-effect identity swaps ONLY — a multi-component family (compiled
   riders > 0) keeps its family name when one COMPONENT is renamed (marth's
   deck: a list renamed chaos's fire-component MGEF "Fire Damage" and the
   tri-element gem presented as a second Fire Damage family), and no family
   may relabel INTO a name another curated family owns.
4. **`ExtraDataList::GetDisplayName` resolves through `ExtraReferenceHandle`
   (0x1C)** to the ORIGINAL reference's name data when present. An inventory
   entry that kept the handle from its pickup can read a *stale* name from
   the old world ref. (The enchanting table's output never carries 0x1C —
   it mints a fresh entry.)

## 3. A functional instance enchantment (the SKSE recipe)

Attaching a base `ENCH` form via `ExtraEnchantment` shows the description on
the item card but applies **no effect**. The working flow (from SKSE64
`PapyrusWornObject.cpp`, the code behind `WornObject.CreateEnchantment`):

1. Build a player-style **created** enchantment from MGEF effect items:
   `RE::BGSCreatedObjectManager::GetSingleton()->AddWeaponEnchantment(
   BSTArray<RE::Effect>&)` (SKSE name:
   `PersistentFormManager::CreateOffensiveEnchantment`). Created forms get
   FF-prefix runtime FormIDs and are engine-persisted in the save.
2. **Make it free** — `AddWeaponEnchantment` auto-computes a per-hit CHARGE
   COST from magnitude, which `ExtraEnchantment.charge` (a `uint16`, max
   `0xFFFF`) must cover per swing. Force cost 0 so it never drains:
   `ench->data.costOverride = 0;`
   `ench->data.flags.set(RE::EnchantmentItem::EnchantmentFlag::kCostOverride);`
   Then attach with max charge:
   `xList->Add(new RE::ExtraEnchantment(ench, 0xFFFF, false))`.
3. If the item is currently equipped:
   `actor->UpdateWeaponAbility(baseForm, xList, leftHand)`
   (`RELOCATION_ID(37803, 38752)`) — activates the magic caster. Skipping
   it = description with no effect.

`RE::Effect` fields: `effectItem{magnitude, area, duration}`, `baseEffect`
(the `EffectSetting*`), `cost` (the effect's cost — NOT the enchant's; the
enchant recomputes its own, hence step 2). **Charge state gates BOTH damage
and the elemental visual** (v0.20.0-m12): a charge-starved enchant shows its
description but does no damage and needs a sheathe/redraw to reconcile the
glow. A free enchant (cost 0, full charge) is permanently active, so damage
and FX apply immediately on stamp — see §8.

## 4. Event sinks — hook-free triggers (all validated)

| Event | Shape | Notes |
|---|---|---|
| `RE::TESEquipEvent` | `{actor, baseObject, uniqueID, equipped}` | plain unenchanted items report `uniqueID=0` |
| `RE::TESSpellCastEvent` | `{NiPointer<TESObjectREFR> object; FormID spell}` | fires for lesser powers too → menu-less "cast a power" UX |
| `RE::TESDeathEvent` | `{actorDying, actorKiller, bool dead}` | fires twice; act on `dead == true` |
| `RE::TESCellAttachDetachEvent` | `{NiPointer<TESObjectREFR> reference; bool attached}` | **fires per reference**, not per cell — ideal for stamping world loot at load |
| `SKSE::CrosshairRefEvent` | `{NiPointer<TESObjectREFR> crosshairRef}` | via `SKSE::GetCrosshairRefEventSource()`; read-only ground-ref diagnostics |

Register TES events on `RE::ScriptEventSourceHolder`; defer all mutation to
`SKSE::GetTaskInterface()->AddTask` (main-thread).

## 5. Native message box with buttons (no UI framework)

> Retired in MEO v0.11.0 in favor of a container menu, which was itself
> scrapped the same day (uid rewrite trap, §1). Pattern kept: it is proven,
> safe, and the right tool for small confirmations.

Verified against Exit-9B/ForgetSpell + D7ry/valhallaCombat:

```cpp
auto* factory = RE::MessageDataFactoryManager::GetSingleton()
    ->GetCreator<RE::MessageBoxData>(RE::InterfaceStrings::GetSingleton()->messageBoxData);
auto* box = factory->Create();
box->unk4C = 4;  box->unk38 = 10;          // required magic (copied from working mods)
box->bodyText = "Socket which gem?";
box->buttonText.push_back("Fire I");        // up to ~10 buttons; paginate past 8
box->callback = RE::BSTSmartPointer<RE::IMessageBoxCallback>{ new MyCallback };
box->QueueMessage();
// In MyCallback::Run(Message m): button index = static_cast<int32_t>(m) - 4
```

The `- 4` offset on the callback message is the trap everyone hits.

## 6. Co-save discipline (SKSE serialization)

- Versioned records; keep readers for every shipped version forever, write
  only the newest. Never reorder/remove fields — extend via version bump.
- Store stable STRING identities (catalog gids / effect signatures), never
  enumeration indexes — they must survive load-order and catalog changes.
- Key per-instance records on `(baseFormID, uniqueID)` (§1).
- **Every stored FormID passes `ResolveFormID` on load** (v1.0.6 review B1) —
  the co-save stores raw runtime ids, mod-index byte included; any load-order
  change remaps plugin indices and un-resolved rehydration mis-keys every
  record. Applies to dynamic FF-form ids too (they go through the handle
  map). Unresolvable → drop + log, never guess.
- **Bound counts, bail on short reads, clamp fields at ingestion** — a
  truncated record must stop the parse, not fabricate keys; a corrupt level 0
  indexed `thresholds[-1]` two subsystems away.
- **SKSE does NOT round-trip unread co-save records** (v1.0.6 review S1):
  newer-version records loaded by an older DLL are DESTROYED by its next
  save. Warn loudly (log + one-shot message box); the old "preserved as
  unread" log line was a comforting falsehood.
- One-time grants (starter kits): only consume the persisted flag when the
  grant actually succeeded — a missing ESP must retry next load, not burn it.
- **Equip/unequip dispatch is SYNCHRONOUS into every registered sink** —
  never cycle equipment while iterating a live `entryList`/`extraLists`
  (`BSSimpleList`); snapshot targets first, re-find records by key, hold
  actors by handle (INVARIANTS.md has the full rule set; cross-repo copy
  §17-§20 of the Linux-Native-Tools notes).

## 7. Ops traps (test protocol)

- **Stale DLL voids tests.** The MEO.log version header is the mandatory
  first check before believing any in-game result (bitten twice).
- **MO2 has two checkboxes**: left-pane mod AND right-pane plugin.
  `profiles/<P>/plugins.txt` needs `*MEO.esp` — starless = not loaded, and
  the only symptom is our own "form not found" log line.
- Zero-code control items beat code diagnostics: an enchanting-table-renamed
  weapon is an engine-canonical record to diff against (`DumpXList` in
  plugin.cpp prints the full extra-data anatomy of any instance).
- Proton log path:
  `compatdata/<appid>/pfx/drive_c/users/steamuser/Documents/My Games/
  Skyrim.INI/SKSE/MEO.log` (Lorerim appid 3375297225; truncates per launch).

## 8. Known issues — external or by design (validated 2026-07-07, v0.8.0)

- **~~Enchant-visual mods lag until sheathe/redraw.~~ FIXED in v0.20.0-m12.**
  We long treated the FX lag as an external FX-mod timing quirk. It was
  actually ours: the created enchant carried a magnitude-scaled per-hit charge
  cost against a fixed 500 charge, so it lived in a "chargeable / depletes with
  use" state the engine only reconciles on equip — hence the redraw was needed
  to refresh the elemental glow (and at high magnitude the cost outran the
  charge, so it never fired at all — the m11 bug report). Forcing the enchant
  free (cost 0 + `0xFFFF` charge, §3 step 2) makes it permanently active, so
  socket/unsocket/level changes — damage AND visual — apply immediately in all
  cases, drawn or not, no sheathe/redraw. marth confirmed in-game 2026-07-08.
  (Confidence: the cost/charge change is definitely the cause — it's the only
  weapon-side change; the exact charge→shader gating path is inferred, not
  instrumented. Revisit here if it ever regresses.)
- **Worn instance-`ExtraEnchantment` sockets go INERT on load until a real
  re-equip. FIXED in v0.24.0-m16.** The enchant data survives the save
  PERFECTLY — the m15 `[load-diag]` (reads the as-loaded enchant before we
  touch it) proved `kCostOverride=true`, `costOverride=0`, `charge=0xFFFF` all
  round-trip. So this was NEVER an enchant-data problem (the m14/m15
  costOverride-loss hypothesis was WRONG — disproved by the diag). What dies is
  the actor's **equipped-weapon enchant delivery cache**, which the engine
  rebuilds on load from the item's **base-form** enchantment (`formEnchanting`)
  — which a socketed item does NOT have; ours is an instance `ExtraEnchantment`
  the load path doesn't re-derive from. `Actor::UpdateWeaponAbility`/
  `UpdateArmorAbility` do NOT rebuild that cache here: m15 called them 4× over
  8s and the weapon still never fired. The ONLY thing that reactivates it is
  the engine's own equip flow — a real unequip → re-equip (which is exactly the
  manual fix marth found). m16 fix: `ReapplyWornSockets` collects worn socketed
  items (refresh created enchant for MCM magnitude), then
  `RE::ActorEquipManager::UnequipObject` → `EquipObject` on each (weapons to
  their `BGSDefaultObjectManager` hand slot kLeft/kRightHandEquip; armor
  slotless). REFINED (v0.27.4-m19f, marth's insight): the real rule is that
  **the ability refresh only takes when the enchant extra CHANGES.**
  In-session socketing reactivates without any equip cycle (it changes the
  extra); post-load, a rebuild dedupes to the SAME FF form, so
  UpdateWeaponAbility no-ops against the save's stale ability bookkeeping —
  and re-equip only worked because unequip is a forced teardown. The
  m19f blinkless recipe (strip `ExtraEnchantment` + Update*Ability teardown,
  then NEXT task rebuild + Update*Ability) was **RETIRED in m35d**: it leaned on
  Update*Ability to REPLACE the worn ability, but that call early-outs on
  teardown and each restamp minted a NEW FF form, so old abilities lingered and
  the same effect stacked ("base item + renamed item", marth). The shipped model
  (v0.47.1-m35d) reactivates worn sockets with the engine's own complete
  teardown — a real **equip cycle** (`EquipCycleWorn`, the same call in-session
  socketing uses) — which is IDEMPOTENT: unequip drops ALL old abilities, equip
  installs exactly one from the current extra, so repeated passes can't
  accumulate. Still anchored to Loading-Menu close (+5s/+12s) with a long
  fallback for menu-less loads; the one-frame blink hides behind the post-load fade. GOTCHA: `d3d11.h` pulls `wingdi.h` which `#define`s
  `GetObject`→`GetObjectW`, hijacking `BGSDefaultObjectManager::GetObject<T>()`
  — `#undef GetObject` after the D3D includes.
  SETTLED (2026-07-08, m17b AV probe): the kLeft/RightItemCharge AV-gate
  theory is REFUTED — the probe showed the item-charge AV persists in the
  save and was already 65535 BEFORE the re-equip (identical AFTER). So every
  piece of persistent data survives load intact: the created enchant, its
  kCostOverride/costOverride, ExtraEnchantment.charge, AND the charge AVs —
  yet delivery was still dead (m14/m15) until a real equip. Final statement
  of the mechanism: **the engine's post-load equip reconstruction does not
  register instance-ExtraEnchantment delivery at all; only a live
  ActorEquipManager equip does.** In-session socketing works without an equip
  cycle because the worn item already went through a live equip that session.
  The "pre-m12 builds fired on load" recollection is best explained by
  routine manual re-equipping masking the deadness — the one candidate that
  could have explained genuine on-load firing (persisted charge AVs) is now
  eliminated. The m16 deferred unequip→re-equip is therefore not a workaround
  but the engine's own required path.
- **Weapons in containers, on racks/displays, or in NPC inventories are NOT
  born socketed.** `TESCellAttachDetachEvent` fires per *world reference*;
  container contents and carried items are inventory entries, not refs, and
  rack/display items ride linked/persistent refs that don't come through the
  attach path the same way. By design for M3c — only loose world refs roll.
  Container/NPC-inventory pre-socketing is future work (container-changed
  sink or stamp-on-transfer).
- **Some floor items vanish after save → main menu → load.** Suspected
  engine save-culling issue (wskeever pinpointed it), external to MEO.
  Socketed items *in inventory* persist correctly across save/load.

## 9. In-process ImGui overlay menu (validated 2026-07-08, v0.12.0 in-game)

The M6 gem menu: ImGui drawn inside the game's own DX11 present, no
external overlay. Verified against D7ry/wheeler source, then validated
in-game under Lorerim's ENB/Community Shaders stack on 1.6.1170.

**Three trampoline hooks** (`SKSE::AllocTrampoline(64)` at plugin load,
installed in `SKSEPluginLoad` — before the renderer exists):

| Hook | RelocationID | Offset | Purpose |
|---|---|---|---|
| D3DInit | `(75595, 77226)` | `VariantOffset(0x9, 0x275, 0x0)` | grab device/context/swapchain, init ImGui backends |
| DXGIPresent | `(75461, 77246)` | `Offset(0x9)` | draw (render thread!) |
| InputDispatch | `(67315, 68617)` | `Offset(0x7B)` | translate + swallow input while open |

- Renderer access (NG 3.7): `RE::BSGraphics::Renderer::GetSingleton()->
  data.{forwarder, context, renderWindows[0].swapChain}`; hwnd from
  `swapChain->GetDesc()` → `sd.OutputWindow`; WndProc swapped via
  `SetWindowLongPtrA` (clear ImGui keys on `WM_KILLFOCUS`).
- InputDispatch signature: `void(RE::BSTEventSource<RE::InputEvent*>*,
  RE::InputEvent**)`. While the menu is open, walk the event list into
  ImGui IO and set `*a_events = nullptr` — the game sees nothing, so no
  vanilla menu bleed-through and Esc/Tab are ours to interpret.
- **Threading rule**: the present hook draws from the render thread. All
  engine mutation goes through `SKSE::GetTaskInterface()->AddTask` (main
  thread); the draw reads a mutex-guarded snapshot rebuilt by each task.
- **`io.DisplaySize` lies under Proton/upscalers**: the Win32 backend
  reads `GetClientRect`, which can disagree with the backbuffer — v0.12.0
  drew its "centered" window visibly off-center (validated symptom).
  Fix: cache `sd.BufferDesc.{Width,Height}` at init and overwrite
  `io.DisplaySize` every frame between `ImGui_ImplWin32_NewFrame()` and
  `ImGui::NewFrame()` (shipped v0.13.0, validation pending).
- **Build traps**: NG 3.7 declares but does not export
  `RE::ExtraDataList::ExtraDataList()` → LNK2019; never `new` an xList —
  mint instances via the engine (`RemoveItem(kDropping)` → stamp uid on
  the dropped ref → `PickUpObject`). `d3d11.h` pulls `windows.h`, which
  NG never includes: guard with `WIN32_LEAN_AND_MEAN` + `NOMINMAX` or its
  min/max macros break `std::max`/`std::clamp` everywhere.
- **`TESDataHandler::LookupForm<T>` silently rejects abstract intermediates
  (found in-field 2026-07-09, shipped as v0.31.0's dead conversion table —
  "0 live, 10146 skipped").** The template gates on `form->Is(T::FORMTYPE)`,
  and intermediate classes like `RE::TESBoundObject` define no `FORMTYPE` of
  their own — they inherit `TESForm`'s `FormType::None`, so the check fails
  for EVERY real form and the lookup returns nullptr. Compiles clean; fails
  100% at runtime. The trap is asymmetric: `TESForm::LookupByID<T>` routes
  through `form->As<T>()`, which handles intermediates fine — only the
  `TESDataHandler::LookupForm<T>`/`LookupFormRaw<T>` pair uses the broken
  `Is(FORMTYPE)` gate. Rule: give the data-handler templates CONCRETE record
  classes only (`TESObjectWEAP`, `BGSPerk`, ...); for an intermediate, look
  up as plain `TESForm` (non-template overload) and cast with
  `->As<RE::TESBoundObject>()` — that cast IS field-proven
  (MenuUnsocket/DestroyGem run it constantly).
  Fixed v0.31.1-m23b, with the skip counter split by reason (item/base/gem)
  so a resolution failure can never again read as one silent number.
- **Spawn-and-pickup is THEFT in owned locations (found in-game 2026-07-08,
  fixed v0.25.1-m17b).** A `PlaceObjectAtMe` ref has NO owner, so ownership
  falls back to the cell/location owner; `PickUpObject` on it near guards =
  witnessed theft → instant bounty (marth: 100g per gem swap in town). Any
  spawn→stamp→pickup recipe MUST first
  `ref->extraList.SetOwner(player->GetActorBase())`. Applied to the
  gem-return path (GiveGemInstance) and the plain-stack drop/pickup mint.
- **Vendor barter stock lives in a non-actor CONTAINER, not the vendor actor
  (found by Fable audit 2026-07-13, fixed v1.0.1-m37).** `StockVendorGems`
  resolves the sell inventory as `vendorData.merchantContainer` — a chest REFR,
  not the merchant Actor. `ConvertInventory` used to early-return on any
  non-actor holder (`if (!actor) return 0`), so that sweep was a SILENT no-op
  since m23: vendor stock (where a low-level player meets generic enchanted
  armor like "of Major Wielding") NEVER converted, and logged nothing because
  every miss line was gated behind the actor/player branch. Fix: `ConvertInventory`
  sweeps container holders too. Non-actor holders can't `PickUpObject`
  (actor-only). CORRECT flow (m47, issue #2):
  `holder->AddObjectToContainer(base, xl, 1, nullptr)` where `xl` is a HEAP
  `ExtraDataList*` from the engine's OWN ctor (`RELOCATION_ID(11437, 11583)`,
  size 0x20, MemoryManager-allocated) — NO placeholder world ref.
  **CONTRACT, proven at disassembly on 1.6.1170 (InventoryChanges worker id
  16053, node write at `+0x484`): `AddObjectToContainer` LINKS the passed
  `a_extraList` pointer into the container entry's `extraLists` list — it does
  NOT deep-copy. The entry TAKES OWNERSHIP of that pointer.**
  ⚠ **m44 was WRONG and shipped a use-after-free (v1.0.1–v1.0.6b, fixed m47 /
  v1.0.6c).** m44 stamped `&tempRef->extraList` (an interior pointer at
  TESObjectREFR+0x70), linked it via `AddObjectToContainer`, then
  `tempRef->Disable(); SetDelete(true)` — freeing a list the container entry
  still owned. The dangling list moved BY POINTER into whoever looted/bought
  the item (inventory extra-data transfers by pointer, §1), was freed at the
  source cell's detach, and the next inventory walk (Requiem worn-keyword
  re-eval, savegame serialize, item destroy) read a torn `0x2` pointer → AV at
  a `SkyrimSE.exe` offset with MEO nowhere on the stack (issue #2; also a
  tbbmalloc `Block::freeOwnObject` free-path variant). The m44 review passed on
  the visible SYMPTOM (no under-counter dup) without checking the API ownership
  contract — the "copies / entry owns independently / reap the placeholder"
  claim is FALSE; delete it from your mental model. The live-actor branch
  (`PickUpObject` CONSUMES its placeholder ref, so its extraList travels
  correctly) is fine and unchanged. NOT the same
  as Papyrus `AddItem(ObjectReference)`. Mirror this to ../Linux-Native-Tools instance-data notes.
  Guard the actor-only steps (`worn` equip, player ownership/theft-guard,
  player convert-miss diag) behind `if (actor…)`. Log container sweeps with a
  count so this failure class self-diagnoses.
- **Vendor stock RESTOCKS from leveled lists as the BARTER menu opens — after
  DialogueMenu-open — and that re-roll fires no `TESContainerChangedEvent`
  (found by Fable + deck-log proof 2026-07-13, fixed v1.0.2-m38).** The m37
  container sweep was hung on DialogueMenu-open (per the m19e lesson: mutating the
  chest while the barter list builds broke Belethor). But the engine re-generates
  the vendor's LVLI stock when the player picks the *trade* line / the BarterMenu
  opens — one beat AFTER the dialogue sweep — so freshly re-rolled enchanted
  generics ("of Burning", "Minor Archery") arrive unconverted on top of the
  already-converted stock. Deck log 2026-07-13 nailed it: `[convert] container …
  7 item(s) converted` at dialogue time, yet barter still displayed re-rolled
  enchanted names, and none of those names were in the converted-7 list → they
  were created by the post-sweep restock. LVLI regeneration emits NO container
  event, so the ContainerSink fallback never catches it either. **Fix:** add a
  BarterMenu-open sweep, deferred TWO frames (`AddTask` inside `AddTask`) so the
  barter list is fully built before we touch the chest (mid-build mutation is the
  m19e breakage), then rebuild the open list via the game's own inventory-update
  routine — the same one a buy/sell fires. NOTE: our pinned CommonLibSSE-NG is
  **3.7.0** (vcpkg colorglass registry, commit c4ab853d), which PREDATES
  `RE::SendUIMessage::SendInventoryUpdateMessage` (that header 404s at this
  commit) — bind the relocation it uses in newer CommonLib directly:
  `REL::Relocation<void(RE::TESObjectREFR*, const RE::TESBoundObject*)>{
  RELOCATION_ID(51911, 52849) }` and call `(target, nullptr)`. Keep the
  dialogue-open sweep
  for vendors not due to restock. Do NOT pre-empt the reset by hand (calling
  reset early + writing `vendorData.lastDayReset` is hand-writing engine
  bookkeeping and risks desyncing vendor gold) — let the engine restock, then
  convert, then ask it to refresh.
- **Barter stock has THREE sources, not two (m48, 2026-07-17 — field bug).**
  Beyond (1) the merchant chest and (2) the LVLI re-roll into that chest at
  barter-open, a vendor also sells (3) their OWN actor personal inventory —
  LoreRim citizens carry `LootCitizenPocketsCommon`, which chains down to
  enchanted-jewelry ladders (`…RingList → LItemEnchRingHealth → 'Ring of Major
  Health' 0FCEFF`). The barter menu lists a vendor's unworn personal sellables
  alongside the chest. m42 (living-NPC border) removed the CellAttach/
  ObjectLoaded `ConvertInventory(actor)` sweep that used to convert this, so
  vendor-personal stock stopped converting — it displayed enchanted but still
  converted on purchase (player path). Fix `ConvertVendorPersonalStock`
  (marth-approved scoped m42 exemption): unworn sellables only, via the m47
  container recipe (no PlaceObjectAtMe/PickUpObject/equip), at dialogue-open +
  the deferred barter task. **Verified chest map: Belethor = `0009CAF9` via
  `ServicesWhiterunBelethorsGoods [09CAF5]` merchantContainer — the old
  `0009CAFE` note was WRONG (that's a different Whiterun vendor, 0001A67C).**
- **The uid-rekey ambiguity is a permanent RATCHET (m49).** `RekeyTransferred
  Sockets` skips when >1 stranded records share a base (can't tell which maps
  to the arriving instance). But ONE stranded record poisons EVERY subsequent
  transfer of that base forever — it never clears itself. Field case: an
  ancient fortifystamina orphan on Iron Boots + a bought resistshock Iron Boots
  → ambiguous → skip → the bought gem invisible in the menu (the `ours` check
  fails on the rewritten uid). Fix: disambiguate by FAMILY SIGNATURE — the
  arriving orphan's MEO-built enchant names its gem families, so the true
  source is the stranded record whose gem mgef(s) all appear in that enchant
  (pointer or `SameEffectSig`); 0 or >1 survivors → keep skipping
  (mis-assignment stays impossible). Self-heals a poisoned save on the next
  drop+re-pickup of the item.
- `io.IniFilename = nullptr` — never write imgui.ini into the game dir.
- vcpkg: `imgui` features `dx11-binding`, `win32-binding`; link
  `imgui::imgui d3d11`.

## 10. Enchanting-station detection + takeover (m10/m18)

- **Detect**: `MenuOpenCloseEvent` for `RE::CraftingMenu::MENU_NAME` opening,
  then (deferred to a task) `player->GetOccupiedFurniture()` → base
  `TESFurniture` → `workBenchData.benchType == BenchType::kEnchanting` (3).
  The DG staff enchanter is a different bench type — untouched by design.
- **Takeover (m18, v0.26.0)**: MEO replaces enchanting, so the vanilla menu
  is dismissed at open via the engine's own queue:
  `RE::UIMessageQueue::GetSingleton()->AddMessage(CraftingMenu::MENU_NAME,
  UI_MESSAGE_TYPE::kHide, nullptr)`, and the gem menu owns the station.
  CAVEAT: hiding it fires the menu's own CLOSE event — any close-handler
  coupling ("crafting closed → close our menu") must be gated off in takeover
  mode or it will kill the menu you just opened. INI/MCM `bStationTakeover`
  (default on) restores overlay mode when off. Validation pending (m18).

## 11. Perk-tree replacement — record formats (m20, offline-verified)

The whole skill-menu tree for a skill is ONE record: the skill's AVIF
(`AVEnchanting` 0x45D). Whoever wins that override owns the tree — every
mod's added nodes (Ordinator, Special Feats, Wand Keywords) vanish with a
single last-loading override. Verified against the full LoreRim order with
Mutagen on Linux (installer/).

- **AVIF perk-tree node**: INAM index, PNAM perk, FNAM uint32 (root=300,
  every real node=1), XNAM/YNAM grid (Y grows outward from root; root has
  junk coords like 398,40 — preserve it verbatim), HNAM/VNAM float nudges,
  SNAM = the AVIF itself, CNAM connection indices = the UI's prerequisite
  lines. Node indices may be sparse; connections reference indices, not
  positions. Root is index 0 with a null perk — keep it, rewire its CNAMs.
- **Ranked perks**: NNAM (next perk FormID) chains rank records; numRanks
  in DATA stays 1 on every record (Requiem convention, e.g. Enchanter's
  Insight 1→2). The skill UI derives "rank 1/5" from the chain.
- **Skill gates**: CTDA on the PERK record. GetBaseActorValue is function
  index 277, param1 = AV index (Enchanting=23), operator byte 0x60 =
  GreaterThanOrEqual (3<<5). 32-byte CTDA:
  `<B3xfH2xiiiii` = op, compValue, func, param1, param2, runOn, ref, -1.
- **DLL coexistence**: the DLL detects the generated patch via
  `TESDataHandler::LookupModByName("MEO - Patch.esp")` and stands down its
  skill-based auto-grant so tree perks cost perk points.

- **Perk-domain classification (m21 installer)**: perk entry-point effects
  carry their own condition lists declaring what they apply to. Craft perks
  = ModEnchantmentPower / soul-gem entry points. Runtime enchantment
  empowerment = entries gated on `GetIsObjectType FormType=Enchantment`
  (Special Feats' Arcane Artificery — would double-scale gem output on top
  of Attunement). Staff/wand utility = GetEquippedItemType / FormType=Spell
  / keyword gates. Fully derivable — no perk names needed. Mutagen quirk:
  IGetIsObjectTypeConditionDataGetter hides the param; reflect
  `GetProperty("FormType")`.

## 12. MGEF-level conditions travel with the effect (m22, record-verified)

A magic effect's own condition list (MGEF `Conditions`, distinct from the
per-effect CTDA on an ENCH/SPEL effect entry) is evaluated wherever the
effect is applied — including inside a runtime-created enchantment built by
`BGSCreatedObjectManager::AddWeaponEnchantment`. Consequences MEO relies on:

- **Riders self-gate.** Requiem's `REQ_Effect_EnchShockDamageFFContactDwemerBonus`
  (mgefConds=1: dwarven automatons) and its vs-undead Turn-Undead damage twin
  (mgefConds=2) fire only when their MGEF conditions pass, even when a gem
  carries them as riders with no conditions on the effect entry. Copying such
  an effect copies its gating for free.
- **Effect-entry conditions do NOT travel** — they live on the ENCH record's
  effect item, and MEO builds its effect items bare. A companion that is
  gated per-entry in the source recipe (chaos's 50%-proc entries) would fire
  unconditionally as a rider; the installer's calibration derivation skips
  those with a note.
- Read both layers with Mutagen: entry = `effect.Conditions`,
  MGEF = `mgef.Conditions` (`ench` dump prints `conds=`/`mgefConds=`).

Also record-verified on LoreRim: Requiem reimplements vanilla utility
effects as **Script-archetype** MGEFs at the vanilla FormKey
(`REQ_Effect_DestructionGM_Slow_Touch` = 0x0B72A0 FrostSlowFFContact,
arch=Script). Script archetypes work fine as gem riders — the gem
references the winning MGEF record and the engine runs its script; do NOT
classify enchantments as non-replicable by archetype. The real
"casting implement" signals are `ENCH.EnchantType == StaffEnchantment` and
Concentration/Aimed effects.
