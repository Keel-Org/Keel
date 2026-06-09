# Keel — Design Notes (Part I)

This document describes the implementation: a C11 reference interpreter, a
self-hosting Keel→C compiler written in Keel, and the runtime they share. It
follows the bootstrap plan in §9 of the specification — *the reference
implementation brings the language up; the language is then rewritten in
itself.* Part I reaches the point where that rewrite is real: Keel compiles
Keel.

The guiding principle, taken from the spec, is **honesty**. Where a feature is
implemented faithfully, this document says so. Where the implementation uses a
runtime stand-in for something the finished compiler will prove statically, it
says that too, plainly.

## What exists

    src/keel.c              Stage-0 reference interpreter (the oracle)
    compiler/keelc.keel     the Keel→C compiler, written in Keel
    runtime/keelrt.{h,c}    runtime that compiled Keel programs link against
    runtime/keel_escape.h   the single-source context-string escapers (C5)
    tests/                  conformance suite + behavioral-equivalence corpus

The interpreter is invoked as `keel <run|test|fmt|tokens|ast|version> file`.
The compiler reads a `.keel` file (through a filesystem capability) and emits C
to standard output; `keelc-build.sh` drives it through `gcc`. Everything builds
with `make` against libc and libm only.

## The two implementations and the runtime

**Stage 0 — the interpreter (the oracle).** A tree-walking evaluator that is the
definition of correct behavior. Source → lexer → parser → AST → evaluator, with
a canonical formatter, a property-test runner, and token/AST dumps hanging off
the AST.

**The self-hosting compiler.** A complete Keel→C pipeline written in Keel:
`source → lex → tokens → parse → AST → codegen → C`. Its parser's
operator-precedence ladder matches the oracle's exactly; its code generator
emits C against the runtime below. It is written in the same subset it compiles,
which is what makes the bootstrap a genuine fixpoint rather than a demo.

**The runtime.** The value representation and operations that compiled programs
use. Each operation mirrors the interpreter's semantics, so a compiled binary is
observably equivalent to `keel run`. The escapers live in one shared header so
the interpreter and the runtime cannot diverge on the injection boundary.

## The pieces, and how honest each one is

### Lexer — faithful

Significant indentation becomes explicit `INDENT`/`DEDENT`/`NEWLINE` tokens via
an indentation stack, with newlines suppressed inside brackets. Numbers carry
their kind (integer, exact `decimal`, or float). Context-string tags
(`sql"…"`, `shell"…"`, `html"…"`, `url"…"`) are recognized at the first
character, which is what makes the typed-string guarantee possible. Diagnostics
carry byte spans and stable codes; the lexer reports a caret excerpt for a stray
character (`lex.char`) and recovers.

### Parser — faithful

A precedence-climbing parser produces the AST for the whole surface syntax:
`def` with a return type after `;` and effects in `{…}`; `struct`/`enum` with
payload-carrying variants; `derive(…)`; `check`/`is` pattern matching; the three
loop forms; `execute`/`handle`; refinement types (`int where >= 0`); generics;
ternary, `??`, `?.`, and postfix `?`. Because Keel is expression-oriented,
`execute`, `check`, and `if` parse in value position as well as statement
position. Parse errors recover and report multiple independent diagnostics with
the code `parse.error`.

One known limitation, shared by both parsers: a *grouping* parenthesis whose
`)` is immediately followed by a `:` (the end of an `if`/`loop`/`check` header)
is misread as a lambda parameter list. It is always avoidable through precedence
or a helper function, and is a candidate fix for Part II.

### Diagnostics, panic, and the Fail taxonomy — faithful

Errors recover where possible and are reported with spans, caret excerpts, and
stable codes. The runtime distinguishes two failure kinds, and the distinction
is load-bearing:

- A **type error** — a violation the type system is meant to make impossible: a
  plain string reaching a SQL sink (`type.sink`), an unknown method
  (`type.method`), a missing field (`type.field`), an undefined name
  (`name.undefined`). These are *uncatchable* aborts with exit code 70. Making
  them recoverable would model an impossible condition as a runtime concern.
