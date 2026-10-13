#!/usr/bin/env bash
# Remove the hextrix binary installed by install.sh.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"

if rm -f "$BINDIR/hextrix"; then
    echo "removed: $BINDIR/hextrix"
else
    echo "nothing to remove at $BINDIR/hextrix" >&2
    exit 1
fi
