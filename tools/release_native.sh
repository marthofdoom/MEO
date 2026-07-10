#!/usr/bin/env bash
# Cut an immutable, MO2-installable MEO release built around the CI-built DLL.
#
# MEO.dll can only be built on Windows CI (CommonLibSSE-NG needs the MSVC ABI),
# so this pulls the DLL from a successful `native` workflow run and wraps it in
# the MO2 archive layout: the zip root IS the virtual Data folder, so MO2
# installs it with zero manual file placement — no dropping DLLs by hand.
#
# Layout produced (unzip = Data/):
#   SKSE/Plugins/MEO.dll                    (+ MEO.pdb if CI produced one)
#   SKSE/Plugins/MEO/meo_runtime.json
#   MEO.esp
#   MEO.Installer.exe                       (CI-built win-x64 post-install patcher)
#   MEO-README.txt                          (post-install steps)
#   fomod/info.xml                          (MO2 metadata card)
#
# Usage:
#   tools/release_native.sh <version> "desc" [--run <id>] [--installer-run <id>] [--esp] [--json]
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
INST_RUN_ID=""
# Every release is a COMPLETE, standalone mod (Marth's rule 2026-07-08: MO2
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
        --installer-run) INST_RUN_ID="${2:?--installer-run needs an id}"; shift 2 ;;
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

# Regenerate ALL DLL-adjacent content fresh so the zip is a complete, standalone
# mod every time (never rely on files left in a previous install).
python3 MEO_GenerateESP.py out            # MEO.esp + MCM/ + SKSE/Plugins/MEO/meo_runtime.json
tools/compile.sh MEO_MCM >/dev/null       # Scripts/MEO_MCM.pex (Papyrus, local Proton toolchain)

mkdir -p "$STAGE/SKSE/Plugins/MEO"
cp out/SKSE/Plugins/MEO/meo_runtime.json "$STAGE/SKSE/Plugins/MEO/"
# The installer derives per-list rider calibration from this catalog at
# post-install time (write-calibration); it must ship next to the exe.
cp data/gem_catalog.json "$STAGE/SKSE/Plugins/MEO/"
# m24 menu skins: three OFL typefaces (Cinzel head / EB Garamond body /
# Inter sans) + their licenses; the DLL bakes them at init, skins pick.
mkdir -p "$STAGE/SKSE/Plugins/MEO/fonts"
cp data/fonts/head.ttf data/fonts/body.ttf data/fonts/sans.ttf \
   data/fonts/OFL-*.txt "$STAGE/SKSE/Plugins/MEO/fonts/"
cp out/MEO.esp "$STAGE/"
mkdir -p "$STAGE/MCM";     cp -r out/MCM/. "$STAGE/MCM/"
mkdir -p "$STAGE/Scripts"; cp out/Scripts/MEO_MCM.pex "$STAGE/Scripts/"

# CI-built win-x64 installer exe (post-install patcher: perk tree + choices).
# Built on windows-latest from public source — the trust story for Nexus.
if [[ -z "$INST_RUN_ID" ]]; then
    INST_RUN_ID=$(gh run list --workflow=installer.yml --status success --limit 1 \
                  --json databaseId -q '.[0].databaseId')
    [[ -n "$INST_RUN_ID" ]] || { echo "ERROR: no successful 'installer' run found. Push installer/ and let CI go green first." >&2; exit 1; }
fi
echo "== installer exe from run $INST_RUN_ID =="
gh run download "$INST_RUN_ID" -n MEO-installer -D "$STAGE"
[[ -f "$STAGE/MEO.Installer.exe" ]] || { echo "ERROR: artifact had no MEO.Installer.exe" >&2; exit 1; }

# MO2 metadata card + post-install steps shipped inside the mod folder.
mkdir -p "$STAGE/fomod"
cat > "$STAGE/fomod/info.xml" <<EOF
<fomod>
  <Name>Marth's Enchanting Overhaul</Name>
  <Author>Marth</Author>
  <Version>$VER</Version>
  <Website>https://github.com/marthofdoom/MEO</Website>
  <Description>Socketable, leveling enchantment gems. After installing, run MEO.Installer.exe from this mod's folder to adapt the enchanting perk tree to your load order — see MEO-README.txt.</Description>
</fomod>
EOF
cat > "$STAGE/MEO-README.txt" <<'EOF'
Marth's Enchanting Overhaul (MEO)
=================================
Socketable, leveling enchantment gems.

POST-INSTALL (required once per modlist):
  1. Install this mod in MO2 and enable it (left pane) + MEO.esp (right pane).
  2. Open this mod's folder (right-click the mod -> Open in Explorer) and run
     MEO.Installer.exe. It reads YOUR load order -- nothing is hardcoded --
     replaces the enchanting perk tree's crafting perks with MEO's gem perks,
     and asks you, perk by perk, about anything else it finds in the tree
     (staff/wand perks etc.). Your answers are saved next to the patch in
     "MEO - Patch.choices.json"; edit or delete that file and re-run to change
     your mind. Re-run the installer after any modlist update.
  3. Enable the generated "MEO - Patch.esp" in MO2's right pane, near the end
     of your load order.

The installer never launches or touches the game -- it only reads MO2's own
files and writes one .esp. If Windows SmartScreen warns about an unrecognized
app: the exe is built by public GitHub CI from the sources at
https://github.com/marthofdoom/MEO (Details -> Run anyway).
EOF

# Completeness gate: refuse to write a release missing any required file.
for req in "SKSE/Plugins/MEO.dll" "MEO.esp" "Scripts/MEO_MCM.pex" \
           "MCM/Config/MEO/config.json" "MCM/Settings/MEO.ini" \
           "SKSE/Plugins/MEO/meo_runtime.json" "SKSE/Plugins/MEO/gem_catalog.json" \
           "SKSE/Plugins/MEO/fonts/head.ttf" "SKSE/Plugins/MEO/fonts/body.ttf" \
           "SKSE/Plugins/MEO/fonts/sans.ttf" \
           "MEO.Installer.exe" "fomod/info.xml" "MEO-README.txt"; do
    [[ -f "$STAGE/$req" ]] || { echo "ERROR: release incomplete — missing $req" >&2; exit 1; }
done

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
printf '%s\n' "$VER" > "$DEST/VERSION"
[[ -n "$DESC" ]] && printf '%s\n' "$DESC" > "$DEST/NOTES.txt"
printf 'built from native run %s, installer run %s\n' "$RUN_ID" "$INST_RUN_ID" >> "$DEST/NOTES.txt"

echo "== manifest (MO2 installs this as Data/) =="
unzip -l "$ZIP"
echo
echo "Wrote $ZIP"
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
