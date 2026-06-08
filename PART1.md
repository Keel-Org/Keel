# Keel — Part I: a self-hosting language

Part I of the roadmap asks for an honest, working slice of Keel: a reference
implementation solid enough that the language can begin to be written in itself.
This document records what that slice actually is — the pieces, how they fit,
what is faithful to the design, and what is a deliberate, documented stand-in
for later parts.

The headline result: **Keel is self-hosting.** A compiler written in Keel
compiles itself to C, that C is built to a native binary, and the binary
compiles the compiler again to byte-identical output. The interpreter and the
compiled compiler agree, and the self-hosted compiler agrees with the reference
interpreter on a behavioral-equivalence corpus.

```
make            # build the Stage-0 reference interpreter (bin/keel)
make conform    # the conformance suite — the executable specification
make bootstrap  # build the self-hosting chain and assert the fixpoint
make equiv      # the self-hosted compiler agrees with the oracle
make all-checks # all of the above
```

## The shape of the implementation

There are two implementations of Keel in this repository, and a runtime shared
between them.

**Stage 0 — the reference interpreter (`src/keel.c`).** A tree-walking
interpreter in C11, called *the oracle* because it is the definition of correct
behavior. It lexes with significant indentation and stable error codes, parses
the full surface language, runs programs, type-checks the parts Part I commits
to checking, formats canonically, and dumps tokens and ASTs for differential
testing. It is the thing every other piece is measured against.

**The self-hosting compiler (`compiler/keelc.keel`).** The centerpiece: a
Keel→C compiler written in Keel. It is a complete pipeline —

```
source --lex--> tokens --parse--> AST --codegen--> C
```

— with a hand-written lexer (explicit indent stack), a recursive-descent parser
whose operator-precedence ladder matches the oracle's exactly, and a code
generator that emits C against the shared runtime. It reads its input through a
filesystem capability (the same containment the interpreter enforces) and writes
C to standard output.

**The runtime (`runtime/keelrt.{h,c}`).** The library that compiled Keel
programs link against. Its tagged value type and every operation mirror the
interpreter's semantics, which is *why* a compiled binary behaves like
`keel run`. The context-string escapers — the injection boundary — are not
reimplemented here: both the interpreter and the runtime `#include` one header,
`runtime/keel_escape.h`, so there is exactly one definition of "escape for SQL."

## The bootstrap, concretely

`make bootstrap` runs four stages and checks a fixpoint:

| stage | who runs the compiler | input | output |
|-------|----------------------|-------|--------|
| 0 | — | — | `bin/keel` (the C oracle) |
| 1 | oracle interprets `keelc.keel` | `keelc.keel` | `stage1.c` → `keelc1` |
| 2 | `keelc1` (native) | `keelc.keel` | `stage2.c` → `keelc2` |
| 3 | `keelc2` (native) | `keelc.keel` | `stage3.c` |

A compiler is self-hosting when it reproduces itself: **`stage2.c == stage3.c`**,
byte for byte. It does. We also check **`stage1.c == stage2.c`** — the
interpreter and the compiled compiler emit identical code — and that holds too,
which is a stronger statement than the fixpoint alone requires. Determinism is
by construction: ordered AST traversal and a monotonic gensym counter, with no
iteration over unordered structures.

## Keel-Core: the self-hosting subset

Part I builds the *self-hosting subset* first, exactly as the roadmap advises.
**Keel-Core** is the language the compiler is written in and the language it
compiles: definitions with typed parameters and effect annotations; `let`/`mut`
bindings and assignment; `if`/`elif`/`else`; `loop`, `loop till`, and
`loop through`; `check`/`is` pattern matching over enums, options, and results;
records and arrays with their methods; strings with indexing, slicing, and the
character builtins; integer arithmetic with overflow trapping; function and
method calls; `?` for failure propagation with per-function `Fail` boundaries;
and capability-mediated file reads.

