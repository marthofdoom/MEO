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
#   SKSE/Plugins/MEO/meo_runtime.json       (only with --json)
#   MEO.esp                                 (only with --esp)
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
WITH_ESP=0
WITH_JSON=0
# First non-flag arg (if any) is the description.
if [[ $# -gt 0 && "$1" != --* ]]; then DESC="$1"; shift; fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run)  RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        --esp)  WITH_ESP=1; shift ;;
        --json) WITH_JSON=1; shift ;;
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

# Optional data catalog the DLL reads (regenerate first so it is current).
if [[ "$WITH_JSON" == 1 ]]; then
    python3 MEO_GenerateESP.py out
    mkdir -p "$STAGE/SKSE/Plugins/MEO"
    cp out/SKSE/Plugins/MEO/meo_runtime.json "$STAGE/SKSE/Plugins/MEO/"
fi

# Optional ESP (only builds that ship forms). Carries the MCM config +
# compiled MCM shim script, which are generated alongside the ESP.
if [[ "$WITH_ESP" == 1 ]]; then
    [[ -f out/MEO.esp ]] || python3 MEO_GenerateESP.py out
    cp out/MEO.esp "$STAGE/"
    if [[ -d out/MCM ]]; then
        mkdir -p "$STAGE/MCM"; cp -r out/MCM/. "$STAGE/MCM/"
    fi
    if [[ -f out/Scripts/MEO_MCM.pex ]]; then
        mkdir -p "$STAGE/Scripts"; cp out/Scripts/MEO_MCM.pex "$STAGE/Scripts/"
    fi
fi

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
printf '%s\n' "$VER" > "$DEST/VERSION"
[[ -n "$DESC" ]] && printf '%s\n' "$DESC" > "$DEST/NOTES.txt"
printf 'built from native run %s\n' "$RUN_ID" >> "$DEST/NOTES.txt"

echo "== manifest (MO2 installs this as Data/) =="
unzip -l "$ZIP"
echo
echo "Wrote $ZIP"
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
