#!/usr/bin/env bash
# Cut an immutable MEO release: regenerate ESP, compile scripts, assemble the
# MO2-installable package, zip it into releases/<version>/ (NEVER overwritten),
# and create a git tag. Older releases are never deleted.
#
# Usage: tools/release.sh v0.0.1-p0 "one-line description"
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:?usage: tools/release.sh <version> [description]}"
DESC="${2:-}"
DEST="releases/$VER"
ZIP="$DEST/MEO-$VER.zip"

if [[ -e "$DEST" ]]; then
    echo "ERROR: $DEST already exists. Releases are immutable; bump the version." >&2
    exit 1
fi

echo "== regenerate ESP =="
python3 MEO_GenerateESP.py out

echo "== compile scripts =="
tools/compile.sh all

echo "== assemble package =="
STAGE="$(mktemp -d)"
cp out/MEO.esp "$STAGE/"
mkdir -p "$STAGE/Scripts"
cp out/Scripts/*.pex "$STAGE/Scripts/"
mkdir -p "$STAGE/Scripts/Source"
cp Source/Scripts/MEO_*.psc "$STAGE/Scripts/Source/"   # ship our own sources only
# SEQ: P0's startup quest is Run Once, which starts without a SEQ file. When a
# not-Run-Once quest is added (P1 tracker/MCM), emit Data/SEQ/MEO.seq here.

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
printf '%s\n' "$VER" > "$DEST/VERSION"
[[ -n "$DESC" ]] && printf '%s\n' "$DESC" > "$DEST/NOTES.txt"
rm -rf "$STAGE"

echo "== manifest =="
unzip -l "$ZIP"
echo
echo "Wrote $ZIP"
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