- A **recoverable condition** — bad input, a missing file, a path escaping its
  root. These are catchable `Fail` values that a handler can recover.

The injection-sink rejection is uncatchable by design: a `Fail` handler cannot
turn `db.run(plain_string)` into a recovery path.

### Algebraic effects — faithful mechanism, one-shot resumption

A real effect mechanism using `setjmp`/`longjmp`: a dynamic stack of handlers
and a stack of perform-sites. Performing `Fail` finds the nearest matching
handler; a handler clause either falls off the end (aborting to the `execute`
site with its value) or evaluates `resume v` (returning to the perform site).
Resumptions are **one-shot and in-scope** — sufficient for the effects the spec
leans on (`Fail`, condition/restart, `Io`); full multi-shot continuations are
out of scope for Part I. The compiled runtime carries the same handler/boundary
model, so each compiled function installs a `Fail` boundary exactly as the
interpreter's call does.

### Capability-based authority — faithful, with real containment

`main(sys: System)` receives the one root capability; everything else is reached
by narrowing it. Capabilities are ordinary values; a function not handed a `Db`
cannot touch a database. Path containment is real and resolves symlinks: a
requested path is resolved lexically against the capability root (rejecting any
`..` that would pop above it), and then *canonicalized with `realpath`* — its
longest existing ancestor is resolved through symlinks and the result is
required to remain at or below the canonical root. This closes the symlink-escape
hole that pure lexical checking leaves open (a link inside the root pointing
outside it; cf. CWE-22/CWE-59), while a `..` escape and a symlink escape both
surface as a *catchable* `Fail`. Reads are restricted to regular files of bounded
size, so a directory or device handed to `open` fails cleanly rather than
mis-sizing the read. File reads perform real I/O within the root (the compiler
reads its own source this way). The database capability is still a test double
whose `run` executes a tiny query engine over in-memory rows — enough to
demonstrate the security properties without being a driver.

### Modules — faithful in shape, capability-honest

Imports resolve relative to the importing file and bind the imported
definitions. Import cycles are detected (`module.cycle`). Crucially, **no
effectful code runs at import time**: a module may only declare definitions, and
authority enters the program only at `main` (`module.toplevel` rejects top-level
effects). Importing a module cannot, therefore, smuggle in authority.

### Context-typed strings — faithful (the real guarantee)

`db.run(…)` accepts only a `sql"…"` value; a plain `string` is rejected.
Interpolating into `sql"…#id…"` escapes for SQL — doubling both the single quote
*and* the backslash, so the escape holds on MySQL/MariaDB default mode as well as
ANSI/standard-conforming dialects (quote-doubling alone is bypassable where a
backslash can escape the closing quote; cf. CWE-89). The same interpolation into
`html`/`shell`/`url` escapes for those contexts. The escapers are defined once,
in `runtime/keel_escape.h`, and included by both the interpreter and the
compiled runtime — so "escape for SQL" has a single definition and the two
implementations cannot drift. The injection class is closed structurally; true
parameter binding is the Part IV completion of the runtime escape.

### Trust and secrets — faithful

`Untrusted<T>` wraps network input and cannot flow into a refined or plain
target without passing through a validation boundary (parsing *is* the
validation). `Secret<T>` renders as `Secret(<redacted>)` everywhere except an
explicit `reveal()`.

### Refinement types — runtime checks, not an SMT proof

The most important stand-in. The spec discharges `int where >= 0` with an SMT
solver at compile time. The implementation **checks the predicate at runtime**,
at binding/construction/validation boundaries, and fails if it does not hold.
The observable behavior at the boundary matches, but it is a dynamic check, not
a static proof — it cannot reject a bad program before it runs. Part IV makes it
static.

### Checked arithmetic & exact decimals — faithful

