#!/usr/bin/env bash
# ============================================================================
# run.sh — the Keel test driver, organized by QUALITY GOAL.
#
# The suite is the executable specification. Tests are grouped by the property
# they protect, not by the mechanism that checks them, so "are we covered on
# security?" is answerable at a glance from the scorecard. Each category bundles
# whatever mechanisms it needs (run-and-diff, reject-with-code, golden dumps,
# format properties, oracle-vs-compiled equivalence, real-filesystem
# containment, complexity budgets).
#
#   correctness  — the language does what the spec says
#   safety       — no undefined behavior in safe code (overflow/bounds/recursion)
#   security     — capabilities, injection sinks, trust boundary
#   reliability  — graceful handling of malformed input; the failure taxonomy
#   performance  — complexity-regression budgets; guards fail fast
#   determinism  — formatter is idempotent & semantics-preserving; oracle ≡
#                  compiled; the self-hosting bootstrap reaches a byte fixpoint
#   tooling      — fmt/tokens/ast and the in-language test runner behave
#
# Usage:
#   tests/run.sh                 # every category + scorecard
#   tests/run.sh security        # one or more named categories
#   KEEL=./bin/keel tests/run.sh # explicit interpreter
# ============================================================================
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KEEL="${KEEL:-./bin/keel}"
RUN="${KEEL_RUN:-$KEEL run}"
FMT="${KEEL_FMT:-$KEEL fmt}"
TOKENS="${KEEL_TOKENS:-$KEEL tokens}"
AST="${KEEL_AST:-$KEEL ast}"

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
red(){ printf '\033[31m%s\033[0m' "$1"; }; grn(){ printf '\033[32m%s\033[0m' "$1"; }
note(){ printf '  %-50s ' "$1"; }
ok(){   grn ok; echo; CP=$((CP+1)); }
bad(){  red FAIL; echo " — $1"; CF=$((CF+1)); }

# category accumulators (filled per category, then folded into the scorecard)
declare -A CAT_PASS CAT_FAIL
CP=0; CF=0                       # current-category pass/fail

