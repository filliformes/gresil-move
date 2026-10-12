#!/bin/bash
# Install Gresil to Ableton Move via SSH
set -e

MOVE_IP="${1:-move.local}"
MODULE_DIR="$(cd "$(dirname "$0")/.." && pwd)/modules/gresil"

if [ ! -f "$MODULE_DIR/gresil.so" ]; then
    echo "Error: gresil.so not found. Run build.sh first."
    exit 1
fi

echo "Installing Gresil to $MOVE_IP..."
DEST="/data/UserData/schwung/modules/audio_fx/gresil"
ssh root@$MOVE_IP "mkdir -p $DEST"
scp -r "$MODULE_DIR/." root@$MOVE_IP:$DEST/
ssh root@$MOVE_IP "chmod +x $DEST/gresil.so && chown -R ableton:users $DEST"

echo "Done. Power-cycle the Move to pick up module.json changes."
