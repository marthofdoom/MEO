# Changelog — marth's Enchanting Overhaul

Newest first. Every version that reached the game shipped as a complete
standalone zip in `releases/vX.Y.Z/` (tag = release). Grouped by milestone
arc; point fixes are folded into their feature entry unless load-bearing.

## v1.0.7-beta3 — SE 1.5.97 crash fix (2026-07-21)

Critical point fix over beta2 from the second beta report. Same phase-3 feature
set. **If you play on Skyrim SE 1.5.97, update — beta2 crashes for you.**

- **Fixes a crash on Skyrim SE 1.5.97 whenever a socketed WEAPON is equipped**,
  including a few seconds after loading any save with one worn. The cause is a bug
  in the library MEO is built against: it reads an engine table by dereferencing a
  value it should have taken the address of. On 1.5.97 that lands exactly on a
  block of `true` bytes and crashes 100% of the time; on AE (1.6.x) the same faulty
  read happens to land on valid memory, which is why this went unnoticed — it has
  been present in **every MEO version ever released**, v1.0.6d included. Socketed
  *armor* was never affected. MEO now reads that table directly.
  - AE players get a small bonus fix: in the rare case that faulty read returned
    nothing, a left-hand weapon could re-equip to the wrong hand. That's gone too.
  - The same fix ships in stable **v1.0.6e** for players not on the beta.

## v1.0.7-beta2 — gem-menu double-fire fix (2026-07-20)

Point fix over beta1 from the first beta report. Same phase-3 feature set.

- **Socketing no longer double-fires.** Clicking a gem, slot, or soul row in the
  Gem Pouch menu could register as TWO actions from a single click — most
  visible when you socketed a stack of two identical 0-XP gems and BOTH went in
  at once, or a single-socket item socketed-then-swapped in one click. The menu
  now fires exactly once per click (press or controller/keyboard activate),
  never the trailing release. No gems were duplicated or lost by this — it was a
  doubled action, not inventory corruption — but it also **fixes a soul gem being
  spent twice on one feed** (that one was real resource loss). Latent since the
  menu shipped; auto-minting made same-type 0-XP gem stacks common enough to
  surface it.

## v1.0.7-beta1 — Phase 3: auto-minting + the uncovered-loot toggle (m45–m52, 2026-07-20)

**PUBLIC BETA (Nexus preview file).** The first 1.0.7 build. It adds automatic
gem coverage for enchantment mods your load order adds — MEO no longer only knows
its own ~54 curated families, it now MINTS new gem families from the enchanted
loot in *your* list at patch time. This is the big feature and the reason it's a
beta: the minting and the new strip toggle have been proven in the patcher and at
build time, but NOT yet validated across a full in-game playthrough on an
enchant-heavy list. That's what this beta is for. Save-safe over v1.0.6x (co-save
schema unchanged, v11); the pool grew append-only so existing socketed gems never
move.

### New: automatic family minting (phase 3)
- **MEO mints gem families for enchantments it doesn't ship a curated gem for**
  (m45, S1–S3). The Synthesis patcher scans your load order's enchanted generic
  loot, clusters it by effect, and assigns each viable family a slot in a reserved
  pool of pre-made gem forms; the DLL registers them at runtime from the
  calibration. On a Summermyst + Thaumaturgy list that is ~70+ new families
  (Balefire, Soul Harvest, Spell Absorption, Stormbringer, Sun Damage, the
  Weakness-to-X lines, and so on) on top of the curated set.
