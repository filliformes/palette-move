#!/bin/bash
set -e
MODULE_ID="palette"
MOVE_HOST="${1:-${MOVE_HOST:-move.local}}"
DEST="/data/UserData/schwung/modules/audio_fx/$MODULE_ID"

echo "Installing $MODULE_ID to $MOVE_HOST..."
ssh ableton@"$MOVE_HOST" "mkdir -p $DEST"
scp "dist/$MODULE_ID/$MODULE_ID.so" "dist/$MODULE_ID/module.json" ableton@"$MOVE_HOST:$DEST/"
[ -f "dist/$MODULE_ID/help.json" ] && scp "dist/$MODULE_ID/help.json" ableton@"$MOVE_HOST:$DEST/" || true
echo "Done. Power-cycle the Move (or remove/re-add the FX) to pick up module.json changes."
