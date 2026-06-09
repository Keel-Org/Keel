#!/usr/bin/env bash
# ============================================================================
# perf.sh — performance & stress gate.
#
# Not a microbenchmark: each case asserts an *order-of-magnitude* budget chosen
# well above the measured time on a reference machine, so normal variance never
# trips it but a complexity regression (e.g. amortized array growth silently
# becoming O(n^2), or per-iteration scope leaking) does. It also asserts that the
# safety guards (recursion, range bound) FAIL FAST rather than hang.
#
# Budgets are deliberately loose; the signal is "still polynomial-of-the-right-
# degree", not absolute speed.
# ============================================================================
set -u
cd "$(dirname "$0")/.."
KEEL="${KEEL:-./bin/keel}"

pass=0; fail=0
note(){ printf '  %-40s ' "$1"; }
ok(){   printf '\033[32mok\033[0m  (%ss, budget %ss)\n' "$1" "$2"; pass=$((pass+1)); }
bad(){  printf '\033[31mFAIL\033[0m — %s\n' "$1"; fail=$((fail+1)); }

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# run program $2 with wall-clock budget $3 seconds; require exit $4 (default 0).
budget(){
    local name="$1" prog="$2" lim="$3" wantrc="${4:-0}"
    note "$name"
    printf '%s\n' "$prog" > "$work/p.keel"
    local t0 t1 dt rc
    t0=$(date +%s.%N)
    timeout "$lim" "$KEEL" run "$work/p.keel" > "$work/out" 2>&1; rc=$?
    t1=$(date +%s.%N)
    dt=$(echo "$t1 - $t0" | bc)
    if [ "$rc" = 124 ]; then bad "TIMED OUT (> ${lim}s) — likely a complexity regression"; return; fi
    if [ "$rc" != "$wantrc" ]; then bad "exit $rc != $wantrc: $(tail -1 "$work/out")"; return; fi
    ok "$(printf '%.2f' "$dt")" "$lim"
}

echo "── throughput (loops / calls / scope) ───────────────────"

budget "sum 1,000,000 (per-iteration scope)" 'def main(sys: System); Io:
    mut s = 0
    loop till false; mut i = 0; i++:
        if i >= 1000000:
            break
        s = s + i
    println(s)' 12

budget "array push 200,000 (amortized growth)" 'def main(sys: System); Io:
    mut a = [0]
    loop till false; mut i = 0; i++:
        if i >= 200000:
            break
        a = a.push(i)
    println(a.len())' 6

budget "fib(28) recursion (call overhead)" 'def fib(n: int); int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
def main(sys: System); Io:
    println(fib(28))' 8

budget "nested 1000x1000 (1e6 inner)" 'def main(sys: System); Io:
    mut c = 0
    loop till false; mut i = 0; i++:
        if i >= 1000:
            break
        loop till false; mut j = 0; j++:
            if j >= 1000:
                break
            c = c + 1
    println(c)' 8

budget "string build 50,000 (push+join idiom)" 'def main(sys: System); Io:
    mut parts = [""]
    loop till false; mut i = 0; i++:
        if i >= 50000:
            break
        parts = parts.push("x")
    println(parts.join("").len())' 5

budget "array map+filter+sum over 100,000" 'def main(sys: System); Io:
    xs = range(0, 100000)
    println(xs.map((x): x * 2).filter((y): y % 3 == 0).sum())' 10

echo "── safety guards fail fast (must not hang) ──────────────"

budget "deep recursion → clean abort, fast" 'def f(n: int); int:
    if n <= 0:
        return 0
    return f(n - 1)
def main(sys: System); Io:
    println(f(100000000))' 8 70

budget "huge range → clean abort, fast" 'def main(sys: System); Io:
    println(range(0, 9999999999))' 5 70

echo "─────────────────────────────────────────────────────────"
echo "  $pass passed, $fail failed"
[ "$fail" = 0 ]