- **Multi-effect minting (BETA-within-the-beta)** (e3838b6). When an enchantment
  is one concept built from several magic effects (a primary plus real
  sub-effects), MEO carries the sub-effects on the minted gem as riders — closest
  to parity with the original item. A Synthesis setting ("Mint multi-effect
  recipes (BETA)") toggles it; ON by default. Single-effect minting is unaffected.
- **128-slot pool** (beb5eaa, grown from 32→64→128). One enchantment mod
  (Summermyst) alone used 54 slots; a second mod overflowed 64. The pool is now
  128 (ESL-safe, 128 further slots still in reserve), so a two-or-three
  enchantment-mod list mints everything viable with room to spare.
- **Bundled effect rulings** (4a4496d, 72a6ed5). Some enchantments are built from
  hidden bookkeeping effects that would otherwise block their own family from
  minting — Summermyst's "Drain Skills" is one visible effect plus 17 invisible
  per-skill workers. MEO now ships rulings that waive that machinery, so those
  families mint and convert instead of being stranded. Waivers ship with the mod
  and apply automatically; a mod your list doesn't have is simply inert.

### New: "Allow uncovered enchanted loot" toggle (m52)
- An MCM toggle on **Loot & Spawns**, **default ON** (today's behavior). ON: any
  enchanted loot MEO still has no gem family for keeps its enchantment and appears
  normally. **OFF: that loot is stripped to plain gear**, so nothing enchanted
  slips past MEO. The strip is a live, in-game toggle — no re-patch to flip. It is
  a base-swap, not a destructive record edit: Synthesis only TAGS which generics
  are strippable, the DLL does the swap at runtime.
- **Unique items, artifacts, and quest items are NEVER stripped**, in either
  toggle state — they are structurally excluded at tag time, not by a runtime
  guess. Player-made enchants, MEO's own sockets, and enchantments injected by
  transfer mods are also never stripped.
- **Source-resumes**: an item already stripped stays plain even if you turn the
  toggle back ON; new drops resume enchanted (loot tables are never edited).

### Fixes folded in from the 1.0.6 hotfix line + new this cycle
- **v1.0.6c (m47)** — converted-loot use-after-free: buying/looting converted
  gear then changing cells could crash. AddObjectToContainer LINKS (not copies)
  the extra-data list; the fix mints on an engine-owned heap list.
- **v1.0.6d (m50)** — every-load crash stripping a converted item that lost its
  unique-ID node across save/load: NG's `RemoveByType` null-derefs when a strip
  empties the list. `SafeRemoveAllByType` fixes it; affected saves self-heal.
- **EDU-class enchant hijack + lossless conversion (m51)**. If you use a mod that
  MOVES or UPGRADES enchantments between items, MEO could convert a transferred
  enchant into a gem or, worse, silently drop effects it couldn't fully map. Now:
  a new **"Convert player-enchanted gear to gems"** toggle (default ON — turn OFF
  if you use such a mod), and conversion is **lossless-or-skip** — if any effect
  would be lost, the item is left enchanted instead of partially converted. A
  converted item also keeps a custom name.
- **Tree mode now follows the winning perk tree, not a filename (m51b)**. MEO
  hands out its enchanting perks through the perk tree when its patch wins that
  tree, and by Enchanting skill otherwise. It now decides on whether MEO's perk is
  actually in the winning tree — so a load order where another enchanting overhaul
  overrides the tree correctly falls back instead of leaving perks unreachable.
- **m48/m49** — vendor "personal stock" (the merchant's own worn/sold gear) now
  converts, and a container transfer no longer mis-assigns a socket record between
  two items of the same base.
- **m46** — a mod renaming an effect no longer rebrands unrelated gem recipes.
- **Installer/Synthesis hardening (06f316e, 4c4167b)** — refuses to run if a
  Synthesis group named "MEO" would overwrite the real MEO.esp; reads non-English
  (UTF-8) plugin strings correctly; and the patcher builds through a repo solution
  as Synthesis requires (it was silently failing to build for a stretch of 1.0.5–
  1.0.6b).
- **No executable is ever packaged** (d4e4bc8). MEO installs via Synthesis only;
  the release now asserts the zip contains nothing runnable but the SKSE plugin.

### CAVEATS — please read before testing
- **Phase-3 minting is not yet playthrough-validated.** It is proven at patch/build
  time; this beta exists to test it live. Expect to report minted gems that feel
  off, misnamed, or mis-scaled.
- **The uncovered-loot strip is a one-way, no-undo operation** and has not been
  run in a live cell. It ships **default ON (safe)** — you only get the strip if
  you deliberately turn it OFF. If you do, back up a save first.
- **Multi-effect minting is beta within the beta** — the rider recipes are a
  best-effort approximation, not authored parity.
- **Re-run the MEO Synthesis patcher after installing**, and after any load-order
  change — minting and calibration are derived from *your* list. If you DON'T
  re-run it, nothing breaks: the beta simply behaves like 1.0.6 (no minting, and
  the strip toggle does nothing).
- **The Synthesis settings page needs the patcher versioned to a build that
  contains it** — this tagged beta does; if you point Synthesis at an older tag
  the page renders empty.
- **Stripping (toggle OFF) drops tempering and custom names** along with the
  enchantment — a stripped item becomes the plain base. Uniques/artifacts/quest
  are never stripped, so this only affects generic loot you chose to strip.
- **Downgrade is safe.** You can revert to stable v1.0.6x mid-playthrough: the
  save format is unchanged (co-save v11), so minted-family gems go DORMANT, not
  lost, and loose minted gems are re-minted with their banked XP when you return
  to the beta. (A minted gem just won't function while you're on a DLL that
  doesn't know its family.)

## v1.0.6b — NPC-border hardening + loose-spawn placement fixes + MCM version + leveling nudge (m42–m44, 2026-07-15)

Point release over v1.0.6. Same feature set; stability, placement, and pacing fixes.

Stability
- **MEO no longer touches a living NPC's existing enchanted gear** (m42). Existing
  enchants convert to socketed gems only when the NPC actually **dies** (the corpse
  is handled like a container — a plain inventory swap, no equip) or when **you loot
  them**. MEO still adds its own gem to an enemy's *unenchanted* gear as before, now
  gated on the actor being fully loaded. Removes a crash class where a mod that
  force-equips an enchanted weapon onto a mid-loading NPC could fault the engine
  during conversion.

Loot placement
- **Converted world weapons now sit where the original was** (m43). A socketed
  replacement for a loose weapon on a shelf/rack was spawning at the item's
  authored spot (often embedded in the surface) and sinking **under the floor**;
  it now lands at the weapon's actual on-shelf position.
- **No more duplicate socketed weapons under merchant counters** (m44). Converting
  a merchant's stock left a stray copy of each socketed weapon in the world at the
  chest's position — invisible inside a closed chest, but a visible (and lootable)
  duplicate under an open counter like Warmaiden's. The stray placeholder is now
  cleaned up; the merchant's converted stock is unaffected.

Balance
- **Gem leveling slowed ~10%.** The Gem-XP ladder to reach II/III/IV/V rose from
  400/900/2,800/7,000 to **500/1,000/3,000/8,000** (kept on multiples of 20 so the
  "XP required" always shows clean whole numbers across all gem tiers).

UI
- **The MCM now shows the MEO version** (Debug page → *Version*), stamped from the
  build so it always matches the release you installed.

## v1.0.6 — enchant-sound fix + XP/attunement rebalance + shorter names + Summermyst weakness gems (m40, 2026-07-15)

Audio
- **The enchant/unsheathe "hum" is silenced.** Socketed weapons no longer play
  the looping magic-enchant unsheathe sound on load, vendor open, or re-equip.
  Fixed by a windowed attenuation-mute of the four `MAGEnchantedUnsheathe` sound
  descriptors around MEO's own enchant builds and the post-load restore — global
  audio (ambience, music, dialogue) and genuine combat weapon draws are
  untouched. (The long-standing racket; "must solve or the mod fails.")

Balance
- **XP economy rebalanced, and every XP slider now reads 1.0 as the tuned
  default** (a ×multiplier on a baked base, not a raw rate):
  - Gem-kill Enchanting XP base cut 0.01 → **0.001**. The MCM key was renamed
    `fGemXpSkillXP` → **`fGemKillXpMult`** (0–10×) so a stale saved value can't be
    misread as a multiplier.
  - New-gem discovery XP cut 50 → **10** per family, with a new
    `fDiscoverSkillXP` slider; discovering a big haul no longer spikes Enchanting.
  - Soul-feed Enchanting XP **−25%** (per soul-size ladder).
- **Attunement perk reduced +8% → +5% magnitude per rank** (max +40% → +25%), so
  a fully-attuned level-5 gem no longer runs away past vanilla; perk-tree tooltips
  corrected to match.

Content / UX
- Added 4 gem families for **Summermyst - Enchantments of Skyrim**'s weakness-to-
  fire/frost/shock/poison WEAPON enchantments. On a load order with Summermyst,
  those weapons convert to a socketed weakness gem (magnitudes calibrated to the
  list); without Summermyst the families auto-disable (MGEF plugin absent), so
  nothing changes. New gem MISC forms are pure additions — no existing FormID
  moved. (First hand-authored families toward general auto-minting.)
- Socketed-item titles are now shorthand so multi-gem names stay readable: drops
  "Fortify", trims a trailing " Damage" (Fire II, not Fire Damage II), and
  abbreviates "Resist" → "Res". The gem-joining "+" and support "·" are unchanged;
  loose pouch gems keep full names. New MCM toggle `bFullGemNames` (XP & Balance
  page, default off) restores the full scheme. Fortify Magicka/Stamina keep
  "Fortify" so they stay distinct from the Magicka/Stamina Damage gems.
- Enchanted loot in **containers/chests** now converts to socketed gems the same
  as world/vendor/corpse loot — swept when the container's cell loads, so it
  reads converted in every loot UI (vanilla menu, and QuickLoot/iEquip panels,
  which bypass the vanilla container menu). Static/hand-placed enchanted chest
  content that never triggered a pickup event is now caught; empty containers are
  left untouched (no early leveled-loot rolls, no save bloat).
- **Shields can now be socketed.** Off-hand shields were the one worn armor slot
  MEO didn't accept — plain shields couldn't be manually socketed, NPCs never
  spawned socketed shields, and player-enchanted shields refused conversion, even
  though DESIGN budgets the off-hand a socket. They now take their one gem like
  any other worn piece. (Converted/looted shields already worked.) Also silenced a
  cosmetic "gem disabled: MGEF 000000" log line for the support gems, which have
  no effect of their own by design and function correctly.

Stability & save safety (from a four-part pre-release code review)
- **Sockets now survive a load-order change.** Co-save records (and the gem pouch
  ref) are resolved through SKSE's `ResolveFormID` on load, so adding/removing/
  ESL-flipping a plugin no longer silently orphans every socketed item's gems and
  banked XP. Co-save reads are bounded (a truncated save can't fabricate garbage
  records), and loading a save from a *newer* MEO now warns loudly that saving
  would destroy its records instead of silently doing so.
- **Enemies actually spawn with socketed gear again.** The 2-of-a-kind stacking
  cap (a player-only limit) was wrongly stripping NPC-spawned socketed enchants
  and leaking a co-save record each time; NPC gear now keeps its gem.
- Per-kill Gem XP no longer double-counts on a mastered-gem level-up; follower
  gear reactivation hardened; the stacking-cap cleanup now vouches only for worn
  copies; instance UID minting is wrap/collision-safe; the in-game menu's input
  handling is thread-safe.
- **Installer / Synthesis robustness:** a dangling enchantment (missing master)
  no longer crashes the whole patch run; a self-referential perk chain can't hang
  it; and the generated ESP no longer attaches VMAD scripts it doesn't ship (was
  spamming a Papyrus error every load).

Upgrade note: the two renamed/new XP sliders default to 1.0 from the shipped
settings file. MCM per-save values carry over; the old `fGemXpSkillXP` key is
retired and ignored.

## v1.0.5 — Synthesis-only install (no exe) (2026-07-14)
- **The install-time patcher is now a Synthesis patcher, not a bundled exe.**
  The old `MEO.Installer.exe` (and its unsigned bundled DLLs) tripped Nexus and
  VirusTotal screening. MEO now adapts to your load order through **Synthesis**,
  added from GitHub and compiled locally — so the Nexus download ships **no
  binary tool at all** (just the DLL/ESP/MCM), and there's nothing to get
  flagged. The patcher reuses the exact same record-analysis code, so its output
  (gem calibration + rebuilt perk tree) is byte-identical to the old installer.
- Requirement change: **Synthesis** replaces the standalone installer (and its
  .NET runtime requirement). No gameplay/DLL logic changes from v1.0.4.

## v1.0.4 — slim installer (framework-dependent) + MCM name fix (2026-07-14)
- **Installer no longer bundles the .NET runtime.** The self-contained build was
  a 44 MB single-file exe (the packed runtime is what tripped anti-virus / mod-
  host screening). It's now a framework-dependent build and **requires the .NET 9
  Runtime (x64)** — a free Microsoft download, needed only to run the installer
  once, never in-game. Same behavior, verified byte-identical calibration output.
- MCM title now reads **"marth Enchanting Overhaul"**; the new "Enchanting XP from
  gem kills" slider ships its INI default so it no longer reads -1 on the MCM.
- No gameplay/DLL logic changes from v1.0.3 (version bumped for the release).

## v1.0.3 — enchanting-XP-from-kills nerf + slider (m39, 2026-07-13)
- The per-kill Enchanting skill-XP trickle (from Gem XP your socketed gems earn)
  was leveling Enchanting far too fast; cut the default 5× (0.05 → 0.01) and
  exposed it as an MCM slider ("Enchanting XP from gem kills", `fGemXpSkillXP`,
  0–0.05) so it's tunable in game, down to off.

## v1.0.2 — vendor restock defeated the conversion sweep (m38, 2026-07-13)
- **Dedup no longer breaks the stacking cap or strips follower gear** (m38e,
  from a pre-release review): the save-cleanup pass counted a "first copy wins"
  and dispelled the rest, but the engine gives two same-family-same-level worn
  pieces one shared enchant form — so the 2-of-a-kind cap's legitimate second
  copy was silently removed on every pass. Cleanup now allows exactly as many
  copies as worn items × effects vouch for. Separately, a follower's gem gaining
  XP ran the player-only stacking cap against the follower's gear and stripped
  its enchant; the cap now applies only to player-worn items.
- **Dedup is silent + logging is toggleable** (m38d): the save-cleanup pass that
  dispels stale/duplicate gem effects no longer shows a corner notification (or
  its sound) — it's internal housekeeping that fires on load/replace and
  shouldn't nag. Added a `bEnableLogging` toggle (MCM Debug page + INI, default
  ON) that turns MEO's log file on/off in one place.
- **Socketed-item gold value scales with gem tier** (m38c): a socketed gem used
  to inflate its item's price to 20k+ — the instance enchant carried a flat
  `charge = 0xFFFF`, and the engine prices weapon enchants as
  `fEnchantmentPointsMult × MaxCharge`. Now the premium tracks the gem's tier
  (I–V), routed through the field the engine isn't spending (charge for weapons,
  `costOverride` for constant-effect armor) so gems still never drain. Same price
  whether a vendor's stock was converted or the player socketed it. Global
  `fSocketValueMult` INI knob.
- **Boots are socketable** (m38c): they get converted, so they now take one
  socket (kFeet), like other single-slot gear.


- **Vendor barter stock now actually converts.** v1.0.1 revived the merchant-
  container sweep, but it runs at dialogue-open — and the engine re-rolls the
  vendor's leveled-list stock a beat later, as the *barter* menu opens, dropping
  fresh unconverted generics ("of Burning", "Minor Archery", "Minor Alteration",
  …) on top of the just-converted stock. Deck log confirmed it: the chest
  converted 7 items at dialogue time, yet barter still showed re-rolled enchanted
  names. Added a barter-open sweep that fires *after* the restock (deferred two
  frames so it never mutates the list mid-build — the m19e Belethor breakage),
  then rebuilds the open list through the engine's own inventory-update signal
  (`SendInventoryUpdateMessage`), the same one a buy/sell emits. The dialogue-open
  sweep stays as belt-and-suspenders for vendors whose stock isn't due to restock.

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