start_cat(){ CP=0; CF=0; printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
end_cat(){ CAT_PASS["$1"]=$CP; CAT_FAIL["$1"]=$CF; }

# ---- shared mechanisms ------------------------------------------------------
# program runs; stdout (and exit) match the recorded expectation
ck_run(){ for f in "$1"/*.keel; do [ -e "$f" ] || continue
    local b="${f%.keel}" n; n=$(basename "$b"); note "run $n"
    $RUN "$f" >"$tmp/o" 2>"$tmp/e"; local rc=$?
    local want=0; [ -f "$b.exit" ] && want=$(cat "$b.exit")
    if ! diff -q "$b.out" "$tmp/o" >/dev/null 2>&1; then bad "stdout differs"
    elif [ "$rc" != "$want" ]; then bad "exit $rc != $want"; else ok; fi
done; }
# program is REJECTED for a NAMED reason (error code on stderr)
ck_reject(){ for f in "$1"/*.keel; do [ -e "$f" ] || continue
    local b="${f%.keel}" n; n=$(basename "$b"); note "reject $n [$(cat "$b.code")]"
    local code; code=$(cat "$b.code")
    $RUN "$f" >"$tmp/o" 2>"$tmp/e"; local rc=$?
    if [ "$rc" = 0 ]; then bad "expected [$code], succeeded"
    elif grep -q "\[$code\]" "$tmp/e"; then ok
    else bad "want [$code]; got: $(head -1 "$tmp/e")"; fi
done; }
# token + AST differential fixtures
ck_golden(){ for f in "$1"/*.keel; do [ -e "$f" ] || continue
    local b="${f%.keel}" n; n=$(basename "$b")
    note "golden $n.tokens"; $TOKENS "$f" >"$tmp/t" 2>/dev/null
    diff -q "$b.tokens" "$tmp/t" >/dev/null 2>&1 && ok || bad "tokens differ"
    note "golden $n.ast"; $AST "$f" >"$tmp/a" 2>/dev/null
    diff -q "$b.ast" "$tmp/a" >/dev/null 2>&1 && ok || bad "AST differs"
done; }
# every Keel-Core program: oracle output == native-compiled output
ck_equiv(){ local CC="gcc -std=c11 -w -Iruntime" RT="runtime/keelrt.c"
  local KEELC_CMD="${KEELC:-$KEEL run compiler/keelc.keel}"
  for f in "$1"/*.keel; do [ -e "$f" ] || continue
    local n; n=$(basename "$f" .keel); note "equiv $n"
    "$KEEL" run "$f" >"$tmp/orc" 2>&1; local oe=$?
    if ! $KEELC_CMD "$f" >"$tmp/c.c" 2>"$tmp/cg"; then bad "codegen: $(head -1 "$tmp/cg")"; continue; fi
    if ! $CC -o "$tmp/cb" "$tmp/c.c" $RT -lm 2>"$tmp/cc"; then bad "gcc: $(head -1 "$tmp/cc")"; continue; fi
    "$tmp/cb" >"$tmp/cmp" 2>&1; local ce=$?
    { diff -q "$tmp/orc" "$tmp/cmp" >/dev/null && [ "$oe" = "$ce" ]; } && ok || bad "oracle != compiled"
done; }
# formatter is idempotent AND preserves semantics, over every runnable program
ck_format(){ for d in tests/correctness/run tests/security/run tests/reliability/run; do
  for f in "$d"/*.keel; do [ -e "$f" ] || continue
    local b="${f%.keel}" n; n=$(basename "$b")
    note "fmt idempotent $n"
    $FMT "$f" >"$tmp/f1" 2>/dev/null; $FMT "$tmp/f1" >"$tmp/f2" 2>/dev/null
    diff -q "$tmp/f1" "$tmp/f2" >/dev/null 2>&1 && ok || bad "not idempotent"
    note "fmt preserves run($n)"
    local want=0; [ -f "$b.exit" ] && want=$(cat "$b.exit")
    local fmtd="$d/.fmtcheck.keel"            # beside original: relative imports resolve
    $FMT "$f" >"$fmtd" 2>/dev/null
    $RUN "$fmtd" >"$tmp/fo" 2>/dev/null; local fe=$?
    rm -f "$fmtd"
    if ! diff -q "$b.out" "$tmp/fo" >/dev/null 2>&1; then bad "run(fmt) stdout differs"
    elif [ "$fe" != "$want" ]; then bad "run(fmt) exit $fe != $want"; else ok; fi
  done
done; }
# fold an external runner's "N passed, M failed" line into the scorecard
ck_external(){ local label="$1"; shift
    note "$label"
    local out; out=$("$@" 2>&1); local rc=$?
    local line; line=$(printf '%s\n' "$out" | grep -E '[0-9]+ passed, [0-9]+ failed' | tail -1)
    local p f; p=$(echo "$line" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+')
    f=$(echo "$line" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+')
    p=${p:-0}; f=${f:-0}
    CP=$((CP+p)); CF=$((CF+f))
    if [ "$f" = 0 ] && [ "$rc" = 0 ]; then grn "ok"; printf ' (%s passed)\n' "$p"
    else red "FAIL"; printf ' (%s passed, %s failed)\n' "$p" "$f"; fi
}

# ---- categories -------------------------------------------------------------
want(){ [ $# -eq 0 ] && return 0; for w in "$@"; do [ "$w" = "$SEL" ] && return 0; done; return 1; }
SEL_ALL=("$@")
run_cat(){ local c="$1"; [ ${#SEL_ALL[@]} -eq 0 ] && return 0; for s in "${SEL_ALL[@]}"; do [ "$s" = "$c" ] && return 0; done; return 1; }

do_correctness(){ start_cat correctness; ck_run tests/correctness/run; ck_golden tests/correctness/golden; ck_reject tests/correctness/reject; end_cat correctness; }
do_safety(){      start_cat safety;      ck_reject tests/safety/reject;  end_cat safety; }
do_security(){    start_cat security;    ck_run tests/security/run; ck_reject tests/security/reject; ck_external "filesystem containment (interp+compiled)" ./tests/security_fs.sh; end_cat security; }
do_reliability(){ start_cat reliability; ck_run tests/reliability/run; ck_reject tests/reliability/reject; end_cat reliability; }
do_performance(){ start_cat performance; ck_external "complexity budgets + guards-fail-fast" ./tests/perf.sh; end_cat performance; }
do_determinism(){ start_cat determinism; ck_equiv tests/determinism/equiv; ck_format
    note "bootstrap fixpoint (stage2.c == stage3.c)"
    if ./bootstrap.sh >"$tmp/bs" 2>&1 && grep -q "FIXPOINT REACHED" "$tmp/bs"; then ok; else bad "fixpoint not reached"; fi
    end_cat determinism; }
do_tooling(){ start_cat tooling
    note "in-language test runner (prop shrinking demo)"
    # 05_tests intentionally contains one false property to exercise shrinking,
    # so a non-zero exit is expected; we require it RUNS and reports a failure.
    if $KEEL test tests/tooling/05_tests.keel 2>&1 | grep -qE "[0-9]+ passed"; then ok; else bad "test runner did not report results"; fi
    for tool in fmt tokens ast; do note "$tool smoke (well-formed program)"
        $KEEL $tool tests/correctness/run/10_arithmetic.keel >/dev/null 2>&1 && ok || bad "$tool failed"; done
    end_cat tooling; }

ALL=(correctness safety security reliability performance determinism tooling)
for c in "${ALL[@]}"; do
    if [ ${#SEL_ALL[@]} -eq 0 ] || run_cat "$c"; then do_$c; fi
done

# ---- scorecard --------------------------------------------------------------
printf '\n\033[1m── scorecard (by quality goal) ──────────────────────────\033[0m\n'
TP=0; TF=0
for c in "${ALL[@]}"; do
    [ -z "${CAT_PASS[$c]+x}" ] && continue
    p=${CAT_PASS[$c]}; f=${CAT_FAIL[$c]}; TP=$((TP+p)); TF=$((TF+f))
    if [ "$f" = 0 ]; then printf '  %-14s \033[32m%3d/%-3d ok\033[0m\n' "$c" "$p" "$((p+f))"
    else printf '  %-14s \033[31m%3d/%-3d (%d FAILED)\033[0m\n' "$c" "$p" "$((p+f))" "$f"; fi
done
printf '  %s\n' "─────────────────────────────────────────────"
if [ "$TF" = 0 ]; then printf '  \033[32mTOTAL %d/%d passed\033[0m\n' "$TP" "$((TP+TF))"
else printf '  \033[31mTOTAL %d/%d passed — %d FAILED\033[0m\n' "$TP" "$((TP+TF))" "$TF"; fi
[ "$TF" = 0 ]
