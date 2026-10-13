#!/bin/sh
# Hextrix installer. Works from a checkout or piped:
#   curl -fsSL https://raw.githubusercontent.com/atif-1402/hextrix/main/install.sh | sh
set -eu

PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"
REPO="https://github.com/atif-1402/hextrix.git"

SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || true)
CLONE=""
if [ -z "$SRC_DIR" ] || [ ! -f "$SRC_DIR/Makefile" ] || [ ! -f "$SRC_DIR/src/main.c" ]; then
    command -v git >/dev/null 2>&1 || { echo "install.sh: git is required" >&2; exit 1; }
    CLONE=$(mktemp -d)
    echo "hextrix: cloning $REPO"
    git clone --depth 1 "$REPO" "$CLONE"
    SRC_DIR="$CLONE"
fi

command -v make >/dev/null 2>&1 || { echo "install.sh: make is required" >&2; exit 1; }

make -C "$SRC_DIR"
install -Dm755 "$SRC_DIR/hextrix" "$BINDIR/hextrix"
if [ -n "$CLONE" ]; then
    rm -rf "$CLONE"
fi

echo "hextrix: installed $BINDIR/hextrix"
case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) echo "hextrix: note: $BINDIR is not on PATH" ;;
esac
