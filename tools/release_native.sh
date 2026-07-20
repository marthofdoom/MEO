#!/usr/bin/env bash
# Cut an immutable, MO2-installable MEO release built around the CI-built DLL.
#
# MEO.dll can only be built on Windows CI (CommonLibSSE-NG needs the MSVC ABI),
# so this pulls the DLL from a successful `native` workflow run and wraps it in
# the MO2 archive layout: the zip root IS the virtual Data folder, so MO2
# installs it with zero manual file placement — no dropping DLLs by hand.
#
# NO EXECUTABLE IS EVER PACKAGED (marth's standing rule, 2026-07-19). Since
# v1.0.5 the SYNTHESIS PATCHER is the one and only install path — it derives the
# perk tree and the per-list calibration. The old MEO.Installer.exe path is dead;
# this script asserts, positively, that the zip contains no executable at all, so
# a stale artifact or a copied line can never reintroduce one.
#
# Layout produced (unzip = Data/):
#   SKSE/Plugins/MEO.dll                    (+ MEO.pdb if CI produced one)
#   SKSE/Plugins/MEO/meo_runtime.json
#   MEO.esp
#   MEO-README.txt                          (install steps — Synthesis)
#   fomod/info.xml                          (MO2 metadata card)
#
# Usage:
#   tools/release_native.sh <version> "desc" [--run <id>] [--esp] [--json]
# Examples:
#   tools/release_native.sh v0.2.0-m0 "M0 native skeleton"          # DLL only
#   tools/release_native.sh v0.4.0-m2 "native socket" --esp --json  # full mod
#
# Defaults to the latest SUCCESSFUL native run. Releases are immutable: the
# script refuses to overwrite an existing releases/<version>/.
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:?usage: tools/release_native.sh <version> [description] [--run <id>] [--esp] [--json]}"
shift
DESC=""
RUN_ID=""
# Every release is a COMPLETE, standalone mod (marth's rule 2026-07-08: MO2
# install REPLACES a mod's content, so a partial zip = broken download). ESP,
# MCM config + settings, the compiled MCM script, and the runtime JSON are
# ALWAYS packaged alongside the DLL. --esp/--json accepted for back-compat, ignored.
WITH_ESP=1
WITH_JSON=1
# First non-flag arg (if any) is the description.
if [[ $# -gt 0 && "$1" != --* ]]; then DESC="$1"; shift; fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run)  RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        # Refuse loudly rather than ignore: anyone passing this is working from
        # the dead exe-installer playbook and needs to know it's gone.
        --installer-run)
            echo "ERROR: --installer-run is retired. MEO ships NO executable; the Synthesis" >&2
            echo "       patcher is the install path. Drop the flag." >&2
            exit 1 ;;
        --esp|--json) shift ;;  # always-on now; accepted for back-compat
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

DEST="releases/$VER"
ZIP="$DEST/MEO-$VER.zip"
if [[ -e "$DEST" ]]; then
    echo "ERROR: $DEST already exists. Releases are immutable; bump the version." >&2
    exit 1
fi

# Resolve the CI run that built the DLL.
if [[ -z "$RUN_ID" ]]; then
    RUN_ID=$(gh run list --workflow=native.yml --status success --limit 1 \
             --json databaseId -q '.[0].databaseId')
    [[ -n "$RUN_ID" ]] || { echo "ERROR: no successful 'native' run found. Push native/ and let CI go green first." >&2; exit 1; }
fi
echo "== native DLL from run $RUN_ID =="

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/SKSE/Plugins"

# Pull MEO.dll (+ MEO.pdb if present) straight into the Plugins folder.
gh run download "$RUN_ID" -n MEO-dll -D "$STAGE/SKSE/Plugins"
[[ -f "$STAGE/SKSE/Plugins/MEO.dll" ]] || { echo "ERROR: artifact had no MEO.dll" >&2; exit 1; }
echo "DLL: $(stat -c '%s bytes' "$STAGE/SKSE/Plugins/MEO.dll")"