Integer arithmetic traps on overflow rather than wrapping (the overflow-checked
builtins live in the shared header, so the interpreter and compiled code trap at
the same point). The trap covers the easily-missed boundary cases too —
`INT64_MIN / -1`, `INT64_MIN % -1`, and negating `INT64_MIN` would each be
undefined behavior in C (a SIGFPE or a silent wrap), so each is checked and
trapped instead. `decimal` is an exact scaled integer in the interpreter. Arrays
broadcast over scalars, and `range`/`..` bound the materialized length so a
pathological span fails cleanly instead of exhausting memory.

### Robustness — no crash on hostile input

A tree-walking interpreter recurses for nesting in its input, so deeply nested
expressions or unbounded recursion could exhaust the C stack — a SIGSEGV, which
is exactly the undefined behavior the language rules out in safe code. The lexer,
parser, and evaluator therefore measure live C-stack usage against a fraction of
the real `RLIMIT_STACK` and convert exhaustion into a clean diagnostic
(`runtime.recursion`, or a recoverable parse error). Probing the stack pointer
rather than a frame counter keeps the guard correct across the `longjmp`-based
effect unwinding. (Under an AddressSanitizer build, run with
`detect_stack_use_after_return=0`, since its heap "fake stack" defeats any
stack-pointer probe.)

### Memory model — deliberately not modeled at runtime

Keel's memory safety without a garbage collector is a compile-time discipline
(ownership, borrowing, regions, generational references). A tree-walker has
nothing to be safe about in that sense; the compiled runtime uses a **tracked
arena freed at exit** — the roadmap's explicitly time-boxed interim allocator.
Borrows evaluate to the referent; `region`/`scope` blocks execute. The ownership
system is the type checker's job, carried by the self-hosting compiler in a
later part. Part IV replaces the arena with ownership-directed allocation.

### Tests and the formatter — faithful

`test` and `test prop` are first-class; the property runner generates inputs,
runs ~100 cases, and shrinks counterexamples. `keel fmt` emits the one canonical
layout, is idempotent (`fmt(fmt(x)) == fmt(x)`), and is *semantics-preserving*:
the conformance suite additionally requires `run(fmt(x)) == run(x)` for every
positive program, so the formatter may not silently change a result (it preserves
precedence parentheses, loop init/step clauses, and quoted import paths).

The suite is the executable specification, and it is organized by the **quality
goal** each test protects rather than by the mechanism that checks it, so a
single scorecard answers "are we covered on security?" at a glance. `make check`
runs every category and prints that scorecard. The categories: **correctness**
(feature programs whose stdout/exit match, golden token/AST fixtures, and parse/
type/name/refinement rejections each naming an error code); **safety** (the
UB-class traps — overflow, division, `INT64_MIN` edges, bounds, recursion/nesting
depth, range size, shift range — each rejected with a clean code); **security**
(injection-sink rejection, trust/secret redaction, and capability containment
over a *real* filesystem — symlink escape, `..` traversal, directory reads,
absolute paths — checked on both the interpreter and native-compiled binaries);
**reliability** (graceful rejection of malformed input — lexer edges, module
cycle/top-level — and the unhandled-`Fail` taxonomy); **performance**
(order-of-magnitude budgets that catch a complexity regression such as amortized
array growth degrading to O(n²), plus guards-fail-fast assertions);
**determinism** (the formatter is idempotent *and* semantics-preserving over
every runnable program, every Keel-Core program is byte-identical interpreted vs
compiled, and the self-hosting bootstrap reaches a fixpoint); and **tooling**
(`fmt`/`tokens`/`ast` and the in-language `test` runner). `make harden`/`make
fuzz` build under AddressSanitizer + UBSan and mutation-fuzz the front end; the
corpus and the fuzzer run clean, which is the evidence behind the
no-undefined-behavior claim.

## The self-hosting compiler

The compiler is the centerpiece of Part I. It targets **Keel-Core**, the
self-hosting subset (see PART1.md for the full scope), and is written in that
subset. Its design choices:

- **Codegen targets the tagged-value runtime.** Each Keel function becomes a C
  function returning the runtime's `kl` value type; each expression becomes a
  runtime call. The semantics are exact; an unboxed, monomorphized
  representation is Part IV's concern.