What Keel-Core leaves to the full surface language (still accepted by the oracle,
and slated to move onto this same front-end in Part II) is a small set of
conveniences not needed to *describe* a compiler: string interpolation (the
compiler builds its output with explicit concatenation; a literal `#` is
`chr(35)`), the full numeric tower beyond integers, and closures. Drawing the
line here is what makes the bootstrap reachable and the fixpoint real rather
than aspirational.

Two language rules fall out of this and are worth stating plainly, because they
are real constraints a Keel-Core program must respect:

- **Parameters are immutable.** To mutate, copy into a `mut` local. The
  interpreter treats a reassigned parameter as a shadow, not a mutation, which
  silently gives the wrong answer (a textbook `gcd` that reassigns its
  parameters returns the wrong value); compiled code would mutate. Neither is
  the intended use, so Keel-Core forbids it. The disciplined form — `mut x = a`
  then mutate `x` — agrees everywhere.
- **No shadowing across nested blocks.** Codegen mirrors Keel's lexical scoping
  with C block scoping and a declare-on-first-binding scope stack; re-binding an
  outer name is an update, not a fresh shadow.

There is also one known front-end limitation, inherited from the oracle's
parser: a *grouping* parenthesis whose closing `)` sits immediately before a
`:` (the end of an `if`/`loop`/`check` header) is misread as a lambda parameter
list. It is always avoidable — rely on precedence, or lift the sub-expression
into a helper — and is a candidate fix for Part II.

## What Part I checks, and how

The **conformance suite** (`tests/conform.sh`) is the executable specification.
It is written against the command-line interface, not any implementation's
internals, so it runs unchanged against the interpreter today and could run
against any future implementation. Four categories, as the roadmap's
cross-cutting decision C2 prescribes:

- **positive** — the program runs; stdout and exit code match exactly.
- **negative** — the program is *rejected for a named reason*. This is where the
  guarantees live: a plain string reaching a SQL sink is rejected with code
  `type.sink`; a traversal that escapes a capability root fails catchably;
  integer overflow traps; an undefined name is `name.undefined`; a refinement
  violation, a bad parse, a stray character each have their code.
- **golden** — token and AST dumps match fixtures (differential testing).
- **property** — the formatter is idempotent: `fmt(fmt(x)) == fmt(x)`.

The **equivalence harness** (`tests/equiv.sh`) is the self-hosting cross-check.
For every program in the Keel-Core corpus (`tests/compiled/`) it runs the
program two ways — interpreted by the oracle, and compiled to a native binary by
`keelc` — and requires identical stdout and exit. It passes with the compiler
running interpreted *and* with the compiler running as a native binary.

## Faithful, and honestly deferred

The pieces that are real and load-bearing in Part I:

- Recovering diagnostics with byte spans, caret excerpts, and stable codes.
- A `panic`/`Fail` taxonomy: type errors that the type system is meant to make
  impossible are uncatchable aborts with codes (`type.sink`, `type.method`,
  `name.undefined`, …); recoverable conditions are catchable `Fail`s. The
  injection-sink rejection is uncatchable by design.
- Real capability containment: paths are canonicalized relative to a root and
  any `..` that would escape is rejected, on real file I/O.
- Capability-honest modules: imports resolve and bind, cycles are detected, and
  no effectful code runs at import time — authority enters only at `main`.
- The single-source escaper shared between interpreter and runtime.
- The amortized-array and per-iteration-scope fixes in the interpreter that make
  running a real compiler on the tree-walker tractable.

The stand-ins, each pointing at the part that replaces it:

- **Refinements are checked at runtime**, at binding and validation boundaries,
  rather than discharged by an SMT solver. Part IV makes them static.
- **Memory is a tracked arena freed at exit** — the roadmap's explicitly
  time-boxed "conservative interim" allocator. Part IV replaces it with the
  ownership-directed allocation that delivers the GC-free promise.
- **Codegen targets a tagged-value runtime**, not yet monomorphized unboxed
  machine types. The semantics are right; the representation is Part IV's job.

Stating the line between the two is the point. Part I is not the whole language;
it is a faithful core that already carries the language's distinctive
guarantees — and that can now be extended in Keel itself.
