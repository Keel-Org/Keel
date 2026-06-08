#!/usr/bin/env bash
# keelc-build.sh — compile a .keel file to a native binary using keelc.
#   $1 = keel source, $2 = output binary, $3 = the keelc front-end command
# KEELC defaults to running the Keel-written compiler on the oracle.
set -e
cd "$(dirname "$0")"
SRC="$1"; OUT="$2"
KEELC="${KEELC:-./bin/keel run compiler/keelc.keel}"
CFILE="$(mktemp /tmp/keelc.XXXXXX.c)"
$KEELC "$SRC" > "$CFILE"
if ! grep -q "int main" "$CFILE"; then
    echo "keelc-build: compiler produced no main() for $SRC" >&2
    cat "$CFILE" >&2
    exit 1
fi
gcc -std=c11 -w -Iruntime -o "$OUT" "$CFILE" runtime/keelrt.c -lm
rm -f "$CFILE"
