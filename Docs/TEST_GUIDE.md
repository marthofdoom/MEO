# MEO Test Guide — everything since the last test (m7 → m11)

Your last in-game test was **v0.13.0-m6a** ("menu works great"). Since then five
builds shipped. Install **v0.19.0-m11** before testing (MEO.dll + MEO.esp +
MCM/ + Scripts/MEO_MCM.pex — the full zip). This guide walks each change with
concrete steps, expected results, and the log line that proves it.

## Gem roster
**51 gems total — 15 weapon + 36 armor.** All socketable. 49 level I→V; two
(Soul Trap, Waterbreathing) are single-level utilities that never level and
never drop as loot.

## Before anything: verify the build loaded
1. Load a save, open the console, or just check the log.
2. **MEO.log** →
   `~/.local/share/Steam/steamapps/compatdata/3375297225/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim.INI/SKSE/MEO.log`
3. First lines MUST show `MEO native v0.19.0 (M11 ...)`. If they show an older
   version, the DLL didn't update — fix that before believing any test below.
4. Expect: `catalog resolved: N/51 gems live (weapon+armor)` and a `[perks]` line.

**Testing tip — get gems fast:** most of this needs gems in hand. Open the MCM
(see §1) and set **Gem drop chance** and **World weapon socket chance** to max,
and **Level II spawn chance** up, so kills/loot hand you gems quickly. Turn them
back down when done. (Console alternative: `player.additem` needs the load-order
FormID, which varies — the MCM/drop route is easier.)

---

## 1. MCM (v0.16.0-m8)
The headline: a real Mod Configuration menu, paired with an INI the DLL reads live.

- [ ] Pause → **Mod Configuration** → **Marth's Enchanting Overhaul** appears.
- [ ] Two pages: **Loot & Spawns**, **XP & Balance**. Sliders + one toggle.
- [ ] Move **Gem drop chance** to ~0.25. Close the pause menu.
- [ ] **Live apply:** kills should now very frequently drop gems immediately —
      no save/reload needed. (Proves the DLL re-reads `MCM/Settings/MEO.ini` on
      Journal-menu close.) Log: a fresh `config: drop=0.250 ...` line each close.
- [ ] **Gem power scale** slider: set to 1.5, then re-socket a gem (§3) — its
      magnitude should be 50% higher. (Applies at stamp, not retroactively.)

Expected: settings persist across save/reload (they live in the INI).

---

## 2. Loot, rarity curve & the stack-dup fix (v0.15.0-m7)
- [ ] With drop chance up, kill ~20 enemies. Loot the corpses: gems appear.
- [ ] **Rarity curve:** over many drops, common "control" gems (Banish, Fear,
      Turn Undead) show up most; elemental gems (Fire/Frost/Shock) middling;
      **S-tier (Absorb Health, Chaos, Stagger) are rare** (~2% each). Log lines:
      `[loot] corpse gem '<gid>' I on ...`.
- [ ] **Level II drops:** with Level-II chance up, some drops log `... II on ...`.
- [ ] **World weapons born socketed:** explore; some dropped/placed weapons are
      pre-socketed. Log: `[world] ref ... born socketed: <gem> <lvl> <weapon>`.
- [ ] **STACK-DUP FIX (important):** get a stack of ≥2 identical plain weapons
      (e.g. buy 3 Iron Swords). Open the Gem Pouch. The stack shows as one row
      labelled `Iron Sword x3` (a plain stack), **not** a single instance.
      Socket a gem into it → exactly **one** sword becomes socketed, the other
      two stay plain, and **one** gem is consumed. (The old bug enchanted the
      whole stack for one gem.) Log: `[menu] minted instance ... via drop/pickup`.

---

## 3. Armor gems (v0.17.0-m9)
- [ ] Obtain an **armor gem** (corpse drops now include them — e.g. Fortify
      Destruction, Resist Fire, Fortify Carry).
