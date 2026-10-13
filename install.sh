#!/usr/bin/env bash
# Build hextrix and install the binary into ~/.local/bin (usually on PATH).
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"

cd "$(dirname "$0")"

make

install -Dm755 hextrix "$BINDIR/hextrix"
echo "installed: $BINDIR/hextrix"
case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) echo "note: $BINDIR is not on PATH" ;;
esac
