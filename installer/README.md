# MEO Installer (stage 3c)

Install-time patcher, run once against the user's MO2 setup. Two jobs:

1. **Perk tree replacement** — swap the load order's enchanting perk tree
   (whatever won the AVEnchanting override) for MEO's 13 gem perks (attunement
   ×5, Gem Cutter, Soul Feeder, the three elemental affinities, Facet Insight,
   and the two socket perks), retiring the DLL's auto-grant.
2. **Per-list calibration + conversion table** — scan the load order and write
   `meo_calibration.json`: per-family magnitude curves, rider recipes, rank
   ladders, and the CONVERSION table that maps generic (non-artifact) enchanted
   items to a plain base + the matching gem. The DLL converts them in-game
   (no leveled-list surgery); artifacts and unique enchantments stay untouched.

## Stack

.NET 9 console app + [Mutagen](https://github.com/Mutagen-Modding/Mutagen)
(`Mutagen.Bethesda.Skyrim`). Runs natively on Linux — no game or VFS needed.

MO2 resolution is done manually (no VFS on Linux): `modlist.txt` is
highest-priority-first, first hit wins, `Stock Game/Data` is the final
fallback. Same logic as `tools/scan_loadorder.py`.

## Build & run

```sh
# one-time: user-local SDK (no root)
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 9.0 --install-dir ~/.dotnet
export DOTNET_ROOT=~/.dotnet PATH=~/.dotnet:$PATH

cd installer/MEO.Installer
ulimit -n 8192   # 3400+ plugins = 3400+ open memory-mapped files
dotnet run -c Release -- /mnt/gaming/modlists/LoreRim Default
```

## Proven (2026-07-09)

- Full LoreRim read: 3418 plugins resolved (0 missing), all parsed as lazy
  binary overlays in ~1.5 s.
- Winning-override + link-cache pass: dumped the live AVEnchanting perk
  tree (Requiem + Special Feats + Wand Keywords + Ordinator, 15 nodes)
  with cross-master perk resolution in 0.2 s.
