# MRO In-Game Test Procedures

Console-driven, repeatable checks for every system. Clean up `modav`
deltas afterwards — they persist in the save.

## Install sanity
- `help MRO_ 0` — GLOBs, quests, spells, perks all resolve (FE-prefixed ESL ids)
- `help MRO_DR76 4` and `help MRO_Barter01 4` — perk ladders loaded
- Active Effects UI shows "Elemental Absorb" and "Carry Weight Bonus"
  (constant self abilities with FULL names; may take one 30s heartbeat)
- `player.getav CarryWeight` — +150 over base
- MCM "marth Requiem Overhaul" present (else `setstage ski_configmanagerinstance 1`)

## Elemental absorb
```
player.getav FireResist
player.modav FireResist 150        ; well past 100
player.damageav Health 200
player.cast 12FCD player right     ; Firebolt at self
```
Health goes UP on the hit. Controls: at exactly 100 → no damage, no heal;
MCM toggle off → no heal. Overflow: at full health, the heal spills into
stamina/magicka (watch those bars). MCM Features > Live Status shows
"(absorbs N%)" per element while over 100.

## Physical DR ladder
```
player.getav DamageResist
player.modav DamageResist 1000     ; cross the kink
```
Wait 30s. `player.hasperk <MRO_DRxxPerk fid>` — rung matches
MCM Features > Live Status "Physical DR". At the 99% slider point damage
taken drops to ~1/25th of the engine cap's. Toggle off → perk removed
within 30s. Cleanup: `player.modav DamageResist -1000`.

## Mastery XP gating
- Swing at air / a rock / a corpse / a follower: NO progress (MCM Mastery
  page percentages unchanged).
- Land hits on a hostile enemy with base skill >= 100: progress ticks;
  level-up shows the vanilla-style skill-increase banner.
- Casting spells out of combat: no school XP; in combat: XP per cast.
- Crafting menus and barter menus grant session XP (skill >= 100 only).

## Vendor gold
Values are baked at ESP generation from the live load order (doubled).
Merchants only re-roll chest gold on CELL RESET — wait 72+ in-game hours
away from the shop, then barter and check gold (e.g. spell vendors 1840).

## Speech mastery barter ladder
With Speech mastery leveled: buy prices drop up to 20%, sell prices rise
up to 25%. Verify perk: `player.hasperk <MRO_BarterXXPerk fid>`; compare
a vendor's price for the same item before/after `player.addperk`/removal.

## Smithing temper caps
`getgamesetting fSmithingArmorMax` — rises with Smithing mastery
(base x2 at full mastery). Temper an item at a workbench to confirm the
higher ceiling.

## Update-in-place
After installing a new MRO build over an existing save: one
"updated in place" notification on next heartbeat, no repeated intro
popup, mastery XP array intact (MCM percentages preserved).
