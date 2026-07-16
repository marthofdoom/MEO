# MEO Installer / Synthesis patcher

Install-time patcher that adapts MEO to the user's load order. Two projects, one
shared brain:

- **`MEO.Synthesis/`** — the SHIPPED path (v1.0.5+). A [Synthesis](https://github.com/Mutagen-Modding/Synthesis)
  patcher, added in Synthesis from this GitHub repo and compiled locally on the
  user's machine. Distributing source (no binary on Nexus) sidesteps the exe/AV
  screening that got the bundled installer blocked. Requires Synthesis.
- **`MEO.Installer/`** — the original standalone .NET console exe (legacy /
  dev use). Same jobs, run directly. Its record-analysis lives in
  `Commands.cs`, which `MEO.Synthesis` compiles too (`<Compile Include=
  "..\MEO.Installer\Commands.cs" />`) — ONE code path, so both produce
  byte-identical output. `Mo2LoadOrder`/CLI stay installer-only.

Both do the same two jobs, run once (and again after any load-order change):

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

## Synthesis patcher (`MEO.Synthesis`)

`RunPatch(state)`:
1. `Commands.ApplyPatch(state.LoadOrder, state.LinkCache, state.PatchMod)` —
   perk-tree edit into Synthesis's own output mod (`state.PatchMod`),
   non-interactive keep-all for foreign perks.
2. `Commands.WriteCalibration(calLo, calCache, catalog, calOut)` where `calLo`
   is a load order we build OURSELVES (see Creation Club, below). Writes
   `meo_calibration.json` to `state.DataFolderPath/SKSE/Plugins/MEO/` (under MO2,
   that lands in Synthesis's overwrite).

`gem_catalog.json` is linked from `data/` (not copied) and resolved at runtime
from `AppContext.BaseDirectory` (NOT `Assembly.Location`, which is `""` in a
single-file build).

**Users add it in Synthesis** as a Git-repository patcher pointing at this repo,
project `installer/MEO.Synthesis/MEO.Synthesis.csproj` (or by opening
`assets/MEO.synth`, which does it for them).

### TRAP: Synthesis requires a SOLUTION at the repo root (`MEO.sln`)

Synthesis's git-patcher flow **clones this repo, locates a `.sln`, and builds the
project named by `MEO.synth`'s `SelectedProject`**. With no solution it stops on a
blocking **"could not locate solution to run"** — the patcher never runs.

This shipped broken through v1.0.5/1.0.6/1.0.6b: the repo had *never* had a `.sln`,
and since v1.0.5 went exe-free the Synthesis patcher is THE install path, so every
user hit it. It hid because we only ever built the patcher locally (`dotnet run`),
never through Synthesis's clone-and-build. Rules learned:

- **Keep `MEO.sln` at the repo root**, and its project path must match
  `MEO.synth`'s `SelectedProject` **verbatim** (backslashes included).
- **Keep the solution minimal — the patcher project only.** Synthesis rewrites
  Mutagen/Synthesis package versions in the projects it builds; extra projects are
  needless build surface and version-conflict risk on the user's machine.
  (`MEO.Installer` builds fine standalone via its `.csproj` — CI does exactly that
  — so it doesn't need to be in the solution.)
- **No new tag/release is needed to fix the patcher.** Synthesis's
  `GithubPatcherSettings` defaults to `PatcherVersioning=Branch` +
  `FollowDefaultBranch=true`, so a newly-added patcher builds the **default branch
  tip** — a push to `main` reaches users immediately.
- `assets/MEO.synth`'s schema (`{"AddGitPatcher":{"Url","SelectedProject"}}`) is
  per the official Synth-File docs — that part was never the problem.
- **CI guards this now** (`.github/workflows/installer.yml` → `synthesis-patcher`):
  it asserts the solution + `SelectedProject` exist and agree, then builds through
  the solution exactly as Synthesis does. Don't remove it — building only
  `MEO.Installer` (the legacy exe) is what let this ship unnoticed.

## Phase 3 auto-minting (`MintFamilies`, inside `WriteCalibration`)

Generic-loot enchant clusters that match NO catalog family are promoted to
**minted** gem families on MEO.esp's reserved pool (32 slots × 5 MISC forms at
0xB00–0xB9F; `data/pool_forms.frozen.json` is the contract, linked into the
Synthesis build beside `gem_catalog.json`). Emitted as a top-level `"minted"`
section in `meo_calibration.json` — additive: a pre-phase-3 DLL ignores the key
and skips its conversions at gid lookup, so the calibration is safe on every
shipped DLL.

Rules (marth 2026-07-16): overflow = leave uncovered + report; minted gems are
CONVERSION-ONLY (no spawn/vendor pools); filters = archetype whitelist
(Value/PeakValue/DualValue-Modifier, Absorb), magnitude > 0, ≥4 generic items
or ≥2 tiers, cast-shape guards (weapon needs FF + touch/target-actor, armor
needs constant+self and non-hostile; minority-domain carriers are excluded
from conversion). Curve = p90 anchor on the canonical LINEAR ramp, floored at
0.1/level; identity gid = `x_<stem≤16>_<fnv1a-hex8 of full filename>_<mgef-fid
hex6>` (derived from the primary MGEF's own FormKey — NEVER change its shape,
co-saves store it; INVARIANTS 26).

**Multi-effect recipes (BETA — marth 2026-07-16 "as close to parity as
possible")**: many modded enchants are ONE conceptual effect built from
several MGEFs. Companions are classified mechanically (never by name):
*desc-inert* (zero-mag, unconditional, inert-without-magnitude archetype —
ValueModifier family, or Script with no attached scripts) are waived as pure
text/markers; *conditional* are never carried (pin their items); *real*
companions are adopted as gem RIDERS (≤4, by carrying weight; median ratio vs
the primary, ratio 0 for scripted zero-mag procs) — the same rider machinery
curated families use, emitted per minted entry and resolved by the DLL.
**A slot is only assigned after ≥1 carrier passes the full lossless gate** —
a family that would convert nothing never burns an append-only slot (Drain
Skills' 18 sub-effects bust the rider cap and are skipped pre-slot).
Surfaced as the Synthesis setting "Mint multi-effect recipes (BETA)" (default
ON) / standalone `--no-mint-riders`. Known beta caveats: rider sets are the
UNION across the cluster's recipes (a low-rank item can carry a high-rank
recipe's procs), and riders apply unconditionally (a vs-player-shaped helper
rides all hits). Authoria validation: 9/9 minted families convert (1,321
items; was 805 with 5 dead slots), PINNED 708 → 24.

**`meo_pool_assignments.json`** (next to the calibration) is APPEND-ONLY
per-user state: a gid keeps its pool slot forever, and a vanished family's
slot stays burned — loose pool gems in an old save must never change species
when the list is re-patched. Verified on LoreRim+Summermyst(+Thaumaturgy):
re-run byte-identical; Summermyst removed → slots stay burned, minted section
empties; Thaumaturgy added → appends at the next free slots.

## Creation Club — must read `Skyrim.ccc` yourself

Synthesis's `state.LoadOrder` (and the raw `plugins.txt`) OMITS Creation Club
plugins on an AE install — the ~74 CC plugins load via `<GameRoot>/Skyrim.ccc`,
not `plugins.txt`. Relying on `state.LoadOrder` dropped ~24% of conversions and a
whole family (`chaos`, a CC effect). So `MEO.Synthesis` builds its calibration
load order the installer's way (`BuildCalibrationLoadOrder`): base masters →
`Skyrim.ccc` → `plugins.txt`, resolved from `state.DataFolderPath`. The
standalone installer's `ResolveGame` already reads `Skyrim.ccc` for the same
reason. (The perk-tree step doesn't need CC — CC adds no enchanting perks — so it
stays on `state.LoadOrder`.)

## Native-Linux case-sensitivity caveat

Run on a **case-sensitive** filesystem (native Linux), Mutagen fails to resolve
some CC master references → fewer records. Same binary, same LoreRim-less deck
load order (74 CC): native-Linux gave 4139 conversions (chaos 0); the SAME code
under **Proton/Wine** gave 5392 (chaos 49). This is a Mutagen-on-Linux
limitation, not app-specific. Implications:
- **Testing:** validate a load-order patcher under Proton/Wine (or Windows), not
  native Linux, or you'll chase a phantom regression.
- **Users:** Synthesis via Proton (typical Linux Skyrim setup) is
  case-insensitive → correct; only native-Linux Synthesis under-counts.

## Build & run

```sh
# one-time: user-local SDK (no root)
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 9.0 --install-dir ~/.dotnet
export DOTNET_ROOT=~/.dotnet PATH=~/.dotnet:$PATH
ulimit -n 8192   # 3400+ plugins = 3400+ open memory-mapped files

# standalone installer (dev), MO2:
cd installer/MEO.Installer && dotnet run -c Release -- /mnt/gaming/modlists/LoreRim Default
# or game-root/vanilla: dotnet run -c Release -- write-calibration <gameRoot> <plugins.txt|auto> <catalog> <out>

# Synthesis patcher, standalone run (verification): pass a REAL data folder + plugins.txt
cd installer/MEO.Synthesis && dotnet run -c Release -- run-patcher \
  --GameRelease SkyrimSE --DataFolderPath <Data> --LoadOrderFilePath <plugins.txt> --OutputPath "<out>/MEO - Patch.esp"
```

Parity check: byte-diff the patcher's `meo_calibration.json` against the
standalone installer's on the SAME load order (ignore the `generated` timestamp).
Verified 2026-07-14: byte-identical on the deck (native-Linux 4139; Proton 5392).

## Proven (2026-07-09)

- Full LoreRim read: 3418 plugins resolved (0 missing), all parsed as lazy
  binary overlays in ~1.5 s.
- Winning-override + link-cache pass: dumped the live AVEnchanting perk
  tree (Requiem + Special Feats + Wand Keywords + Ordinator, 15 nodes)
  with cross-master perk resolution in 0.2 s.
