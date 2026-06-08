#!/usr/bin/env bash
set -euo pipefail
# Pre-render all AdLib sound effects at native OPL quality into the
# standalone sfxcache.ofx pack (data slot 26).  The port looks sounds up by content hash and
# falls back to on-device synthesis, so the cache is optional & never stale.
#
# Usage: scripts/sfxcache.sh <dir>
#   <dir> contains the game data (AUDIOHED.*/AUDIOT.*),
#   e.g. /run/media/<user>/<CARD>/Assets/wolfenstein/common

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:?usage: sfxcache.sh <game-data dir>}"
TOOL="$ROOT/.obj/sfxcache"
SRC="$ROOT/src/tools/sfxcache/sfxcache.cpp"
DBOPL="$ROOT/src/wolfenstein/port/dosbox/dbopl.cpp"

mkdir -p "$ROOT/.obj"
if [ ! -x "$TOOL" ] || [ "$SRC" -nt "$TOOL" ] || [ "$DBOPL" -nt "$TOOL" ] || [ "$ROOT/src/tools/sfxcache/nuked/opl3.c" -nt "$TOOL" ]; then
    echo "sfxcache: building native tool..."
    g++ -O2 -DUSE_GPL \
        -I "$ROOT/src/tools/sfxcache/shim" \
        -I "$ROOT/src/tools/sfxcache/nuked" \
        -I "$ROOT/src/wolfenstein/port/dosbox" \
        -o "$TOOL" "$SRC" "$DBOPL" \
        "$ROOT/src/tools/sfxcache/nuked/opl3.c"
fi

# Collect AUDIOHED/AUDIOT pairs (any extension: WL6, WL1, SOD, SD2, SD3, N3D)
pairs=()
shopt -s nullglob nocaseglob
for hed in "$DIR"/AUDIOHED.*; do
    ext="${hed##*.}"
    for t in "$DIR"/AUDIOT.*; do
        [ "${t##*.}" = "$ext" ] && pairs+=("$hed" "$t")
    done
done
shopt -u nullglob nocaseglob
if [ ${#pairs[@]} -eq 0 ]; then
    echo "sfxcache: no AUDIOHED/AUDIOT pairs found in $DIR" >&2
    exit 1
fi

OFX="$DIR/sfxcache.ofx"
"$TOOL" "$OFX" "${pairs[@]}"

# Keep the release copy in sync so builds/packages ship the same pack.
DIST_OFX="$ROOT/dist/wolfenstein/Assets/wolfenstein/common/sfxcache.ofx"
if [ "$OFX" != "$DIST_OFX" ]; then
    cp "$OFX" "$DIST_OFX"
    echo "sfxcache: synced $DIST_OFX"
fi
