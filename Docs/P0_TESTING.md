# P0 In-Game Test Matrix — validating "the item is the database"

P0 exists to answer ONE question before anything is built on top of it: is a
runtime enchantment written by `WornObject.CreateEnchantment` (real Fire
effect + a hidden marker effect) reliable across save/load, re-equip, and
rebuild? If any check below fails, the stateless-socket design (marker path
AND native-index path) must change before P1. See Docs/DESIGN.md §7 and §Why
native.

## Setup
1. Install `MEO-v0.0.1-p0.zip` via MO2 on a **clean baseline save** (one that
   has never loaded MEO — back it up read-only per DESIGN §9).
2. Enable `MEO.esp`. Load the clean save.
3. `help MEO_ 0` in console — MGEF/MISC/SPEL/FLST/QUST records resolve.
4. Powers menu now lists **Gem Pouch** (startup quest grants it on first load;
   wait a beat / reload if not). Equip a plain **Iron Sword in the right hand**.

## The driver
Each cast of Gem Pouch acts on the worn right-hand weapon and advances one
step, popping a message box with the state READ BACK FROM THE ITEM:

    none -> Fire I -> Fire II -> Fire III -> Fire IV -> Fire V -> (removed) -> none ...

The message box reports: action, weapon name, **decoded gem** (from the marker),
fire magnitude, and enchant effect count. "Decoded gem" is the load-bearing
number — it proves we can recover socket state from the item alone.

## Checks

| # | Question | Procedure | PASS = |
|---|---|---|---|
| 1 | Insert writes a working enchant | Cast once. `player.getav ...` n/a — swing at a training dummy | Box shows "Fire I", fire magnitude 5, effect count 2; sword now deals fire damage; name shows `[Fire I]` |
| 2 | Marker decodes exactly | Cast to reach Fire III | Box "Decoded gem: Fire III", fire magnitude 15 every time |
| 3 | Level-up rebuilds cleanly | Keep casting II->III->IV->V | Each step shows the new level, effect count stays 2 (old effects replaced, not stacked) |
| 4 | Remove is clean | Cast at Fire V | Box "REMOVE", decoded "(none)"; sword loses fire damage; name back to `Iron Sword`; still equippable/usable |
| 5 | **Survives save/load** | Reach Fire III. `save meo_p0`. `load meo_p0`. Cast again | First readback after load shows Fire III (NOT none); then advances to IV |
| 6 | **Survives re-equip** | Reach Fire III. Unequip + re-equip the sword (inventory or `unequipitem`/`equipitem`). Cast | Readback shows Fire III |
| 7 | **Stacked identical items** (the known risk) | Have 2+ identical Iron Swords. Reach Fire II on the equipped one. Check the other sword in inventory | Only the equipped instance is enchanted; the stack split correctly, the other sword is plain. Record exact behavior even if it splits oddly |
| 8 | Uninstall health | After removal (#4), the sword is a normal Iron Sword with no orphaned name/enchant | Item fully vanilla-clean |

## What each failure would mean
- #5 fails -> runtime enchantments don't persist; the whole item-as-database
  premise is wrong. Pivot: native co-save index becomes mandatory, not optional.
- #3 stacks instead of replacing -> removal/level-up needs SetEnchantment(None)
  before each CreateEnchantment; adjust Apply().
- #7 misbehaves badly -> socketing must force-split or work on a unique
  instance (temp-equip already helps); document the constraint for the menu UX.

## Console cheat sheet
- `help MEO_ 0` — resolve records
- `player.additem <IronSword fid> 1` — get test swords (Iron Sword = 0x00012EB7)
- `player.equipitem <fid>` / `player.unequipitem <fid>`
- `save meo_p0` / `load meo_p0`
- Crash logs: `.../My Games/Skyrim Special Edition/SKSE/crash-*.log` (P0 is
  pure Papyrus — it should never CTD; a native AV implicates a bad record).
