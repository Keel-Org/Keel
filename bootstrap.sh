#!/usr/bin/env bash
# ============================================================================
# bootstrap.sh — build the self-hosting chain and assert the fixpoint (4.3).
#
#   Stage 0 : the C reference interpreter (bin/keel) — the oracle.
#   Stage 1 : oracle runs keelc.keel to compile keelc.keel -> stage1.c -> keelc1
#   Stage 2 : keelc1 (native) compiles keelc.keel            -> stage2.c -> keelc2
#   Stage 3 : keelc2 compiles keelc.keel                     -> stage3.c
#
# A compiler is self-hosting when it reproduces itself: stage2.c == stage3.c,
# byte for byte. We additionally check stage1.c == stage2.c, which shows the
# interpreter and the compiled compiler agree on the generated code as well.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

KEEL=bin/keel
KEELC=compiler/keelc.keel
RT="runtime/keelrt.c"
CC="gcc -std=c11 -w -Iruntime"
mkdir -p build

echo "Stage 0: the C reference interpreter (oracle)"
[ -x "$KEEL" ] || { echo "  build bin/keel first (make)"; exit 1; }

echo "Stage 1: oracle compiles keelc -> stage1.c -> keelc1"
$KEEL run "$KEELC" "$KEELC" > build/stage1.c
$CC -o build/keelc1 build/stage1.c $RT -lm

echo "Stage 2: keelc1 compiles keelc -> stage2.c -> keelc2"
./build/keelc1 "$KEELC" > build/stage2.c
$CC -o build/keelc2 build/stage2.c $RT -lm

echo "Stage 3: keelc2 compiles keelc -> stage3.c"
./build/keelc2 "$KEELC" > build/stage3.c

echo
echo "Fixpoint:"
if cmp -s build/stage2.c build/stage3.c; then
    echo "  stage2.c == stage3.c   — SELF-HOSTING FIXPOINT REACHED"
else
    echo "  stage2.c != stage3.c   — FIXPOINT NOT REACHED"; diff build/stage2.c build/stage3.c | head; exit 1
fi
if cmp -s build/stage1.c build/stage2.c; then
    echo "  stage1.c == stage2.c   — interpreter and compiled compiler agree"
else
    echo "  stage1.c != stage2.c   — (fixpoint still holds from stage2 on)"
fi
echo
echo "  sha: $(sha256sum build/stage3.c | cut -d' ' -f1)"
