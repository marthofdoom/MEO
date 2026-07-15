# MEO Test Guide — the 1.0 release-candidate matrix (v0.49.x)

This is the current in-game test matrix, covering every shipped system.
Every earlier per-milestone guide is superseded by this one. Install the
full release zip (DLL + MEO.esp + MCM/ + Scripts/MEO_MCM.pex +
meo_runtime.json) AND run the installer (`MEO - Patch.esp` +
`meo_calibration.json`) before testing.

## Gem roster
**54 gems total = 51 normal (15 weapon + 36 armor) + 3 support
(Focus/Conduit/Echo).** Normal gems level I→V; two (Soul Trap,
Waterbreathing) are single-level utilities, Muffle has two levels. Support
gems level I–III, only while linked.

## Before anything: verify the build loaded
1. Load a save and check the log: `.../My Games/Skyrim Special
   Edition/SKSE/MEO.log` (path varies by install; under Proton it's inside
   the game's `compatdata` prefix).
2. First lines MUST show `MEO native v0.49.x`. Older version = the DLL
   didn't update; fix that before believing any test below.
3. Expect a `catalog resolved` line, a `[perks]` line, and (if the
   installer ran) calibration/conversion table counts.

**Testing tips:**
- MCM → max out **Gem drop chance** / **World weapon socket chance** /
  **Level II spawn chance** to get gems fast; turn them back down after.
- MCM → Debug → **Grant all MEO perks (testing)** (`bDebugAllPerks`) gives
  Attunement V, both dual-socket perks, all affinities, Facet Insight, Gem
  Cutter, Soul Feeder without grinding. Toggle off to revert.

---

## 1. Sockets & the Gem Pouch
- [ ] Cast the **Gem Pouch** power → the gem menu opens anywhere.
- [ ] Socket a weapon gem into a weapon → name updates (no brackets),
      effect fires on hit. Log: `STAMP`.
- [ ] Socket an armor gem into worn armor → constant effect in Active
      Effects. Boots refuse sockets; **circlets accept them** (m36f).
- [ ] Unsocket → THE same gem returns (level + partial XP intact, visible
      in its name/description); item reverts to plain.
- [ ] **One-click swap** (m35e): pick a gem while a socket is filled → the
      old gem returns and the new one lands, atomically.
- [ ] Gloves take TWO gems (linked). Chest/main-hand take a second only
      with Twinned Fitting / Master Jeweler.
- [ ] **Stacking cap** (m35c): wear 3+ gems of the same effect → only the
      two highest contribute; the 3rd is inert (and un-caps if one leaves).
- [ ] Plain-stack discipline: socket into a stack of 3 identical swords →
      exactly one becomes socketed, one gem consumed.
- [ ] Loose gems live in the pouch, not inventory; vendors never list
      gems and gems show 0 value.
- [ ] **Controller**: full menu navigation on pad — sticks, A/B,
      d-pad left/right jumps between panes (m36e).
- [ ] **Skins**: MCM → Gem menu style → all four (Ebony & Brass, Dwemer
      Parchment, Soul Cairn, Quicksilver) apply on next menu open.

## 2. Leveling, XP, birthing
- [ ] Kills grant Gem XP to every socketed gem in worn gear. Log: `[xp]`.
- [ ] Boss/dragon kills grant the boss multiplier.
- [ ] Followers' kills feed THEIR worn gems.
- [ ] Level-up rebuilds the enchant (higher magnitude, new name).
- [ ] Past level V: **Mastered** notification + a fresh level-I copy is
      born (the only replication).
- [ ] Enchanting skill and Fortify Enchanting potions/gear scale XP gain;
      Gem Cutter +50%; Mentor Gem (granted on Soul Cairn arrival) doubles.

## 3. Enchanting stations
- [ ] Activate an enchanting table → the gem menu opens INSTEAD of the
      vanilla menu (bStationTakeover default ON; toggle OFF restores
      vanilla). Exiting frees the player from the bench.
- [ ] **Feed**: with a filled soul gem, Feed Soul Gem consumes the
      smallest and adds Gem XP — petty/lesser/common/greater/grand =
      1 / 2.5 / 5 / 12 / 40 (×2 with Soul Feeder). Log: `[feed]`.
- [ ] Feeding also grants Enchanting SKILL xp.
- [ ] **Destroy**: Destroy Gem removes the gem and yields a soul gem sized
      to 1/10 of its banked XP. Log: `[destroy]`.

## 4. Loot conversion (installer required)
- [ ] Kill enemies that spawn with generic enchanted gear → it arrives as
      the plain item WITH the family gem socketed and active (worn pieces:
      the enemy actually deals/has the gem effect). Log: `[convert]`.
- [ ] Chests/vendors/pickups: enchanted generics convert on arrival; no
      theft flag on converted enemy loot (m34c).
- [ ] Uniques/artifacts do NOT convert and their sockets are sealed.
- [ ] Creation Club enchanted items (if present) convert too (m32g).
- [ ] A small tail (duration-anchored recipes, no-family effects) stays
      enchanted — expected, logged as `[convert-miss]`/deferred.

## 5. Perks (after the installer)
- [ ] The load order's enchanting perk tree shows MEO's perks: Attunement
      1–5 spine, Gem Cutter, Soul Feeder, the affinity choice-fan
      (Pyrestone/Froststone/Stormstone), Facet Insight, Twinned Fitting,
      Master Jeweler. Kept third-party perks (your installer choices)
      still present.
- [ ] Attunement raises gem magnitude on re-stamp (+5%/rank).
- [ ] Affinities: +25% to matching elemental gems (Chaos counts for all
      three). Facet Insight: +25% to skill/attribute armor gems.
- [ ] Twinned Fitting/Master Jeweler unlock the 2nd linked sockets.
- [ ] **Temper toggle**: with bTemperNoPerk ON, socketed gear tempers at
      grindstone/workbench without Arcane Blacksmith; OFF restores the
      vanilla requirement.

## 6. Support gems (m36)
- [ ] Acquisition: below player level 15 they never drop; at 15+ they
      rarely drop from boss/dragon kills. Guaranteed copies: **Focus** in
      Avanchnzel (Boilery boss chest), **Conduit** in the Midden's Atronach
      Forge Offering Box, **Echo** in Ustengrav Depths (boss chest).
- [ ] A support gem alone in an item = item stays plain (inert).
- [ ] **Focus** + elemental gem (Fire/Frost/Shock damage or resist, or
      Chaos) in a dual slot → +20/35/50% magnitude by tier. Log: `[link]`.
- [ ] **Conduit** + an off-domain gem → the item expresses the same-theme
      sibling of its own domain at ×0.5/0.75/1.0; no sibling = inert;
      same-domain gem passes through.
- [ ] **Echo** in a weapon + elemental gem → on-hit effect hits
      hostile/in-combat actors around the target (area grows with tier).
- [ ] **Echo** in armor + a gem → current followers share the effect
      (exactly once each — no stacking; expires/re-applies safely).
- [ ] Support gems gain XP ONLY while linked; they cap at III and never
      birth.

## 7. Persistence & recovery
- [ ] Save → quit to desktop → relaunch → load: all socketed gear intact,
      worn gem effects live within ~12 s of the load (no manual re-equip).
      Log: `[load]` record dump + reapply passes.
- [ ] Drop / store / sell / buy back a socketed item → gems and levels
      follow it (`[rekey]` on container hops).
- [ ] The pouch survives across saves; if it's ever lost, recovery brings
      the gems back on load (m32c/d). Log: pouch status line.

## 8. MCM
- [ ] All settings live-apply on menu close (log: fresh `config:` line).
- [ ] Settings persist across save/reload (they live in the INI).

## If something's wrong
Grab MEO.log and note the version header plus the relevant `[menu]` /
`[loot]` / `[convert]` / `[link]` / `[feed]` / `[destroy]` / `[perks]` /
`[xp]` / `[rekey]` / `[load]` lines around the failure. For crashes, see
DEBUGGING.md "Crash analysis".
