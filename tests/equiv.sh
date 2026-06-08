#!/usr/bin/env bash
# ============================================================================
# equiv.sh — behavioral equivalence (Tier 4.3).
#
# For every Keel-Core program in tests/compiled/, run it two ways and require
# identical stdout+exit:
#   (1) `keel run prog.keel`          — the Stage-0 interpreter (oracle)
#   (2) keelc compiles prog -> C -> native binary, then run it
# This is the cross-check that the self-hosted compiler agrees with the
# reference semantics on the self-hosting subset.
#
# KEELC selects the compiler front-end (default: keelc.keel on the oracle).
# Pass build/keelc1 to check the *compiled* compiler instead.
# ============================================================================
set -u
cd "$(dirname "$0")/.."

KEEL=bin/keel
CC="gcc -std=c11 -w -Iruntime"
RT="runtime/keelrt.c"
KEELC_CMD="${KEELC:-$KEEL run compiler/keelc.keel}"

pass=0; fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

for src in tests/compiled/*.keel; do
    name=$(basename "$src" .keel)
    printf '  %-14s ' "$name"
    # interpreter
    "$KEEL" run "$src" > "$tmp/oracle.out" 2>&1; oexit=$?
    # compile + run
    if ! $KEELC_CMD "$src" > "$tmp/c.c" 2>"$tmp/cgen.err"; then
        echo "FAIL (codegen): $(head -1 "$tmp/cgen.err")"; fail=$((fail+1)); continue
    fi
    if ! $CC -o "$tmp/bin" "$tmp/c.c" $RT -lm 2>"$tmp/cc.err"; then
        echo "FAIL (gcc): $(head -1 "$tmp/cc.err")"; fail=$((fail+1)); continue
    fi
    "$tmp/bin" > "$tmp/comp.out" 2>&1; cexit=$?
    if diff -q "$tmp/oracle.out" "$tmp/comp.out" >/dev/null && [ "$oexit" = "$cexit" ]; then
        echo "identical"; pass=$((pass+1))
    else
        echo "DIFFER"; fail=$((fail+1))
        diff "$tmp/oracle.out" "$tmp/comp.out" | head -6
    fi
done

echo "  ----------------------------------------"
echo "  $pass identical, $fail differing"
[ "$fail" = 0 ]