- **Lexical scoping is mirrored with C scoping** plus a declare-on-first-binding
  scope stack, so re-binding an outer mutable name is an update and loop-body
  bindings are fresh per iteration.
- **Output is deterministic** — ordered traversal and a monotonic gensym
  counter, with no iteration over unordered structures — which is what lets the
  bootstrap reach a byte-identical fixpoint.
- **The full operator set is in Keel-Core**, including the bitwise operators the
  spec mandates (`& | ^ ~ << >>`): the lexer, the precedence table, and unary
  `~` codegen all mirror the oracle, so a program using bit operations is
  accepted and produces identical results whether interpreted or compiled.
  Shifts trap a count outside `[0, 63]` and the left shift is computed on an
  unsigned value, so there is no shift-related undefined behavior in either
  implementation (per CERT INT34-C).

Two Keel-Core rules make the interpreter and compiled code agree: **parameters
are immutable** (mutate via a `mut` local), and **names are not shadowed across
nested blocks**. Both are stated and motivated in PART1.md.

### Interpreter performance for self-hosting

Running a ~1300-line compiler on a tree-walker surfaced two quadratic costs that
are now fixed, because they were real inefficiencies rather than inherent:
arrays grow with amortized doubling (a capacity field), and each loop iteration
gets a fresh scope so that re-binding an immutable name inside a loop no longer
accumulates bindings in one environment. With these, the full bootstrap runs in
about a second.

## The bootstrap story — completed

`make bootstrap` builds the chain and asserts the fixpoint:

    Stage 0  bin/keel                       the C oracle
    Stage 1  oracle runs keelc.keel  →  stage1.c → keelc1
    Stage 2  keelc1 runs keelc.keel  →  stage2.c → keelc2
    Stage 3  keelc2 runs keelc.keel  →  stage3.c

The compiler is self-hosting because **`stage2.c == stage3.c`** byte for byte;
additionally **`stage1.c == stage2.c`**, so the interpreter and the compiled
compiler emit identical code. `make equiv` then cross-checks that programs in
the Keel-Core corpus produce identical output whether interpreted by the oracle
or compiled by `keelc` — with `keelc` running interpreted *and* as a native
binary.

## What is not implemented

Content-addressed code (§3.16) and comptime metaprogramming (§3.7) are not
realized (`comptime` parses and evaluates; there is no staged compilation). FFI
parses to no-ops. The interpreter itself does no AOT compilation — that is the
self-hosting compiler's job, and the parts of it that Part I delivers compile
Keel-Core to native code via the runtime.

## Building and running

    make            # build bin/keel (the oracle)
    make check      # whole suite, scored by quality goal (alias: all-checks)

The suite is organized by the quality goal each test protects, and `make check`
prints a per-category scorecard so coverage is legible at a glance. Run one
category in isolation with its name:

    make correctness   # the language does what the spec says
    make safety        # no undefined behavior in safe code (overflow/bounds/recursion)
    make security      # capabilities, injection sinks, trust boundary (incl. compiled)
    make reliability   # graceful handling of malformed input; the failure taxonomy
    make performance   # complexity-regression budgets; guards fail fast
    make determinism   # formatter idempotence+semantics; oracle≡compiled; bootstrap
    make tooling       # fmt/tokens/ast and the in-language test runner

    make bootstrap  # build the self-hosting chain, assert the fixpoint
    make harden     # build + exercise the corpus under ASan/UBSan
    make fuzz       # mutation-fuzz the front end under sanitizers
    # (compatibility aliases: conform→correctness, equiv→determinism, perf→performance)

    ./bin/keel run    tests/determinism/equiv/recursion.keel
    ./bin/keel tokens tests/determinism/equiv/patterns.keel
    ./bin/keel ast    tests/determinism/equiv/patterns.keel
    ./keelc-build.sh  tests/determinism/equiv/recursion.keel /tmp/rec && /tmp/rec

