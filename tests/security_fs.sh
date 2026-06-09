#!/usr/bin/env bash
# ============================================================================
# security.sh — capability containment under a REAL filesystem (roadmap 2.1).
#
# The conformance suite is hermetic and cannot create symlinks, directories, or
# files outside a capability root, so the filesystem-dependent half of the
# security model lives here. Each case sets up a real fixture tree, runs a Keel
# program whose `sys.fs` is rooted at that tree (the interpreter roots the root
# capability at the process CWD), and asserts the observable outcome. The same
# programs are also run as NATIVE COMPILED BINARIES so the interpreter and the
# compiled runtime are shown to contain identically (C5).
#
#   reads of in-root regular files succeed
#   a symlink inside the root pointing outside it is REJECTED (realpath escape)
#   `..` traversal above the root is REJECTED
#   an absolute path is reinterpreted relative to the root (never the real /)
#   reading a directory fails cleanly (no UB / no huge allocation)
# ============================================================================
set -u
cd "$(dirname "$0")/.."
ROOT_DIR="$(pwd)"
KEEL="$ROOT_DIR/bin/keel"

pass=0; fail=0
note(){ printf '  %-52s ' "$1"; }
ok(){   printf '\033[32mok\033[0m\n'; pass=$((pass+1)); }
bad(){  printf '\033[31mFAIL\033[0m — %s\n' "$1"; fail=$((fail+1)); }

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# fixture tree: $work/jail is the capability root
mkdir -p "$work/jail/sub"
echo "in-root contents" > "$work/jail/inside.txt"
echo "SECRET — must never be read through the capability" > "$work/outside_secret.txt"
ln -s "$work/outside_secret.txt" "$work/jail/escape_link"   # symlink inside root → outside
ln -s "$work/jail/inside.txt"    "$work/jail/inside_link"   # symlink inside root → inside (legal)

# Run a program under the interpreter AND as a compiled native binary, both with
# CWD = the jail, and require: same stdout, same exit, and that $forbidden never
# appears in the output. $1 name, $2 program, $3 expected-substring, $4 exit.
run_both(){
    local name="$2" prog="$3" want="$4" wantrc="$5"
    note "$name (interpreted)"
    printf '%s\n' "$prog" > "$work/p.keel"
    ( cd "$work/jail" && "$KEEL" run "$work/p.keel" ) > "$work/i.out" 2>&1; local irc=$?
    if grep -q "SECRET" "$work/i.out"; then bad "secret leaked!"; 
    elif ! grep -qF "$want" "$work/i.out"; then bad "want '$want'; got: $(head -1 "$work/i.out")";
    elif [ "$irc" != "$wantrc" ]; then bad "exit $irc != $wantrc";
    else ok; fi

    note "$name (compiled)"
    # The compiler reads its input through its own capability rooted at the repo,
    # so stage the source inside the repo, compile there, then run the resulting
    # native binary with CWD = the jail (the binary's own fs root).
    local stage="$ROOT_DIR/_sec_stage.keel"
    printf '%s\n' "$prog" > "$stage"
    if ( cd "$ROOT_DIR" && ./keelc-build.sh _sec_stage.keel "$work/cbin" ) >"$work/cgen.err" 2>&1; then
        ( cd "$work/jail" && "$work/cbin" ) > "$work/c.out" 2>&1; local crc=$?
        if grep -q "SECRET" "$work/c.out"; then bad "secret leaked (compiled)!";
        elif ! grep -qF "$want" "$work/c.out"; then bad "want '$want'; got: $(head -1 "$work/c.out")";
        elif [ "$crc" != "$wantrc" ]; then bad "exit $crc != $wantrc";
        else ok; fi
    else
        bad "codegen/compile failed: $(head -1 "$work/cgen.err")"
    fi
    rm -f "$stage"
}

echo "── capability containment (real filesystem) ─────────────"

run_both _ "legal in-root read" \
'def main(sys: System); Io:
    f = sys.fs.open("inside.txt")?
    println("READ:", f.read()?)' \
"READ: in-root contents" 0

run_both _ "symlink escape rejected" \
'def main(sys: System); Io:
    f = sys.fs.open("escape_link")?
    println("LEAK:", f.read()?)' \
"main failed: path traversal rejected" 70

run_both _ "in-root symlink allowed" \
'def main(sys: System); Io:
    f = sys.fs.open("inside_link")?
    println("READ:", f.read()?)' \
"READ: in-root contents" 0

run_both _ "dotdot traversal rejected" \
'def main(sys: System); Io:
    d = sys.fs.subtree("../..")
    println("escaped root")' \
"main failed: path traversal rejected" 70

run_both _ "directory read fails cleanly" \
'def main(sys: System); Io:
    f = sys.fs.open("sub")?
    println(f.read()?)' \
"main failed: not a regular file" 70

run_both _ "absolute path stays in root" \
'def main(sys: System); Io:
    f = sys.fs.open("/inside.txt")?
    println("READ:", f.read()?)' \
"READ: in-root contents" 0

echo "─────────────────────────────────────────────────────────"
echo "  $pass passed, $fail failed"
[ "$fail" = 0 ]