# VERSION-MATCH GATE (Fable, 2026-07-20): the MCM readout is baked from
# kMEOVersion at ESP-generation time, but the DLL is pulled from CI — if --run
# points at a build of a DIFFERENT version, the zip ships a DLL whose baked
# version disagrees with the MCM/log (the v1.0.6 stale-zip lesson, INVARIANTS 24).
# kMEOVersion is a plain literal in .rdata; assert the DLL actually contains the
# version we're cutting. Strip a leading 'v' from the tag for the match.
VBARE="${VER#v}"
if ! grep -aq "$VBARE" "$STAGE/SKSE/Plugins/MEO.dll"; then
    echo "ERROR: the CI DLL does not contain version string '$VBARE' — the --run you gave" >&2
    echo "       built a different version. Push the version bump, wait for CI green, and" >&2
    echo "       cut from THAT run. (Refusing to ship a version-mismatched zip.)" >&2
    exit 1
fi
echo "version-match: DLL contains '$VBARE' OK"

# Regenerate ALL DLL-adjacent content fresh so the zip is a complete, standalone
# mod every time (never rely on files left in a previous install).
python3 MEO_GenerateESP.py out            # MEO.esp + MCM/ + SKSE/Plugins/MEO/meo_runtime.json
tools/compile.sh MEO_MCM >/dev/null       # Scripts/MEO_MCM.pex (Papyrus, local Proton toolchain)

