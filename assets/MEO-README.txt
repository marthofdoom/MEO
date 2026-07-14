marth's Enchanting Overhaul (MEO)
=================================
Socketable, leveling enchantment gems.

REQUIREMENTS
------------
  * SKSE64
  * Address Library for SKSE Plugins
  * SkyUI
  * MCM Helper
  * .NET 9 Runtime (x64) — ONLY to run MEO.Installer.exe once (not needed in
    game). Free from Microsoft: https://dotnet.microsoft.com/download/dotnet/9.0
    The "Run console apps" .NET Runtime is enough; the .NET Desktop Runtime also
    works. If the installer says it can't find a runtime, install this and retry.
  * No DLC required (Dawnguard/Dragonborn only add a few extra gems if present).

RUNNING THE INSTALLER (required once, and again after any load-order change)
-----------------------------------------------------------------------------
MEO.Installer.exe adapts MEO to YOUR load order — nothing is hardcoded. It:
  * replaces the enchanting perk tree's crafting perks with MEO's gem perks
    (asking you, perk by perk, about anything unusual it finds; answers are
    saved in "MEO - Patch.choices.json" — edit/delete it to change your mind),
  * derives the gem calibration for your list and writes it to
    SKSE/Plugins/MEO/meo_calibration.json — WITHOUT THIS FILE, LOOT
    CONVERSION DOES NOTHING IN GAME.

IMPORTANT: the exe writes its output NEXT TO ITSELF. Always run it from the
folder it was installed to — never from Downloads or a temp folder.

WITH MO2 (modlists):
  1. Install this mod and enable it (left pane) + MEO.esp (right pane).
  2. Right-click the mod -> Open in Explorer -> run MEO.Installer.exe.
     It finds the MO2 instance above it automatically.
  3. Enable the generated "MEO - Patch.esp" (right pane, near the end).

WITHOUT A MOD MANAGER (vanilla / manual / Steam Deck):
  1. Unzip this whole archive into the game's Data folder.
  2. Run Data/MEO.Installer.exe (on Linux/Deck: with the same Proton/Wine
     the game uses, from a terminal if possible so you can read the output).
     It detects the game folder above it and uses the game's own plugins.txt.
  3. Enable "MEO - Patch.esp" in the in-game CREATIONS/MODS menu.

DID IT WORK? Check that these files now exist next to the exe:
  MEO - Patch.esp
  MEO - Patch.choices.json
  SKSE/Plugins/MEO/meo_calibration.json      <- the important one
If meo_calibration.json is missing, the run failed partway: run the exe from
a terminal and read the "CALIBRATION FAILED" message it prints.
In game, the SKSE log (Documents/My Games/.../SKSE/MEO.log) should say
"calibration: N family recipe(s), M conversion(s) loaded" — not
"no calibration file".

The installer never launches or touches the game — it only reads your load
order and writes the files above. If Windows SmartScreen warns about an
unrecognized app: the exe is built by public GitHub CI from the sources at
https://github.com/marthofdoom/MEO (Details -> Run anyway).