- [ ] Open the Gem Pouch. Select a piece of **armor** (helmet, cuirass,
      gauntlets, ring, or amulet). **Boots should NOT be socketable** (no row,
      or excluded) — that's by design.
- [ ] The gem pane now shows **only armor gems** for an armor item, and **only
      weapon gems** for a weapon (domain filter). Trying to cross-socket is
      blocked ("That gem doesn't fit that kind of gear").
- [ ] Socket an armor gem → its constant effect applies while worn (check your
      active effects / the stat: Fortify Destruction lowers spell cost, Resist
      Fire adds fire resist, etc.). Log: `STAMP ... gem=<gid> ...`.
- [ ] Unequip/re-equip the armor → effect re-applies cleanly.
- [ ] **Armor gems earn XP:** with a socketed armor gem worn, get kills → it
      gains Gem XP and levels like weapon gems. Log: `[xp] ...` for the armor gem.

---

## 4. Enchanting stations — soul feeding + destruction (v0.18.0-m10)
- [ ] Walk up to an **enchanting table** and activate it. The vanilla enchanting
      menu opens AND the **MEO station panel opens over it**. Log:
      `[menu] opened (station): ...`.
- [ ] Select a **socketed** item in the panel. Two buttons appear:
      **Feed Soul Gem** and **Destroy Gem** (only in station mode).
- [ ] **Feed:** have a filled soul gem in your inventory. Click Feed Soul Gem →
      the **smallest** filled soul gem is consumed and its Gem XP is added
      ("Fed a <size> soul (+N Gem XP)"). Log: `[feed] <size> soul -> .../... : +N xp`.
      Feed enough → the gem levels up.
- [ ] **Destroy:** click Destroy Gem → the gem is removed (not returned) and you
      get a filled soul gem sized to 1/10 of its banked XP ("its essence yields a
      <size> soul gem"). Log: `[destroy] ... -> soul tier N`.
- [ ] Close the enchanting menu → the MEO panel closes with it (no lingering
      overlay).

---

## 5. Perk effects (v0.19.0-m11)
Interim: MEO perks are **auto-granted by your Enchanting skill** until the
installer wires them into the tree. Ranks: Attunement 1–5 at Enchanting
0/20/40/60/80; Gem Cutter at 20; Soul Feeder at 40.

- [ ] Check the log after load / after opening+closing the pause menu:
      `[perks] enchanting=NN attuneRank=R gemCutter=B soulFeeder=B`.
      (Open+close the pause menu to force a refresh after an Enchanting skill-up.)
- [ ] **Gem Attunement:** note a gem's magnitude at rank R, then raise Enchanting
      past a threshold (trainer/console `player.setav enchanting 80`), reopen+close
      the pause menu (refresh), and **re-socket** the gem → magnitude is higher
      (+8% per rank; +40% at rank 5). Combines with the MCM power slider.
- [ ] **Gem Cutter (Enchanting ≥ 20):** kills grant noticeably more Gem XP
      (+50%). Compare the `[xp]` numbers below vs above skill 20.
- [ ] **Fortify Enchanting → XP:** wear Fortify Enchanting gear or drink a
      potion; kill XP scales up further (×(1+AV/100)). Higher Enchanting = faster
      gem leveling.
- [ ] **Soul Feeder (Enchanting ≥ 40):** feeding a soul at a station grants
      **double** the XP vs below 40 (e.g. Grand = 400 instead of 200).

---

## What is NOT in yet (don't test — coming in later stage-3 builds)
- Multi-socket (2 linked sockets per item) and the two socket perks
  (Twinned Fitting, Master Jeweler).
- Elemental affinity perks + Facet Insight.
- The C# installer that replaces the load order's enchanting tree with MEO's
  perks (until then, perks are auto-granted by skill as above).

## If something's wrong
Grab the MEO.log and note the version header + the relevant `[menu]`/`[loot]`/
`[feed]`/`[destroy]`/`[perks]`/`[xp]` lines around the failure.