mkdir -p "$STAGE/SKSE/Plugins/MEO"
cp out/SKSE/Plugins/MEO/meo_runtime.json "$STAGE/SKSE/Plugins/MEO/"
# The Synthesis patcher derives per-list rider calibration from this catalog
# (write-calibration); it must ship inside the mod.
cp data/gem_catalog.json "$STAGE/SKSE/Plugins/MEO/"
# Phase 3 pool contract: MintFamilies resolves it BESIDE the catalog —
# without it the patcher silently skips auto-minting.
cp data/pool_forms.frozen.json "$STAGE/SKSE/Plugins/MEO/"
# Bundled effect rulings (phase 3): waivers that are properties of a MOD rather
# than of a user's list — without these the affected families cannot mint at all
# on any install that has the mod. The installer reads <exe dir>/rulings/*.
mkdir -p "$STAGE/SKSE/Plugins/MEO/rulings"
cp data/rulings/*.rulings.json "$STAGE/SKSE/Plugins/MEO/rulings/"
# m24 menu skins: three OFL typefaces (Cinzel head / EB Garamond body /
# Inter sans) + their licenses; the DLL bakes them at init, skins pick.
mkdir -p "$STAGE/SKSE/Plugins/MEO/fonts"
cp data/fonts/head.ttf data/fonts/body.ttf data/fonts/sans.ttf \
   data/fonts/OFL-*.txt "$STAGE/SKSE/Plugins/MEO/fonts/"
cp out/MEO.esp "$STAGE/"
mkdir -p "$STAGE/MCM";     cp -r out/MCM/. "$STAGE/MCM/"
mkdir -p "$STAGE/Scripts"; cp out/Scripts/MEO_MCM.pex "$STAGE/Scripts/"

# MO2 metadata card + install steps shipped inside the mod folder.
mkdir -p "$STAGE/fomod"
cat > "$STAGE/fomod/info.xml" <<EOF
<fomod>
  <Name>marth's Enchanting Overhaul</Name>
  <Author>marth</Author>
  <Version>$VER</Version>
  <Website>https://github.com/marthofdoom/MEO</Website>
  <Description>Socketable, leveling enchantment gems. After installing, run the MEO patcher in Synthesis to adapt the enchanting perk tree and gem calibration to your load order — see MEO-README.txt.</Description>
</fomod>
EOF
cat > "$STAGE/MEO-README.txt" <<'EOF'
marth's Enchanting Overhaul (MEO)
=================================
Socketable, leveling enchantment gems.

RUNNING THE PATCHER (required once, and again after any load-order change)
-----------------------------------------------------------------------------
MEO adapts itself to YOUR load order — nothing is hardcoded. That work is done
by the MEO patcher for SYNTHESIS. It:
  * replaces the enchanting perk tree's crafting perks with MEO's gem perks,
  * derives the gem calibration for your list and writes it to
    SKSE/Plugins/MEO/meo_calibration.json — WITHOUT THIS FILE, LOOT
    CONVERSION DOES NOTHING IN GAME.

MEO ships no executable of any kind. If you are following an older guide that
mentions "MEO.Installer.exe", that path is retired — use Synthesis.

SETUP:
  1. Install this mod and enable it (left pane) + MEO.esp (right pane).
  2. In Synthesis, add the MEO patcher:
       Git Repository -> https://github.com/marthofdoom/MEO
     Synthesis builds it from source; leave the default branch/versioning.
  3. Run the Synthesis group. Enable the patcher's output plugin in your load
     order (right pane), AFTER any other enchanting overhaul.

DID IT WORK? Check that this file now exists in your MEO mod folder (or your
Synthesis output, depending on your setup):
  SKSE/Plugins/MEO/meo_calibration.json      <- the important one
In game, the SKSE log (Documents/My Games/.../SKSE/MEO.log) should say
"calibration: N family recipe(s), M conversion(s) loaded" — not
"no calibration file".

If MEO's perks do not appear in the enchanting tree, MEO.log will say so and
fall back to granting them by Enchanting skill; that means another enchanting
overhaul is overriding the tree — load MEO's patch output after it.
EOF

# Completeness gate: refuse to write a release missing any required file.
for req in "SKSE/Plugins/MEO.dll" "MEO.esp" "Scripts/MEO_MCM.pex" \
           "MCM/Config/MEO/config.json" "MCM/Settings/MEO.ini" \
           "SKSE/Plugins/MEO/meo_runtime.json" "SKSE/Plugins/MEO/gem_catalog.json" \
           "SKSE/Plugins/MEO/pool_forms.frozen.json" \
           "SKSE/Plugins/MEO/rulings/summermyst.rulings.json" \
           "SKSE/Plugins/MEO/fonts/head.ttf" "SKSE/Plugins/MEO/fonts/body.ttf" \
           "SKSE/Plugins/MEO/fonts/sans.ttf" \
           "fomod/info.xml" "MEO-README.txt"; do
    [[ -f "$STAGE/$req" ]] || { echo "ERROR: release incomplete — missing $req" >&2; exit 1; }
done

# NO-EXECUTABLE GATE (marth's standing rule). MEO installs via Synthesis and
# ships nothing runnable. This is a POSITIVE assertion over the staged tree, not
# a matter of "we didn't add one" — it catches a stale artifact, a resurrected
# download line, or anything a future edit drags in. Nexus trust depends on it.
if find "$STAGE" -type f \( -iname '*.exe' -o -iname '*.dll' -o -iname '*.bat' \
                            -o -iname '*.cmd' -o -iname '*.ps1' -o -iname '*.msi' \) \
        ! -path "$STAGE/SKSE/Plugins/MEO.dll" | grep -q .; then
    echo "ERROR: executable content in the release — MEO ships NO executable." >&2
    echo "       Offending files (only SKSE/Plugins/MEO.dll is allowed):" >&2
    find "$STAGE" -type f \( -iname '*.exe' -o -iname '*.dll' -o -iname '*.bat' \
                             -o -iname '*.cmd' -o -iname '*.ps1' -o -iname '*.msi' \) \
         ! -path "$STAGE/SKSE/Plugins/MEO.dll" -printf '         %P\n' >&2
    exit 1
fi

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
printf '%s\n' "$VER" > "$DEST/VERSION"
[[ -n "$DESC" ]] && printf '%s\n' "$DESC" > "$DEST/NOTES.txt"
printf 'built from native run %s (no executable packaged; installs via Synthesis)\n' "$RUN_ID" >> "$DEST/NOTES.txt"

echo "== manifest (MO2 installs this as Data/) =="
unzip -l "$ZIP"
echo
echo "Wrote $ZIP"
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
