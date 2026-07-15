#!/usr/bin/env bash
# Cut an immutable, exe-free (Synthesis-only) MEO release.
#
# Since v1.0.5 the Nexus download ships NO binary tool — the install-time
# patcher is a Synthesis patcher added from GitHub (see releases/*/MEO.synth),
# so the release is: the mod zip (DLL + ESP + MCM + runtime + fonts) plus the
# static MEO.synth onboarding file. The DLL is pulled from a green `native` CI
# run (CommonLibSSE-NG only builds on Windows/MSVC). The precalibrated
# Vanilla-AE-CC variant is cut separately (needs a vanilla+AE+CC load order).
#
# Usage: tools/release_synthesis.sh <version> "desc" [--run <native-run-id>]
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:?usage: tools/release_synthesis.sh <version> [description] [--run <id>]}"; shift
DESC=""; RUN_ID=""
if [[ $# -gt 0 && "$1" != --* ]]; then DESC="$1"; shift; fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

DEST="releases/$VER"; ZIP="$DEST/MEO-$VER.zip"
[[ -e "$DEST" ]] && { echo "ERROR: $DEST already exists. Releases are immutable; bump or clear it." >&2; exit 1; }

# Resolve the CI run that built the DLL.
if [[ -z "$RUN_ID" ]]; then
    RUN_ID=$(gh run list --workflow=native.yml --status success --limit 1 --json databaseId -q '.[0].databaseId')
    [[ -n "$RUN_ID" ]] || { echo "ERROR: no successful 'native' run found." >&2; exit 1; }
fi
echo "== native DLL from run $RUN_ID =="

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/SKSE/Plugins/MEO/fonts"
gh run download "$RUN_ID" -n MEO-dll -D "$STAGE/SKSE/Plugins"
[[ -f "$STAGE/SKSE/Plugins/MEO.dll" ]] || { echo "ERROR: artifact had no MEO.dll" >&2; exit 1; }
echo "DLL: $(stat -c '%s bytes' "$STAGE/SKSE/Plugins/MEO.dll")"

# Regenerate all DLL-adjacent content fresh (complete standalone mod every time).
echo "== regenerate ESP + MCM + runtime json =="
python3 MEO_GenerateESP.py out
echo "== MCM papyrus script =="
if ! tools/compile.sh MEO_MCM >/dev/null 2>&1; then
    echo "  compile.sh unavailable — reusing existing out/Scripts/MEO_MCM.pex"
fi
[[ -f out/Scripts/MEO_MCM.pex ]] || { echo "ERROR: no MEO_MCM.pex (compile it once)" >&2; exit 1; }

cp out/SKSE/Plugins/MEO/meo_runtime.json "$STAGE/SKSE/Plugins/MEO/"
cp data/fonts/head.ttf data/fonts/body.ttf data/fonts/sans.ttf data/fonts/OFL-*.txt "$STAGE/SKSE/Plugins/MEO/fonts/"
cp out/MEO.esp "$STAGE/"
mkdir -p "$STAGE/MCM";     cp -r out/MCM/. "$STAGE/MCM/"
mkdir -p "$STAGE/Scripts"; cp out/Scripts/MEO_MCM.pex "$STAGE/Scripts/"
cp assets/MEO-README.txt "$STAGE/MEO-README.txt"

mkdir -p "$STAGE/fomod"
cat > "$STAGE/fomod/info.xml" <<EOF
<fomod>
  <Name>marth's Enchanting Overhaul</Name>
  <Author>marth</Author>
  <Version>$VER</Version>
  <Website>https://github.com/marthofdoom/MEO</Website>
  <Description>Socketable, leveling enchantment gems. Adapt to your load order with the MEO Synthesis patcher (see MEO.synth / MEO-README.txt).</Description>
</fomod>
EOF

# Completeness gate: refuse an incomplete release.
for req in "SKSE/Plugins/MEO.dll" "SKSE/Plugins/MEO/meo_runtime.json" \
           "SKSE/Plugins/MEO/fonts/head.ttf" "SKSE/Plugins/MEO/fonts/body.ttf" \
           "SKSE/Plugins/MEO/fonts/sans.ttf" "MEO.esp" \
           "MCM/Config/MEO/config.json" "MCM/Settings/MEO.ini" \
           "Scripts/MEO_MCM.pex" "fomod/info.xml" "MEO-README.txt"; do
    [[ -f "$STAGE/$req" ]] || { echo "ERROR: release incomplete — missing $req" >&2; exit 1; }
done

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
cp assets/MEO.synth "$DEST/MEO.synth"          # static Synthesis onboarding file
printf '%s\n' "$VER" > "$DEST/VERSION"
{ [[ -n "$DESC" ]] && printf '%s\n' "$DESC"; printf 'built from native run %s\n' "$RUN_ID"; } > "$DEST/NOTES.txt"

echo "== manifest (MO2 installs this as Data/) =="; unzip -l "$ZIP"
echo; echo "Wrote $ZIP  +  $DEST/MEO.synth"
echo "Precalibrated Vanilla-AE-CC variant: cut separately against a vanilla+AE+CC load order."
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
