marth's Enchanting Overhaul (MEO)
=================================
Socketable, leveling enchantment gems.

REQUIREMENTS
------------
  * SKSE64
  * Address Library for SKSE Plugins
  * SkyUI
  * MCM Helper
  * Synthesis  (https://github.com/Mutagen-Modding/Synthesis)
      MEO adapts itself to your load order through a Synthesis patcher (see
      SETUP). Synthesis brings its own .NET dependency, so there is nothing
      extra to install for MEO, and no program to run by hand.
  * No DLC required (Dawnguard/Dragonborn only add a few extra gems if present).

SETUP
-----
1. Install this mod with your mod manager, near the END of your load order,
   and enable MEO.esp.

2. Adapt MEO to YOUR load order with Synthesis (this replaces the old
   standalone installer — there is no exe):
     EASIEST: download the MEO.synth file, then in Synthesis select a group and
       double-click it — it adds the patcher for you (right project preselected).
     OR MANUALLY:
       a. Open Synthesis, add a new patcher -> Git Repository, point it at:
            https://github.com/marthofdoom/MEO
          Project:  installer/MEO.Synthesis/MEO.Synthesis.csproj
       b. Run your Synthesis pipeline. (Synthesis builds it for you.)

   PURE VANILLA + AE + CREATION CLUB (no mods that add enchanted gear)? Use the
   "Vanilla+AE+CC pre-calibrated" optional file instead and skip Synthesis.
   Synthesis reads your load order and:
     * derives every gem's magnitude curve, elemental recipe, and rank ladder
       from the enchantments your list actually ships, and
     * rewrites the enchanting perk tree to MEO's gem perks,
   writing all of it into the Synthesis output (a patch plugin + the file
   SKSE/Plugins/MEO/meo_calibration.json). WITHOUT THE CALIBRATION FILE, LOOT
   CONVERSION DOES NOTHING IN GAME — so always run Synthesis after installing
   MEO, and again after any load-order change.

3. Enable the Synthesis output in your load order (Synthesis normally does this
   for you) and play.

DID IT WORK?
------------
In game, the SKSE log (Documents/My Games/Skyrim Special Edition/SKSE/MEO.log)
should say "calibration: N family recipe(s), M conversion(s) loaded" — not
"no calibration file". Enchanting stations open the gem menu; pre-enchanted
loot you pick up converts to a base item plus its gem.

NOTES
-----
No FOMOD — all options are runtime MCM toggles (MCM -> marth Enchanting
Overhaul). Upgrades are save-safe within a major line: socket data lives in the
co-save and migrates itself on load. Gems are earned, never bought or sold.
