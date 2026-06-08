#!/usr/bin/env bash
# ============================================================================
# conform.sh — Keel conformance harness (roadmap C2 / Tier 1.2).
#
# The conformance suite IS the executable specification. It is written against
# the CLI, not against any implementation's internals, so it runs UNCHANGED
# against the C oracle today and the self-hosted compiler later (Tier 4.3
# behavioral equivalence). Four case categories, exactly as C2 prescribes:
#
#   positive  — program runs; stdout (and exit code) match exactly.
#   negative  — program is REJECTED for a NAMED reason (error code on stderr).
#   golden    — token/AST dumps match (differential-testing fixtures, C4).
#   property  — formatter idempotence: fmt(fmt(x)) == fmt(x).
#
# Usage:
#   tests/conform.sh                         # uses ./bin/keel
#   KEEL=./bin/keel tests/conform.sh         # explicit interpreter
#   KEEL_RUN='mycompiler-run' tests/conform.sh   # any impl that runs a .keel
# ============================================================================
set -u
cd "$(dirname "$0")/.."

KEEL="${KEEL:-./bin/keel}"
# How to RUN a program to observable output. Override for the compiled path.
RUN="${KEEL_RUN:-$KEEL run}"
FMT="${KEEL_FMT:-$KEEL fmt}"
TOKENS="${KEEL_TOKENS:-$KEEL tokens}"
AST="${KEEL_AST:-$KEEL ast}"

pass=0; fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

red(){ printf '\033[31m%s\033[0m' "$1"; }
grn(){ printf '\033[32m%s\033[0m' "$1"; }

note(){ printf '  %-46s ' "$1"; }
ok(){   grn ok; echo; pass=$((pass+1)); }
bad(){  red FAIL; echo " — $1"; fail=$((fail+1)); }

echo "── positive (run; stdout + exit match) ──────────────────"
for f in tests/positive/*.keel; do
    [ -e "$f" ] || continue
    base="${f%.keel}"; name=$(basename "$base")
    note "$name"
    $RUN "$f" > "$tmp/out" 2>"$tmp/err"; got_exit=$?
    want_exit=0; [ -f "$base.exit" ] && want_exit=$(cat "$base.exit")
    if ! diff -q "$base.out" "$tmp/out" >/dev/null 2>&1; then
        bad "stdout differs"; diff "$base.out" "$tmp/out" | head -4
    elif [ "$got_exit" != "$want_exit" ]; then
        bad "exit $got_exit != $want_exit"
    else ok; fi
done

echo "── negative (rejected for a NAMED reason) ───────────────"
for f in tests/negative/*.keel; do
    [ -e "$f" ] || continue
    base="${f%.keel}"; name=$(basename "$base")
    note "$name"
    code=$(cat "$base.code")
    $RUN "$f" > "$tmp/out" 2>"$tmp/err"; got_exit=$?
    if [ "$got_exit" = 0 ]; then
        bad "expected rejection [$code], but it succeeded"
    elif grep -q "\[$code\]" "$tmp/err"; then
        ok
    else
        bad "expected code [$code]; stderr: $(head -1 "$tmp/err")"
    fi
done

echo "── golden (token + AST dumps; differential fixtures) ────"
for f in tests/golden/*.keel; do
    [ -e "$f" ] || continue
    base="${f%.keel}"; name=$(basename "$base")
    note "$name.tokens"
    $TOKENS "$f" > "$tmp/tok" 2>/dev/null
    if diff -q "$base.tokens" "$tmp/tok" >/dev/null 2>&1; then ok; else bad "token stream differs"; fi
    note "$name.ast"
    $AST "$f" > "$tmp/ast" 2>/dev/null
    if diff -q "$base.ast" "$tmp/ast" >/dev/null 2>&1; then ok; else bad "AST differs"; fi
done

echo "── property (formatter idempotence) ─────────────────────"
for f in tests/property/*.keel; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    note "fmt(fmt($name))==fmt($name)"
    $FMT "$f" > "$tmp/f1" 2>/dev/null
    $FMT "$tmp/f1" > "$tmp/f2" 2>/dev/null
    if diff -q "$tmp/f1" "$tmp/f2" >/dev/null 2>&1; then ok; else bad "not idempotent"; fi
done

echo "─────────────────────────────────────────────────────────"
echo "  $pass passed, $fail failed"
[ "$fail" = 0 ]
