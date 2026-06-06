# Keel — Stage-0 Reference Interpreter: Design Notes

This document describes the implementation in `src/keel.c`: a single-file, tree-walking
reference interpreter for the Keel language, written in C11. It follows the bootstrap plan
in §9 of the specification — *the reference implementation brings the language up; the
language is then rewritten in itself.* This interpreter is Stage 0: the oracle that the
self-hosting toolchain is checked against.

The guiding principle, taken from the spec itself, is **honesty**. Where a feature is
implemented faithfully, this document says so. Where the interpreter uses a runtime
stand-in for something the real compiler would prove statically, it says that too, plainly,
rather than pretending the demonstration is more than it is.

## What the interpreter is

A complete pipeline that takes Keel source to observable behavior:

    source → lexer → parser → AST → tree-walking evaluator
                                  ↘ canonical formatter
                                  ↘ property-based test runner

It is invoked as `keel <run|test|fmt|version> file.keel`. There are no external
dependencies beyond libc and libm; it builds with `make`.

## The pieces, and how honest each one is

### Lexer — faithful

Significant indentation is tokenized into explicit `INDENT` / `DEDENT` / `NEWLINE` tokens
using an indentation stack, with newlines suppressed inside brackets so multi-line
expressions work. Numbers carry their kind through the lexer: an integer, a `decimal`
(exact, when a `.` appears with no float suffix), or a float. Context-string tags are
recognized by the lexer — `sql"..."`, `shell"..."`, `html"..."`, `url"..."` — so the
*type* of a string literal is fixed at its first character, which is what makes the
injection guarantee possible later. Interpolation (`#name`, `#{expr}`) is tokenized in
place. This part of the implementation matches the spec's intent directly.

### Parser — faithful

A precedence-climbing (Pratt) parser produces the AST. It handles the whole surface
syntax exercised by the examples: `def` with return type after `;` and effects after `/`
as `{Io, Fail<E>}`; `struct` / `enum` with payload-carrying variants; `derive (...)`;
pattern matching with `check x: is Circle(r): ...`; the three loop forms
(`loop`, `loop till cond; init; step:`, `loop through xs:`); `execute` / `handle` blocks;
refinement types (`int where >= 0`); generics in both `[T]` and `<T>` forms; ternary,
null-coalescing `??`, optional-chaining `?.`, and postfix `?` for try. Because Keel is
expression-oriented, `execute`, `check`, and `if` are parsed in value position as well as
statement position (e.g. `port = execute: ... handle Fail as e: resume 8080`).

### Algebraic effects — faithful mechanism, one-shot resumption

