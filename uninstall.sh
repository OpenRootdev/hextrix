#!/bin/sh
# Hextrix uninstaller. Works piped or from a checkout:
#   curl -fsSL https://raw.githubusercontent.com/OpenRootdev/hextrix/main/uninstall.sh | sh
set -eu

PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"

rm -f "$BINDIR/hextrix"
echo "hextrix: removed $BINDIR/hextrix"