This is the heart of the language and it is implemented as a real effect mechanism, not
faked. It uses the classic `setjmp`/`longjmp` technique (the same approach behind libhandler
and Leijen's "Implementing Algebraic Effects in C"): a dynamic stack of installed handlers,
and a stack of perform-sites. Performing `Fail` searches outward for the nearest matching
handler; running a handler clause can either fall off the end (which *aborts* to the
`execute` site and yields the handler's value) or evaluate `resume v` (which `longjmp`s back
to the perform site and continues the original computation with `v`). Both paths are
demonstrated in `examples/03_effects.keel`: one handler resumes with a default port, the
other aborts with a fallback value.

The honest limitation: resumptions are **one-shot and in-scope**. A handler may resume at
most once, and only while its `execute` frame is still on the C stack. Full multi-shot
continuations (resuming twice, or storing a continuation to call later) would require
capturing and copying stack segments; that is out of scope for a Stage-0 interpreter. For
the effects the spec leans on — `Fail`, condition/restart, `Io` — one-shot in-scope
resumption is sufficient and behaves correctly.

`Io` is modeled as an ambient effect that simply runs: this is exactly the spec's
"colorblind" claim (§3.4) — the same code is synchronous or asynchronous depending only on
which handler `serve` installs — but the interpreter only ships the synchronous handler.

### Capability-based authority — faithful in shape, runtime-enforced

`main(sys: System)` receives the one root capability; everything else is reachable only by
narrowing it (`sys.fs.subtree("/var/log")`, `sys.net.database(...)`). Capabilities are
ordinary values; a function that is not handed a `Db` cannot touch a database. Path
traversal that escapes a capability's root is rejected — and rejected as a *catchable*
`Fail`, so a handler can recover, which `examples/04_security.keel` shows. The shape of the
model is faithful: authority flows by value, narrows monotonically, and is unforgeable
from within safe code.

The stand-in: the filesystem and network capabilities don't perform real I/O. A `Db` is a
test double whose `run` executes a deliberately tiny "query engine" (it reads the `id = N`
out of the parameterized SQL and filters in-memory rows). This is enough to demonstrate the
*security* properties end to end without pretending to be a database driver.

### Context-typed strings — faithful (this is the real guarantee)

`db.run(...)` accepts **only** a `sql"..."` value. Passing a plain `string` is rejected —
in the interpreter, with an error; in the real compiler, it simply would not type-check.
Either way the property is the same: *a string built from untrusted input cannot reach a
SQL sink*, because a plain string is a different type from `sql""`, and concatenating
untrusted data into a `sql""` is what the typed-string machinery exists to prevent.
Interpolating a value into `sql"...#id..."` escapes it for the SQL context; the same
interpolation into `html"..."` / `shell"..."` / `url"..."` escapes for *those* contexts.
The injection class is closed structurally, not by discipline.

### Trust and secrets — faithful

`Untrusted<T>` wraps network input; it cannot flow into a refined or plain target without
passing through a validation boundary, which is where parsing happens (`raw.to_int()?`).
Parsing untrusted input *is* the validation step, so the parsed, refined result is trusted.
`Secret<T>` never renders its contents — it prints as `Secret(<redacted>)` everywhere
except an explicit `reveal()`. Both are shown in the security and flagship examples.

### Refinement types — runtime checks, **not** an SMT proof

This is the most important stand-in to be clear about. The spec (§3.8) describes refinements
like `int where >= 0` as discharged by an SMT solver at compile time. **This interpreter
does not run a solver.** It checks the predicate *at runtime* at the point of binding,
construction, or validation, and fails if it does not hold. The observable behavior at the
boundary is the same (a value that violates `where >= 0` is rejected), but it is a dynamic
check, not a static proof, and it therefore cannot reject a bad program before it runs the
way the real compiler would. Treat the refinement support here as an executable
specification of *what* must hold, not as the mechanism that proves it.

### Checked arithmetic & exact decimals — faithful

Integer arithmetic uses the compiler's overflow-checked builtins and traps on overflow
rather than wrapping silently (§5.4). `decimal` is implemented as an exact scaled integer:
addition, subtraction, and multiplication are exact, and division extends precision. Arrays
broadcast over scalars (`[1,2,3] * 10`), matching the array-computing goal of §3.10.

### Memory model — deliberately not modeled at runtime

Keel's headline claim is memory safety without a garbage collector, via ownership,
borrowing, regions, and generational references (§4). A tree-walking interpreter has nothing
to be safe *about* in that sense — it is itself written in C and uses a tracked arena that
is freed at exit. So borrows (`&`, `&mut`) evaluate to the referent (identity at runtime),
and `region`/`scope` blocks simply execute. The interpreter neither demonstrates nor
violates the ownership system; that system is a compile-time discipline, and verifying it is
a job for the type checker the self-hosting compiler will carry. This is stated plainly
rather than papered over.

### Tests, including property tests with shrinking — faithful

`test "..."` and `test prop "..." (xs: array[int])` are first-class. The property runner
generates random inputs by type, runs ~100 cases, and on failure **shrinks** the
counterexample toward a minimal one. `examples/05_tests.keel` includes a deliberately false
property; the runner finds it and shrinks the counterexample to `n=0`, the true boundary.

### The canonical formatter — faithful, and idempotent

The spec mandates exactly one layout with no options (§3.14). `keel fmt` walks the AST and
re-emits canonical Keel: 4-space indentation, normalized spacing, one true form. It is
**idempotent** — `fmt(fmt(x)) == fmt(x)` for every example (checked by `make fmt-check`) —
and semantics-preserving (format-then-run reproduces identical output).

### Not implemented

Honesty cuts both ways; these spec features are absent here. Content-addressed code (§3.16):
not implemented. Comptime metaprogramming (§3.7): `comptime` blocks parse and simply
evaluate; there is no staged compilation. The module/import system and FFI parse to no-ops.
AOT compilation to native code (§3.11): there is none — this is an interpreter by design,
since its job is to be the readable oracle, not the fast path.

## The bootstrap story

The reason this interpreter exists, per §9, is to bring the language far enough up that Keel
can be written in Keel. `bootstrap/lexer.keel` is the first concrete link in that chain: **a
lexer for Keel, written in Keel**, running on this C interpreter, tokenizing Keel source
into keywords, identifiers, numbers, strings, and operators. It uses only language features
the interpreter implements faithfully — structs, enums, pattern-friendly dispatch, string
indexing and slicing, the character primitives, and loops — which is the point: the subset
needed to describe the language's own front end is already real.

A full self-hosting compiler extends exactly this approach through the parser, the type
checker (where the refinement *solver* would finally appear for real), and a code generator.
That is a much larger effort; what is demonstrated here is that the foundation is sound and
the language is already expressive enough to begin describing itself — Stage 0 reaching
toward Stage 1.

## Building and running

    make            # build bin/keel
    make examples   # run every example program
    make test       # run the in-language unit + property test suites
    make bootstrap  # run the Keel-in-Keel lexer
    make fmt-check  # verify the formatter is idempotent on every example

    ./bin/keel run     examples/06_flagship.keel
    ./bin/keel test    examples/05_tests.keel
    ./bin/keel fmt     examples/02_types_match.keel
