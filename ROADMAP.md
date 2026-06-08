# Keel — Roadmap from Stage-0 Interpreter to a Production Language

This document is the engineering roadmap for taking Keel from its current artifact — a
~2,800-line C11 tree-walking interpreter — to a **full-fledged, production-grade language**
that teams can rely on to build advanced systems across the whole stack. It is decision-level,
not a task list: it says what is real, what is a stand-in, what is on the critical path, and —
crucially — *why each choice is forced or free*, in the voice of `DESIGN.md` and the
specification (`SPEC.md`).

It is organized in two movements.

**Part I — Reaching self-hosting.** The bootstrap: cross-cutting decisions, hardening Stage 0,
growing the language in Keel, and the three-stage fixpoint that retires the C interpreter to
"reference oracle." This part is preserved in full from the prior plan; its reasoning is sound
and nothing here supersedes it.

**Parts II–VII — Reaching production.** Self-hosting is a milestone, not a product. A language
people build reliable systems with needs an optimizing native backend, every spec guarantee
*actually enforced* rather than stood-in, a real concurrency runtime, the full single-language
toolchain, a broad standard library, the platform targets, content-addressed storage with
editions, supply-chain and security hardening, and a stability-and-governance story. These
parts take the same decision-level rigor out to 1.0 and beyond.

A starting assumption about the current artifact: `Token` and `Node` carry only a line number;
the parser and lexer `exit()` on the first error; effects run on a `setjmp`/`longjmp` one-shot
handler stack; capabilities are runtime test doubles; and the first Keel-in-Keel program
(`bootstrap/lexer.keel`) still hardcodes its input as string literals.

---

## Fidelity check: does this plan diverge from the language's vision?

It does not, and the discipline below is what keeps it aligned — recorded here so the alignment
is auditable rather than assumed.

- **The bootstrap is the spec's own plan (§9).** "A reference interpreter in C brings the
  language up, then the compiler is rewritten in Keel and the C interpreter is retired except
  as a reference oracle." Part I is that sentence, made executable.
- **The assurance continuum is staged, never weakened (§3.8).** Refinements move from runtime
  checks toward SMT-discharged proofs *without ever regressing soundness* (runtime fallback for
  `unknown`). The proof tier (`total`, full verification) is on the map, not dropped.
- **Effects stay full algebraic effects (§3.3, §3.4).** Escaping continuations are treated as a
  *language* requirement (colorblind async), multi-shot as mandated by "full algebraic effects,"
  and the representation chosen at codegen is one that *admits* both rather than welding them shut.
- **Memory stays GC-free by ownership (§4).** The one pragmatic risk — an interim arena/refcount
  allocator to unblock early codegen — is explicitly time-boxed: the ownership-directed allocator
  that delivers the GC-free promise must land before 1.0 (Part IV), and `DESIGN.md` must state
  which allocator any given build uses. Anything else would betray the single property that lets
  Keel span kernels to scripts.
- **The injection guarantee is single-source (§5.2).** One runtime, one definition of "escape for
  SQL," shared by interpreter and compiled output, asserted by negative conformance cases. The
  `db.run`-on-a-plain-string stand-in is promoted to a *static* rejection in the self-hosted
  checker — the costume comes off.
- **One official toolchain, capability-honest dependencies, enforced semver, content-addressing,
  editions (§3.12, §3.14, §3.16).** Each is a named deliverable in Parts II and VI, not an
  afterthought.

The net: the prior roadmap is a faithful *prefix* of the journey. The work below is the rest of
the journey, held to the same vision.

---

## What "production-ready" means here (the 1.0 exit criteria)

A roadmap to "full-fledged" needs a definition of done it can be measured against. Keel reaches
1.0 when *all* of the following hold. Each maps to a Part below; together they are the gate.

1. **Self-hosting and witnessed.** The compiler compiles itself to a fixpoint (Stage2 == Stage3,
   bit-identical), passes the conformance suite identically to the C oracle, and survives a
   diverse-double-compilation check using the oracle (Part I).
2. **Every headline guarantee is *enforced*, not stood-in.** Injection, path traversal, secret
   exposure, null, integer overflow, uninitialized use, unhandled error, and data races are
   rejected statically by the checker — verified by negative conformance cases — and the GC-free
   memory model is realized by the ownership pass, not an arena (Part IV).
3. **Fast as C, interactive as Python.** The tiered toolchain (interpreter/JIT for iteration, LLVM
   AOT for release) meets published performance targets on a benchmark suite, with a regression
   gate (Part III).
4. **The single toolchain is complete.** Compiler, the one canonical formatter, linter, package
   manager (content-addressed, capability-sandboxed, semver-enforced), test/property/fuzz runner,
   doc generator, language server, and debugger ship as one (Part II).
5. **The standard library is broad, modular, and capability-honest** — collections (incl.
   persistent immutable), exact decimal, time, text, encoding, N-d arrays/SIMD, networking, crypto
   — auto-imported as a prelude, every authority-bearing module exposing a capability type (Part VI).
6. **The stack is reachable.** Native (Windows/macOS/Linux), `no-runtime` freestanding, and
   WebAssembly targets all pass conformance; first-class cross-compilation; safe C FFI both
   directions (Part VI).
7. **Concurrency is real.** A working `Io` scheduler delivers colorblind sync/async on the same
   source; structured concurrency is enforced; the typed-actor/supervision library runs (Part V).
8. **Evolution without breakage is institutional.** Editions, compiler-enforced semver,
   reproducible builds, a security-response process, and an RFC governance model are in place
   (Parts II, VII).
9. **The spec is executable.** The conformance suite *is* the language definition, runs against any
   implementation, and is published as the standard (Parts I, VII).
10. **It is learnable.** Reference manual, a book-length guide, migration/interop guides, and a
    language server good enough that the editor experience matches the spec's "readable as Python"
    claim (Part VII).

Beyond 1.0, the spec's frontier items — the full proof tier for avionics/crypto/kernels/contracts,
production hot code reload, deterministic record-replay debugging, and the quantum circuit library
— are tracked in Part VII as post-1.0 commitments, because the spec lists them and a credible
roadmap must say where they land.

---

## The single organizing idea

The whole bootstrap rests on one claim: **the C interpreter is a trusted oracle**. Every later
stage — the Keel-written lexer, parser, checker, and code generator — is correct exactly to the
degree that it agrees with the oracle on observable behavior. Two consequences shape everything
below.

First, the oracle is only as good as the *definition of agreement*. That definition is the
conformance suite (Tier 1.2), so that suite is not regression-testing hygiene — it is the
operational specification of Keel, and it must be written to run unchanged against *any*
implementation, the C one today and the self-hosted one later. This is also why, in Part VII, the
same suite becomes the published language standard: it was built to be implementation-agnostic
from the first day.

Second, you have, at each link in the chain, two implementations of the same stage: the C one
already in `keel.c` and the Keel one you are writing. Running both on the same corpus and
asserting they agree (**differential testing**) converts the interpreter's maturity into a
free, continuous oracle for the reimplementation. This technique is not in the original plan
and it is the highest-leverage assurance move available; it is called out repeatedly below.

## Two tracks: the critical path versus the assurance bets

The tiers in Part I are ordered roughly easy-to-hard. That is a fine work order, but it conflates
two different goals, and separating them changes what you do first.

**The critical path to self-hosting** is short and does *not* include the hardest research items:

    source spans  →  real file I/O  →  module loader  →  parser.keel  →
    minimal type checker  →  deterministic C codegen  →  three-stage bootstrap fixpoint

A compiler can compile itself while still checking refinements at runtime, while still resuming
effects one-shot-in-scope, and while doing no static ownership analysis at all — provided it
compiles *itself* correctly (a batch transform never awaits or backtracks). Reaching "Keel compiles
Keel" therefore does not require solving the SMT, resumable-continuation, or borrow-checking
problems — even though, per 3.2, escaping continuations are required by the *language* (§3.4
colorblind concurrency) and so sit on the language track, not the self-hosting one.

**The assurance bets** — the SMT refinement solver, resumable/multi-shot continuations, and the
ownership/borrow pass — are what make Keel the language the spec promises. They are off the
self-hosting critical path. The strategic payoff of seeing this: you can reach the self-hosting
milestone first, retire the C interpreter to oracle status, and then harden the guarantees *in
Keel*, where they belong, with the oracle still available to check you. Trying to land the
assurance bets in the C interpreter first means doing hard work twice — once in C, then again in
Keel — on an artifact destined to retire.

The roadmap keeps the original four-tier structure for Part I because it is a sensible difficulty
gradient, but it flags for each item whether it is on the critical path or on the assurance track,
and at the end of Part I proposes a single merged ordering for someone who wants the fastest sound
route to self-hosting. Parts II–VII then carry their own ordering, because once the oracle has done
its job the constraint shifts from "agree with C" to "be fast, complete, and stable."

---

# PART I — REACHING SELF-HOSTING

*Preserved in full from the prior plan. Internal references (Tier 1.2, 3.1, C1…C5, §-numbers)
are unchanged so the cross-links remain intact.*

## Cross-cutting decisions to make once, before Tier 1 coding

Five things will be threaded through every later stage. Deciding them now, deliberately, prevents
five expensive retrofits. None of them is large; all of them are load-bearing.

### C1. Spans are a permanent value type, not a column added to a token

The instinct for Tier 1.1 is "add a `col` field." Resist it. Introduce a first-class span — a
file id plus a byte range `(lo, hi)` into the source — and compute line/column lazily from a
per-file table of line-start offsets built once at lex time. Reasons this must be a value type
and not a patch:

- Byte ranges compose. A binary expression's span is the union of its operands' spans; a
  diagnostic can underline an entire sub-expression, not just point at a token. Line+column
  cannot be unioned meaningfully.
- The span type becomes part of the **AST contract** (C3). The Keel-side parser must produce the
  *same* span representation, because the self-hosted compiler must emit diagnostics of the same
  quality. If you design spans casually in C now, you design the diagnostics ceiling for the real
  compiler.
- Every later stage consumes spans: the type checker propagates them through inference so a
  unification failure points at the right expression; the refinement pass (3.1) points at the
  predicate it could not discharge; codegen embeds them for runtime panics. A span that exists
  only in the parser is a span you rebuild three more times.

Concretely: intern filenames to integer ids, store `Span{file_id, lo, hi}` on every `Token` and
every `Node`, keep the source text addressable by file id for excerpt rendering, and make the
line/column computation a pure function of `(file_id, offset)`.

### C2. The conformance suite is the executable specification

Build it implementation-agnostic from day one: each case is `(program, optional stdin) →
expected stdout, expected exit code, and — once 1.1 lands — expected diagnostics keyed by code and
span`. A thin runner diffs actual against expected. This is the artifact that makes "oracle"
mean something, and because it depends on no internals it runs verbatim against the self-hosted
compiler in Tier 4 — and is published as the language standard in Part VII. Design it with four
case categories, not one:

- **Positive**: program runs, output matches.
- **Negative**: program must be *rejected*, and rejected for a *named reason* (error code + span).
  Negative cases are where the language's guarantees actually live; a suite with only positive
  cases cannot certify that injection "cannot type-check."
- **Property**: formatter idempotence (`fmt(fmt(x)) == fmt(x)`), format-then-run equivalence, and
  parse-unparse-parse stability. `make fmt-check` already does the first; fold it in.
- **Golden-AST / golden-token**: a canonical dump of the token stream and AST for each program,
  for differential testing (C4). These are the cases the Keel-side stages will be checked against.

### C3. The AST is a versioned, serializable contract between stages

The moment `parser.keel` emits an AST that `checker.keel` and `codegen.keel` consume, AST churn
cascades through three programs. Treat the AST as a published interface: a versioned schema, with
a canonical serialization. You already own a serialization you can trust — the formatter. Exploit
it: `fmt` is the AST's pretty form, and `fmt(parse(x))` is a cheap, already-trusted fingerprint of
"what the parser understood." For machine comparison (C4) add a stable structural dump (S-expression
or CBOR) with deterministic field order.

A note on the C side specifically: the current `Node` is one fat struct with `a,b,c,d` plus two
reused `Vec`s, disambiguated by `kind` and documented at the use site. That is fine for a private
interpreter and wrong for a published contract. The *Keel-side* AST should be proper enums and
records — the bootstrap lexer already shows Keel expresses exactly this — and the C-to-Keel mapping
should be explicit and tested, not implied.

### C4. Differential testing is the safety net for the entire climb

At every link, run the C stage and the Keel stage on the same corpus and assert equality of the
canonical output: identical token streams for the lexers, structurally identical ASTs (compare via
the C3 serialization, or via `fmt` equivalence) for the parsers, identical accept/reject verdicts
for the checkers. This catches divergence the instant it appears rather than at the final bootstrap,
and it means every hour the C interpreter is mature is an hour of oracle coverage you get for free.
This is the move that de-risks Tiers 2 and 4. It requires only C2's golden dumps and C3's canonical
serialization, which is why those come first.

### C5. Factor the runtime and prelude out now; the escapers are a security single-source

The context-string escapers (`sql`/`html`/`shell`/`url`), the exact-decimal arithmetic, the
checked-arithmetic traps, the capability objects, and the effect trampoline currently live inside
`keel.c`. Both the interpreter *and* the compiled output will need identical behavior from all of
them. The escapers are the sharp case: they are the injection boundary, and if `keel run` escapes a
`sql"...#id..."` one way and a compiled binary escapes it another way, the headline guarantee
silently diverges between interpreter and compiler with no test failing. Decide the runtime contract
behaviorally in the conformance suite (C2), and plan for a *single* runtime implementation that both
the interpreter and the C-codegen target link against — ideally, in the long run, the escapers
written in Keel, compiled once, and shared, so there is exactly one definition of "escape for SQL."

These five are cheap to decide and expensive to retrofit. Everything below assumes them.

---

## Tier 1 — Harden Stage 0 (days, low risk, all on or enabling the critical path)

### 1.1 Error reporting with source spans and statement-level recovery

**Goal.** Replace `line:message` plus immediate `exit(65)` with column-accurate, caret-underlined
excerpts, and recover at statement boundaries so one run reports many errors.

**Why here.** This is the first place the span type (C1) becomes real, and the bootstrap parser
will be the first large Keel file where "die on first error" becomes intolerable. Doing it now
means the diagnostic machinery exists before the files that need it.

**Sub-tasks.**
- Land the `Span` value type (C1) on `Token` and `Node`; build per-file line-start tables; render
  excerpts with a caret/underline spanning the offending range.
- Give each diagnostic a stable error **code** (e.g. a short mnemonic), because negative conformance
  cases (C2) assert on codes, not on prose, and because the self-hosted compiler must reproduce them.
- Replace `perr`/`lex_error`'s `exit()` with error *accumulation* plus recovery. The recovery
  discipline for an indentation-significant, statement-oriented language is to **synchronize on
  `NEWLINE`/`DEDENT`**: on a parse error, record it, then discard tokens until the next statement
  boundary at the current or shallower indentation, and resume. This bounds cascade errors without
  the bracket-counting that brace languages use.
- Distinguish *lex* errors (often unrecoverable mid-token; recover by skipping to newline) from
  *parse* errors (recover to statement boundary) from *eval* errors (already a `longjmp` to
  `g_panic_buf`; leave runtime semantics alone, this step is about static diagnostics).

**Gotchas.** Recovery that is too aggressive produces phantom errors that bury the real one; cap
reported errors (e.g. first N) and prefer under-reporting to a wall of noise. Indentation recovery
interacts with the `INDENT`/`DEDENT` stack — after skipping tokens you must resynchronize the
indentation stack to the boundary you stopped at, or the next statements mis-nest.

**Done means.** A file with three independent syntax errors reports three, each with a correct
caret; the conformance suite has negative cases asserting specific codes at specific spans; no
input causes `exit()` before the whole file has been scanned for further errors.

### 1.2 The conformance harness (this is C2 — build it here)

**Goal.** Turn `make examples` from "runs without crashing" into "produces exactly the expected
observable behavior," across all four case categories.

**Why here.** It locks behavior before the codebase grows, and — more importantly — it is the spec
the oracle embodies. Every later "did I break something" and the final "does the self-hosted
compiler agree" question is answered by this one artifact.

**Sub-tasks.** Lay out `tests/` as `(input.keel, expected.out, expected.exit, expected.diag)` tuples;
write the runner to diff all three; seed it from the existing examples; add the negative and property
categories from C2; add the golden token/AST dumps that C4 will consume. Make `make test` run the
whole thing and fail loudly.

**Gotchas.** Pin determinism in the *interpreter's* observable output now (stable map/iteration order,
no addresses in `Debug` output, fixed PRNG seed for property tests so a failing `test prop` shrinks
to the same counterexample every run) — the property runner already shrinks `n=0`; make that
reproducible. This determinism discipline is the same one codegen will need (4.2), so you are
building the habit early.

**Done means.** `make test` is the gate; a behavior change anywhere shows up as a specific diff; the
suite is written against the CLI, not against C internals, so it is portable to the self-hosted
compiler.

### 1.3 Decide the panic-versus-Fail taxonomy, with a principle rather than case-by-case

**Goal.** Several builtins currently `panic` where the real compiler would emit a *type error*
(`db.run` on a plain string; `no method`), while others raise a *catchable* `Fail` (path traversal).
Replace ad-hoc choices with a rule.

**The principle.** *Fail is for conditions a correct program could plausibly anticipate and recover
from at runtime — bad input, a missing file, a port already in use, a path that escapes its root. A
hard, uncatchable abort is for violations the type system is meant to make impossible — a plain
string reaching a SQL sink, a call to a method that does not exist, an arity mismatch.* In the
interpreter these aborts are stand-ins for compile errors, and they must stay uncatchable, because
making them catchable would model an "impossible" condition as runtime-recoverable, which directly
contradicts the language's claim that it cannot happen. A handler that could `resume` past
`db.run("plain string")` would be quietly teaching users that injection is a runtime concern.

This single criterion resolves the case-by-case question: path traversal is a `Fail` (untrusted input
can legitimately try to escape; recovery is meaningful, and `04_security.keel` depends on it);
`db.run` on a non-`sql` value stays a hard abort (it is a type error wearing a runtime costume);
unknown-method and arity errors stay hard aborts (likewise). Document the rule and the resulting
classification in `DESIGN.md`, so the eventual checker knows which of these become static rejections
(the aborts) and which remain runtime effects (the Fails). The `db.run` abort in particular is the
one stand-in that the self-hosted checker *must* promote to a static rejection (Tier 4.1) — it is
the headline guarantee.

**Done means.** Every interpreter abort is labeled in source and in `DESIGN.md` as either "type error
stand-in (uncatchable, becomes static check)" or "runtime condition (catchable Fail)," and the
conformance suite has a negative case for each abort and a recovery case for each Fail.

---

## Tier 2 — Extend the surface the self-hosting compiler needs (1–2 weeks, critical path)

A sequencing correction first: the original order lists file I/O, then parser, then modules. The real
dependency order is **file I/O → module loader → `parser.keel`**, because you cannot load modules from
disk without file I/O, and you cannot cleanly split `parser.keel` from `lexer.keel` and a shared AST
module without imports. Modules are a prerequisite for a clean parser, not a successor to it.

### 2.1 Real file I/O behind the `Dir`/`File` capabilities

**Goal.** Make `sys.fs.open(path).read()` read actual bytes, so `bootstrap/lexer.keel` stops
hardcoding its source strings and starts tokenizing the real tree. This is the single change that
turns the bootstrap demo into a tool.

**Why here.** It is the gate for everything Keel-side: the lexer reading files, the module loader
reading modules, the compiler reading its own source.

**Sub-tasks and the two non-obvious parts.**

- *Path containment must be real, not `strstr(p, "..")`.* The current check both over-rejects (any
  legitimate name containing `..`) and under-rejects (absolute paths, symlink escape, `.`-games,
  `foo/../../etc`). Replace it with proper containment: reject absolute paths from within a
  capability, canonicalize the resolved path, and verify it is still a prefix of the capability root
  *after* resolution, following symlinks. Keep rejection a catchable `Fail` (per the 1.3 principle —
  an escaping path is exactly an anticipatable runtime condition). Add encoding handling (the lexer
  assumes byte access; decide and document UTF-8 handling at the read boundary, because `src[j]`
  indexing in Keel is currently byte indexing).

- *The file-to-compiler authority bootstrap.* This is where capability honesty meets the fact that
  the compiler is itself a Keel program. The compiler must read source files, so *something* holds
  filesystem authority — but Tier 2.3 insists "imports bring names, never authority." The resolution
  to design now: file authority enters only at `main(sys: System)` and flows by value into the
  compiler's loader; the loader reads modules and binds their *names/values* into the importing
  scope; the imported module does **not** thereby inherit the loader's `Dir`. State this explicitly,
  because if it is left implicit it quietly undermines the capability story the moment the compiler
  is a Keel program reading other Keel programs.

**Done means.** `bootstrap/lexer.keel` reads its inputs from disk via the capability; an attempt to
read outside a narrowed root produces a catchable `Fail` and a conformance case proves it; the
authority-flow rule is written down.

### 2.2 Grow `lexer.keel` into `parser.keel`

**Goal.** A recursive-descent parser written in Keel that consumes the token stream and emits the AST
as Keel records/enums. This is the most satisfying milestone — the grammar describing itself — and
the one that surfaces exactly which language features are still missing or awkward.

**The engine of progress: the feature-backfill loop.** Writing `parser.keel` will repeatedly hit a
construct the interpreter does not yet support well enough to express a parser. Name and budget for
the loop explicitly: for each missing feature, (1) add it to the C interpreter, (2) add a conformance
case, (3) extend the formatter to render it idempotently, (4) add a differential test (C4). That
four-step loop is the actual mechanism of Tier 2 and most of Tier 4; treating it as a named process
rather than a series of surprises is what keeps the climb predictable.

**Use the oracle (C4) hard here.** You already have a parser in C. For every program in the corpus,
assert that `parser.keel` and the C parser produce the same AST — compared through the C3
serialization or, cheaply, by checking that `fmt` of both ASTs is byte-identical. This makes the C
parser a continuous oracle for the Keel parser and turns "is my reimplementation correct?" from a
manual review into a test.

**Done means.** `parser.keel` parses every example and the bootstrap sources; its AST matches the C
parser's on the whole corpus under differential testing; the feature-backfill loop has driven any
missing constructs into the interpreter, the suite, the formatter, and the differential tests.

### 2.3 A minimal, capability-honest module/import system

**Goal.** Replace the parse-to-no-op `import` with a real loader so Keel-side code can span files
(`lexer.keel`, `parser.keel`, a shared `ast.keel`).

**Sub-tasks.** Resolve imports to a file via the loader's authority (2.1); bind imported *names* into
the importing module's namespace; enforce the honesty rule (no authority transfer through imports);
decide the resolution model — a search root plus relative resolution is enough to self-host. Detect
and report import cycles with a real diagnostic (1.1).

**Gotchas.** Initialization order and cycles are the classic module hazards; decide now whether
modules may run top-level effectful code at load (prefer: no, or only `Io`-free initialization),
because that decision interacts with the capability story.

**Done means.** The Keel front-end is split across files that import one another; no import grants
authority; cycles are diagnosed; the whole thing still passes differential testing against the C
front-end.

---

## Tier 3 — The assurance bets (the spec's distinctive promises, off the critical path)

These are the features that make Keel *Keel*. None is required to reach self-hosting, and each is
better built in Keel against the oracle than in the C interpreter that is about to retire. The
recommendation for all three is the same in shape: design the *interface* now, defer the *hard
implementation* until after the first self-host, and never let the hard version regress soundness in
the meantime. (Their full production-grade completion is tracked in Part IV.)

### 3.1 Replace the refinement stand-in with a real static discharge — staged, not Z3-first

**The honest gap.** `int where >= 0` is checked at runtime, not proven. Closing it moves refinements
from "executable specification of what must hold" toward the spec's compile-time proofs.

**The architecture is standard; the backend is a choice.** Refinement type checking reduces to the
validity of *verification conditions* discharged by an SMT solver. So build a
**verification-condition generator** over the AST, plus an abstract **discharge oracle** with the
interface `discharge(VC) → {valid, invalid, unknown}`. That separation is the whole game.

**Do not make Z3 the first or the required backend.** Binding the trusted path of a self-hosting,
"certifiable" language to a large external C++ solver gives it a permanent, unverified, non-Keel
dependency in its core. Build the backend in three stages:

1. **A small self-contained decision procedure for the fragment that actually occurs** — linear
   integer arithmetic and simple bounds (`>= 0`, `!= 0`, `< len`, ranges). A static pass over the
   AST that runs before evaluation.
2. **Keep the runtime check as the fallback for `unknown`.** The key soundness move: when the static
   pass cannot prove a VC, fall back to the runtime check rather than rejecting or admitting. The
   system stays sound; the pass is *incremental* (every predicate you learn to prove removes a
   runtime check and never changes a result).
3. **Slot a real SMT backend behind the same `discharge` interface later, as an optional accelerator.**
   When you need the hard fragment (nonlinear, quantified), Z3 or CVC5 plugs into the oracle interface;
   it accelerates, it is never in the minimal trusted path.

**A forward connection.** Refinements interact with ownership: aliasing facts let the refinement logic
reason about mutable locations soundly. Design the VC generator to be able to *consume* the aliasing
facts the ownership pass (3.3) will produce.

**Done means.** A pre-evaluation static pass proves the common predicates and reports the rest as
"checked at runtime"; the abstract `discharge` interface exists; soundness is preserved by the runtime
fallback; `DESIGN.md`'s refinement note is updated accordingly.

### 3.2 Resumable continuations — required for the language, realized at codegen, never welded shut

**What the design already settles.** The colorblind concurrency claim (§3.4) is a spec commitment, and
an async scheduler suspends a computation and resumes it *after* the handler returns to the event loop
— i.e. "storing a continuation to call later," which the present one-shot-*and-in-scope* model cannot
express. Because §3.4 is a stated feature, a full-fledged Keel must deliver it, and delivering it forces
lifting the in-scope restriction.

**Two axes, only one named by the spec.**
- *Scope* (in-scope vs escaping): async and generators are each a sequence of one-shot resumptions that
  merely need to **escape** their handler. **Escaping continuations are a v1 requirement**, from §3.4.
- *Count* (one-shot vs multi-shot): backtracking/`amb`/probabilistic inference need it; no section
  pins it explicitly, but the language's self-description as *full algebraic effects* mandates not
  welding the wall in (the OCaml-5 one-shot compromise is widely treated as a limitation).

**The single decision, and where it lands.** Choose a continuation representation that is *escaping and
multi-shot-capable*, at **codegen (Tier 4.2), not in a rewrite of the interpreter**. Type-directed
selective CPS / evidence passing (Koka's C backend, Effekt) gives both for free: effects in the type
let the compiler emit pure code in direct style and effectful code in CPS, lowering handlers to
multi-prompt delimited continuations and then to C; once the continuation is a CPS closure it is
callable zero, one, or many times. The decision is not "do we need multi-shot" but "we must not pick a
mechanism that precludes it."

**Delivery may stage what the architecture must permit.** v1 exercises one-shot-escaping (async,
generators, `Fail`, condition/restart — the overwhelming majority of real use) on a representation that
*admits* multi-shot; resume-twice turns on once the type system can keep it sound. The interpreter
honestly stays one-shot-in-scope until codegen exists; if an earlier async demo is wanted, the minimal
honest step is heap-allocated fibers (OCaml 5's model), not a `setjmp` retrofit.

**Where multi-shot meets ownership.** Resuming twice re-performs effects; over a continuation that owns
a resource it duplicates ownership. The move/uniqueness tracking the ownership pass (3.3) needs for
GC-free memory is exactly what licenses which resumptions may fire more than once. Design the
effect-lowering and the ownership pass to share that fact.

**Done means.** Codegen's effect lowering uses an escaping, multi-shot-capable representation from the
start; v1 ships at least one-shot-escaping so the §3.4 async handler is real; multi-shot is gated by
3.3's linearity facts.

### 3.3 A static pass for ownership and borrowing

**Goal.** The runtime cannot demonstrate the borrow rules, so the language's headline memory-safety
guarantee currently lives nowhere executable. A flow-sensitive analysis pass (move/use tracking,
unique `&mut` vs shared `&`, region/scope lifetime nesting, generational-reference rules from §4)
lets the toolchain *reject* programs that violate the rules.

**Why off the critical path but high value.** Self-hosting does not need it (write the compiler to
obey the rules and check it later). But it is the guarantee the spec sells, and codegen (4.2) needs
it to *realize* GC-free memory. It pays off twice: as a rejecter of bad programs, and as the analysis
that tells codegen where to place allocations, frees, and generational checks.

**Done means.** Programs that violate move/borrow/region rules are rejected with real diagnostics
(1.1); the pass emits aliasing/lifetime facts as a reusable artifact (consumed by 3.1 and 4.2);
`DESIGN.md` gains a counterpart to its "not modeled at runtime" note describing the static pass.

---

## Tier 4 — The long arc to self-hosting (critical path resumes)

### 4.1 A type checker in Keel — build the self-hosting subset first

**The key insight: the checker needed to self-host is much smaller than the full spec checker.** To
compile the compiler you need, in roughly this priority:

1. **Name resolution and scoping** — across modules (2.3).
2. **The effect-row check** — every `perform`/`?` must be inside a matching handler or propagate the
   effect outward in the signature. Also the prerequisite for the selective-CPS codegen of 3.2.
3. **Context-string typing** — *this is where the `db.run` stand-in from 1.3 finally becomes a static
   rejection.* "Injection cannot type-check" is the headline guarantee and a runtime abort is only its
   costume. Promoting this check is non-negotiable for the checker to be faithful.
4. **Trust typing** — `Untrusted<T>` cannot flow into a refined or plain target except through a
   validation boundary; `Secret<T>` never renders except via `reveal()`.
5. **Structural/generics-by-structure checking** — enough to type the compiler's own data structures.

Defer past the first self-host: SMT refinement discharge (3.1 — runtime checks self-host fine) and the
ownership/borrow pass (3.3 — write the compiler to obey the rules, check it later). Knowing what to
leave out is what makes 4.1 finishable.

**Use the oracle.** Differential-test the checker: for every program, the Keel checker and the C
interpreter's runtime behavior must agree on accept/reject and on which sink violations are rejected.
Negative conformance cases (C2) are the spec for "what must not type-check."

### 4.2 Code generation — a C transpilation target, deterministic from the first byte

**Target choice.** The spec's readable-C target is the right first backend: far easier than LLVM, the
fastest route to a real compiler, and — because the C oracle already exists and C compilers are
diverse — it sets up the trusting-trust assurance in 4.3. Emit readable, auditable C.

**The constraint that must be designed in, not retrofitted: determinism/reproducibility.** The
self-regeneration fixpoint and diverse double-compilation in 4.3 both *require* bit-for-bit
reproducible output. From the first emitted byte: deterministic gensym (stable symbol counters),
ordered traversal (never iterate a pointer-keyed map into output without sorting), no embedded build
metadata, byte-identical output for identical input. If you make codegen deterministic *later*, you
chase nondeterminism through the whole backend.

**Where the memory model finally becomes real.** Codegen must actually *lower* regions, borrows, and
generational references into C — allocation placement, frees at region exit, generational checks —
which is exactly what the 3.3 ownership pass computes. This is the largest sub-project in the arc, and
the reason 3.3 is worth doing before or alongside codegen rather than after. (A conservative interim: a
tracked-arena or reference-counted fallback to get a working compiler, with ownership-directed
allocation as the optimization that delivers the GC-free promise. Be honest in `DESIGN.md` about which
one a given build uses — and see Part IV, which closes this interim out before 1.0.)

**The runtime linkage (C5).** The emitted C links against the single shared runtime — escapers,
decimals, checked arithmetic, the effect trampoline, capabilities. The escapers especially must be the
*same* implementation the interpreter uses, or `keel run` and the compiled binary diverge on the
security guarantee. Lower effects with the escaping, multi-shot-capable representation fixed in 3.2:
the compiler itself needs only one-shot-in-scope to self-host, but the *representation* must admit
escaping and multi-shot from the first emitted handler so the §3.4 async handler and later backtracking
are turn-on, not rebuild. Gate multi-shot resumes on the ownership/linearity facts from 3.3.

**Done means.** `codegen.keel` emits C that compiles and passes the full conformance suite (C2); output
is byte-identical for identical input; the runtime is shared with the interpreter; effects and (at
least conservatively) memory are lowered.

### 4.3 Bootstrap completion — fixpoint, behavioral equivalence, and a trusting-trust check

This is the moment Stage 0 retires to "reference oracle," per §9. Three distinct checks, because they
prove different things:

1. **The self-regeneration fixpoint.** Stage1 = the Keel compiler compiled by the C oracle. Stage2 = the
   Keel compiler compiled by Stage1. Stage3 = the Keel compiler compiled by Stage2. The check is
   **Stage2 == Stage3**, bit-for-bit: it proves the compiler is a fixed point of itself and flushes out
   any nondeterminism in codegen.
2. **Behavioral equivalence on the conformance suite.** Run the full C2 suite against the self-hosted
   compiler and the C oracle; they must agree on every observable. This certifies the self-hosted
   compiler *means the same language* as the oracle.
3. **Diverse double-compilation (Wheeler's DDC) for the trusting-trust property.** You already own a
   diverse second implementation — the C oracle. Using it to recompile the compiler and confirm the
   result matches defeats a self-reproducing compiler Trojan that source review cannot catch (the
   Camlboot pattern). This is the final, highest form of the oracle's value, and what makes keeping it
   around worthwhile long after Stage 0 "retires."

**Done means.** Stage2 and Stage3 are bit-identical; the self-hosted compiler passes the entire
conformance suite identically to the oracle; optionally a DDC pass confirms freedom from a
trusting-trust attack. The C interpreter's job changes from "the implementation" to "the independent
witness," exactly as §9 envisions.

---

## A suggested single ordering for Part I (the fastest sound route to self-hosting)

1. **C1 spans + C2 conformance harness + C3 AST contract + C4 differential harness** — the
   cross-cutting foundations; small, and everything else leans on them.
2. **1.1 diagnostics with recovery, 1.3 the panic/Fail taxonomy** — finish hardening Stage 0.
3. **2.1 real file I/O** (with real path containment and the authority-flow rule).
4. **2.3 minimal module loader.**
5. **2.2 `parser.keel`**, driven by the feature-backfill loop and checked continuously against the C
   parser via C4.
6. **4.1 the self-hosting subset of the type checker** — name resolution, effect rows, context-string
   typing (promote the `db.run` stand-in to a static error), trust typing, structural checks. Defer
   SMT refinements and ownership.
7. **3.3 the ownership/borrow pass** — pulled earlier than its tier, because 4.2 consumes its facts to
   realize GC-free memory; an interim conservative allocator unblocks codegen if 3.3 lags.
8. **4.2 deterministic C codegen** — reproducible from the first byte; shared runtime (C5); effects
   lowered onto the escaping, multi-shot-capable representation fixed in 3.2.
9. **4.3 bootstrap** — fixpoint (Stage2==Stage3), conformance equivalence, optional DDC via the oracle.
10. **Then harden in Keel, with the oracle watching: 3.1 SMT refinement discharge, and turning on the
    full resumable-continuation surface** — the §3.4 async handler made real, then multi-shot gated by
    3.3's linearity facts. (This hand-off is where Part I ends and Part IV begins.)

## Part I scope traps to resist

- **Building the full refinement solver before self-hosting.** Off the critical path; the runtime
  fallback (3.1, step 2) keeps you sound while you defer it. Z3-first is the specific trap.
- **Welding the continuation mechanism shut.** The trap is picking a representation (live-C-stack
  `setjmp`/`longjmp`) that *precludes* escaping/multi-shot and then paying to demolish it. Choose a
  permissive representation at codegen; the interpreter can stay one-shot-in-scope, documented.
- **Letting the AST churn.** Without the C3 contract and C4 differential tests, every parser change
  silently breaks the checker and codegen.
- **Making codegen deterministic "later."** Determinism is a from-byte-one constraint; 4.3's fixpoint
  depends on it.
- **Two definitions of "escape for SQL."** One shared runtime (C5), or the headline guarantee diverges
  with no failing test.
- **A perfect formatter or a perfect module system.** Both are done when they are good enough for the
  bootstrap, not when they are complete. (Part II finishes them.)

---

# PART II — THE SINGLE-LANGUAGE TOOLCHAIN

The spec is emphatic (§3.12, §9): "one official toolchain (compiler, formatter, linter, package
manager, test runner, doc generator, language server) so the ecosystem never fragments." A
self-hosted compiler is one of those eight; the other seven are what turn a compiler into a thing a
team adopts on a Monday. Build them on the same foundations the compiler already established — spans
(C1), the checker (4.1), the conformance suite (C2), the shared runtime (C5) — so the tools are
*views of the compiler*, not parallel reimplementations that drift.

The unifying principle for the whole toolchain: **every tool is the compiler's frontend reused.**
The formatter is the parser plus the canonical printer; the linter is the checker plus extra passes;
the language server is the checker driven incrementally with results streamed to an editor; the
debugger is the runtime's effect trampoline (C5) with a recording handler attached. Reusing the
frontend is what keeps "the formatter, the IDE, and the compiler agree about your code" true by
construction — the failure mode that plagues ecosystems with three independent parsers.

## P2.1 The interactive tier — REPL, JIT, and hot reload

**Goal.** Deliver the spec's "readable as Python… compiling fast enough to use as a REPL" (§3.11):
an interpreter/JIT tier for instant iteration that shares one source tree with the AOT compiler.

**Why this shape.** The spec's tiered model is *one source, three execution modes*. The Stage-0
interpreter is already the slow-but-correct mode; promote it to the *interactive* mode and let AOT
(Part III) be the *release* mode. A REPL is then the interpreter with incremental top-level binding
and effect handlers wired to real `Io`. Hot reload is the same mechanism the actor model and
content-addressing make safe (§8): a changed definition is a new hash, swapped atomically.

**Sub-tasks.** A line/block REPL with persistent session bindings and capability injection at
startup; a baseline JIT (start by JIT-compiling hot functions through the same C-codegen path, or a
simple bytecode + threaded interpreter) so steady-state interactive speed is acceptable; hot-reload
of a single definition behind the content-addressed identity (Part VI) once it exists, with a
documented interim that reloads at module granularity.

**Gotchas.** The REPL must install the *same* escapers and capability semantics as `run` (C5) or
users learn one security model interactively and meet another in production. Effectful top-level
expressions in a REPL need a default handler set that is honest about authority — a REPL that hands
ambient `Io` to every snippet quietly violates the capability story; gate it explicitly and visibly.

**Done means.** `keel repl` evaluates definitions and expressions incrementally with real effects,
shares the runtime and security semantics of `run`, and reloads changed definitions without a full
restart; the conformance suite gains REPL-transcript cases.

## P2.2 The package manager — content-addressed, capability-sandboxed, semver-enforced

This is the most distinctive and the most load-bearing tool, because the spec makes three claims here
that ordinary package managers do not (§3.12): dependencies carry **no ambient authority**, versions
**cannot lie**, and builds are **reproducible and content-addressed**. Each is a real engineering
commitment, and each closes a class of supply-chain failure the spec names by example (`left-pad`,
PyPI malware, the `xz` backdoor).

**P2.2a Reproducible, content-addressed resolution.** A dependency is identified by the hash of its
content (Part VI), so resolution is exact and verified; two builds of the same manifest produce the
same dependency graph forever. Build the manifest + lockfile + a content-addressed store; make
`keel build` reproducible bit-for-bit (it shares the determinism discipline of 4.2).

**P2.2b Capability-sandboxed dependencies — the move no mainstream manager makes.** The spec: "a
library gets *no ambient authority* and can only do what the importer grants by passing a
capability." This is the capability model (§5.1) extended across the dependency boundary, and it is
*already* enforced by the language if imports never transfer authority (2.3) — the package manager's
job is to make it visible and auditable. The deliverable: a tool that reads a dependency's public
signatures and reports its **total authority** as the union of its effect rows and capability
parameters (this is the §8 "mechanical security auditing" emergent capability, surfaced as a CLI). A
logging library that suddenly takes a `Net` capability shows up in the diff as a new authority demand,
and the importer must grant it explicitly or the build fails. "Can this dependency reach the network?"
becomes `keel audit <dep>`, a type query, not a code read.

**P2.2c Compiler-enforced semantic versioning (Elm's idea).** The compiler diffs a package's public
API across versions and *computes* the correct semver bump, rejecting a "patch" that removed a
function or changed a signature. The API surface is exactly what the checker (4.1) already computes;
the version analyzer is a pass over two compiled interfaces. Authority changes count as breaking:
adding a required capability or a new effect to a public function is a major bump, because it changes
what the function can do. **Version numbers stop lying, and authority creep becomes visible in the
version itself.**

**Gotchas.** Reproducibility is ecosystem-wide or it is nothing: a single dependency that embeds a
timestamp breaks every downstream reproducible build, so the build sandbox must scrub nondeterminism
(time, randomness, environment, filesystem order) the way 4.2 scrubs it in codegen. Capability
auditing is only as honest as the FFI boundary (§3.13, Part VI): a dependency that calls C must have
that C wrapped in a capability-bearing, effectful signature, or it is an authority hole — so the FFI
safety rule and the package manager's audit are the same guarantee viewed twice.

**Done means.** `keel build` is reproducible; `keel add`/`keel audit` show a dependency's full
authority and require explicit grants; the compiler refuses a semver bump that understates an API or
authority change; a conformance-style case proves a malicious "patch" that widens authority is
rejected.

## P2.3 The language server (LSP) — the editor experience the spec's readability claim implies

**Goal.** "Readable as Python" (§1) is a DX claim, and a 2026 language is judged at the cursor:
completion, go-to-definition, find-references, inline diagnostics, type-on-hover, rename, and — Keel's
differentiators — *effect-on-hover* (what can this function do?) and *authority-on-hover* (what
capabilities does it demand?).

**Why it is cheap if Part I was done right.** The server is the checker (4.1) run incrementally with
spans (C1) and streamed over LSP. Rename is *trivially correct* once content-addressing lands
(§3.16): a name is metadata pointing at a hash, so rename touches no hashes and breaks nothing —
exactly the spec's "instant, non-breaking renames." Until then, rename is a spans-driven edit checked
by re-running the checker.

**Sub-tasks.** Incremental re-check on edit (reuse the content-addressed cache so only changed
definitions re-check); diagnostics with the C1 spans and 1.1 error codes; hover that renders the
inferred type *and the inferred effect row*; completion driven by the checker's scope; the
authority/effect surfacing that makes capability review an editor feature, not a chore.

**Done means.** A mainstream editor gets sub-100ms diagnostics on save for a medium module, type and
effect hover, reliable rename and find-references, and the capability/effect surfacing that is unique
to Keel; the server reuses the compiler frontend, with no second parser.

## P2.4 The debugger — deterministic record-replay and time-travel, for free

**Goal.** The spec lists this as an *emergent capability* (§8), not a feature with its own budget:
because every interaction with the outside world is an explicit effect through a handler (§3.3) and
all authority is an explicit capability (§5.1), a "recording" handler logs every effect outcome and a
"replay" handler feeds them back deterministically. Any production run — including a heisenbug — can
be replayed exactly, stepped, and reversed, without a special build.

**Why it belongs in Part II.** It is the single most differentiated debugging story available and it
*falls out of the runtime you already built* (C5's effect trampoline). The work is wiring, not
invention: a recording handler that serializes effect outcomes keyed by perform-site, a replay handler
that returns them in order, and a stepping UI over the recorded trace. Pure code records nothing
(it is reproducible by definition), so the trace is small.

**Sub-tasks.** Define the recording format (effect-site id + outcome); implement record and replay
handlers in the shared runtime; a CLI/LSP stepping interface that walks the recorded effects forward
and backward; integration with the JIT tier (P2.1) so you can replay interactively.

**Gotchas.** Multi-shot effects (3.2) complicate "the outcome at a site" because a site may be
re-performed; key recordings by `(site, resume-count)` so the format survives the Part IV
continuation work. Secrets (§5.3) must never enter a recording — a `Secret<T>` is redacted in the
trace exactly as it is in logs, or record-replay becomes a secret-exfiltration channel.

**Done means.** `keel record`/`keel replay` reproduce a run's effect outcomes deterministically and
allow forward/backward stepping; secrets are redacted in traces; the feature requires no special build
flag.

## P2.5 The formatter completed, and the linter — one canonical layout, no options

**Goal.** The Stage-0 formatter is idempotent on the current surface; complete it to the *full* surface
the self-hosted language grows (Part I's feature-backfill loop) and add the linter as a second pass over
the same checked AST. The spec's rule is absolute: "exactly one valid layout" (§3.14), no options, no
config file — so the formatter has no knobs and the linter's style lints are nonexistent by design
(there is nothing to bikeshed). The linter's job is *semantic*: dead code, unreachable handlers,
unused capabilities (an over-broad authority grant is a lint), shadowing, and refinement hints.

**Why here.** The formatter is the AST's canonical serialization (C3) and the conformance suite already
leans on its idempotence (C2). Completing it keeps `fmt(parse(x))` a trusted fingerprint as the language
grows. The linter shares the checker's passes, so it is incremental work, not a new tool.

**Done means.** `keel fmt` is idempotent and semantics-preserving on the entire language surface, with
no configuration; `keel lint` reports dead code, unused authority, and unreachable handlers using the
checker's facts; both are wired into the conformance suite's property category.

## P2.6 The documentation generator

**Goal.** Generate reference docs from source: signatures (with effect rows and capability demands),
doc comments, and — uniquely — a per-function **authority summary** (its effects + capability
parameters), so a library's docs state exactly what it can do. Examples in doc comments are run as
tests (doctest), certified deterministic by the effect system (§3.8), and cached like any other test.

**Done means.** `keel doc` produces browsable reference docs including effect/authority summaries;
doc examples run as cached tests; the generator reuses the checker's type and effect information.

## P2.7 Incremental build and cache infrastructure

**Goal.** Realize the spec's "almost never waiting on the compiler" (§3.16) for the *steady state*,
ahead of the full content-addressed store in Part VI. Cache compiled definitions keyed by the hash of
their typed AST; recompile only what changed; cache deterministic test results (§3.8) and rerun a test
only when a dependency's hash changes.

**Why split from Part VI.** Full content-addressing (code-as-database, conflict-free renames, no
dependency conflicts) is a larger model shift tracked in Part VI; the *incremental cache* is the
near-term payoff and a prerequisite for a responsive language server (P2.3). Build the cache now with
hashing that is forward-compatible with the eventual store.

**Done means.** A no-op rebuild is instant; changing one definition recompiles and retests only its
dependents; the cache keys are the same hashes the content-addressed store (Part VI) will adopt.

---

# PART III — THE OPTIMIZING NATIVE BACKEND AND PERFORMANCE

Self-hosting via the readable-C target (4.2) proves correctness and buys portability, but "as fast as
C" (§1, §3.11) and "retiring the two-language problem" (§3.10) are performance claims that a transpiler
to unoptimized C will not meet on its own. Part III builds the *release* tier of the spec's tiered
toolchain and makes the numeric story real. None of it is on the self-hosting critical path; all of it
is on the **production** critical path, because a systems language that is not fast is not a systems
language.

## P3.1 The LLVM backend — the release tier

**Goal.** A second backend behind the same lowered IR that the C target consumes, emitting LLVM IR for
genuine optimization (inlining, vectorization, scalar replacement, the works) to native code "as fast
as C." The C target stays — it is the certification/ultra-portability path (§9) and the DDC witness
(4.3) — so this is an *additional* backend, selected per build.

**Why behind a shared IR.** Two backends that lower from the same typed, ownership-annotated IR cannot
diverge in semantics, only in performance; differential testing (C4) extends naturally — the LLVM
binary and the C binary must agree on the conformance suite. This is the same single-source discipline
that protects the escapers (C5), applied to codegen.

**Sub-tasks.** Monomorphize generics at the IR level so they are zero-cost (§3.2) — the checker (4.1)
already resolves the instantiations; codegen specializes them. Lower the ownership facts (3.3) to
stack/region allocation and precise frees so the GC-free promise is realized in optimized code, not
just in the C interim. Lower effects via the selective-CPS representation (3.2) and lean on LLVM to
inline away the handler machinery for the common `Fail`/`Io` cases so effects are zero-cost when not
used. Emit DWARF debug info keyed by the C1 spans so native debugging and the record-replay debugger
(P2.4) line up.

**Gotchas.** LLVM is a large, non-Keel dependency in the *performance* path — acceptable, because it is
not in the *trusted* path (the C target + oracle is). Keep the trust boundary explicit: correctness is
witnessed by the C target and the conformance suite; LLVM is an optimizer whose output is checked
against that witness, never the definition of the language.

**Done means.** `keel build --release` produces optimized native binaries that pass the full
conformance suite identically to the C target and the interpreter; generics are monomorphized; effects
and ownership are lowered without a collector; published benchmarks (P3.2) hit the "as fast as C"
target on the kernels that matter.

## P3.2 Performance as a discipline — a benchmark suite and a regression gate

**Goal.** "Fast as C" is a measurable claim, so measure it. A benchmark corpus spanning the language's
target domains (systems: parsing, hashing, allocation-heavy graph work; numeric: dense linear algebra,
broadcasting kernels; concurrency: task throughput, message passing) with published targets versus C,
Rust, and Go, and a CI gate that fails a regression beyond a threshold.

**Why it is a deliverable, not a chore.** A language's performance reputation is set early and is hard
to recover; a regression gate keeps the claim honest as the compiler evolves, and the benchmark corpus
becomes the optimization to-do list. It also protects reproducibility: benchmarks run in the same
scrubbed-nondeterminism sandbox as builds (4.2, P2.2a).

**Done means.** A public benchmark suite with targets and a CI regression gate; documented results
substantiating the "as fast as C" claim on representative kernels; performance changes are visible in
every PR.

## P3.3 Numeric and array computing — retiring the two-language problem (§3.10)

**Goal.** The spec's promise that "the prototype *is* the production code" requires first-class N-d
arrays with broadcasting and slicing (the interpreter has scalar broadcasting and slicing already),
**portable SIMD as a first-class type** (not an intrinsics swamp), whole-array rank-polymorphic
operations that read as ordinary arithmetic, and the exact `decimal` type for money (already present).
Because pure functions are provably pure (§3.3), the optimizer may parallelize and reorder kernels
safely — a correctness license other languages lack.

**Sub-tasks.** Promote arrays to true N-d (shape + strides) with broadcasting rules; add a portable
SIMD vector type that LLVM lowers to target lanes; make whole-array ops vectorize through the LLVM
backend; expose the purity facts to the optimizer so it can auto-parallelize and reorder kernels;
keep an explicit escape for hand-tuned kernels in `unsafe` for the last percent.

**The stretch: GPU backends.** The spec says "via backends — GPUs." This is post-1.0: the same
rank-polymorphic array code targets a GPU backend (e.g. via the LLVM ecosystem or a dedicated IR),
with the purity guarantee underwriting safe offload. Track it as a frontier item; do not let it block
1.0.

**Done means.** N-d arrays with broadcasting and slicing, a portable SIMD type that vectorizes,
auto-parallelization licensed by purity, and exact decimal — all passing conformance and benchmarked
against NumPy/Julia/Fortran on representative kernels; GPU offload is documented as post-1.0.

## P3.4 `no-runtime` (freestanding) codegen — kernels, bootloaders, microcontrollers (§4.7, §10)

**Goal.** The mode that lets Keel "go where a tracing collector cannot" — drop every assumption of OS,
allocator, and runtime, expose raw pointers, pointer arithmetic, inline assembly, and explicit layout
control (`packed`, alignment, field order) inside `unsafe`, and emit a freestanding binary. This is the
mode that substantiates "one language, whole stack" at the bottom of the stack.

**Why it depends on Part IV's memory work.** Freestanding code has no allocator to fall back on, so the
ownership/region model (3.3) must be real here — the interim arena/refcount allocator is *not available*
on bare metal. This is the hardest forcing function for closing out the interim allocator before 1.0:
embedded targets simply will not accept it.

**Sub-tasks.** A freestanding compilation mode with no runtime dependencies; `unsafe` raw-pointer/MMIO
support and inline assembly; explicit layout attributes for ABI and hardware structs; a minimal
`core`-style subset of the stdlib that needs no OS; documented target setup for an MCU and a kernel
context.

**Done means.** A freestanding "hello, hardware" (e.g. blink an LED on a microcontroller, or a minimal
kernel entry that writes to MMIO) compiles and runs with no runtime; `unsafe` and layout control are
documented with their obligations; the GC-free memory model works without any allocator fallback.

## P3.5 First-class cross-compilation (§3.13)

**Goal.** "Cross-compilation is first-class and trivial." Selecting a target triple and producing a
binary for another OS/arch from any host, with bundled sysroots, so a developer on a laptop ships for a
server, a phone, an MCU, and the browser without leaving the toolchain.

**Why it is cheap on LLVM + freestanding.** LLVM cross-compiles by design; the work is sysroot
management, target-triple plumbing, and making `keel build --target=...` reproducible per target
(P2.2a). It composes with `no-runtime` (P3.4) and WASM (Part VI) as just more targets.

**Done means.** `keel build --target=<triple>` produces a working binary for the named target from any
host, reproducibly; the supported targets include the desktop trio, an embedded triple, and WASM; each
is exercised in CI.

---

# PART IV — REALIZING EVERY GUARANTEE (THE ASSURANCE CONTINUUM, COMPLETED)

`DESIGN.md` is honest that today's interpreter *stands in* for several guarantees: refinements are
runtime checks, ownership is unmodeled, effects are one-shot-in-scope, and a few type errors wear
runtime costumes. Part I promotes the security-critical type errors to static checks and starts the
assurance bets; **Part IV finishes them to production grade and closes the interim allocator.** This
is the part that makes the spec's central sentence — "memory-safety bugs, data races, null
dereferences, integer overflow, injection, path traversal, uninitialized reads, and unhandled errors
are impossible in safe code, enforced by the type system" — true rather than aspirational.

The work happens *in Keel, after the first self-host, with the C oracle watching* — which is the whole
reason Part I deferred it. Each item below has a soundness invariant: **the language is never unsound
in the interim.** A guarantee is either enforced or conservatively checked at runtime; it is never
silently dropped.

## P4.1 Close the memory model — ownership realized, the interim allocator retired

**Goal.** Turn 3.3's analysis pass and 4.2's lowering into the *complete*, enforced, GC-free memory
model of §4: single ownership with moves, shared/unique borrowing, inferred lifetimes at boundaries,
**regions/arenas** for whole-graph lifetimes, **generational references** (`shared<T>`) for safe shared
mutable graphs, and deterministic ordered destruction. `Send`/`Share` are derived and checked so values
cross task boundaries only when safe (§4.6).

**The decisive deliverable: retire the interim allocator.** 4.2 allowed a tracked-arena or refcount
fallback to get a working compiler. That fallback is a development convenience and a *vision risk* — the
spec rejects GC precisely because ownership delivers safety *and* no collector *and* predictable latency
(Appendix C). Before 1.0, ownership-directed allocation must be the default: stack/region placement,
precise frees at scope/region exit, generational checks for `shared<T>`, with no tracing collector and
no mandatory refcounting in safe code. `DESIGN.md`'s "deliberately not modeled at runtime" note is
replaced by "modeled statically and lowered" — and the freestanding target (P3.4) is the proof, because
it has no allocator to fall back to.

**Why it underwrites more than memory.** The move/uniqueness facts this pass computes are exactly what
licenses sound multi-shot effects (P4.3) and what 3.1's refinement VC generator consumes for aliasing.
Ownership is the keystone: memory safety, data-race freedom, and sound resumable effects all rest on it.

**Done means.** Programs violating move/borrow/region/`Send`/`Share` rules are rejected with real
diagnostics (1.1); generational references catch stale access deterministically; destruction is
ordered; the GC-free allocator is the default on every target including freestanding; no safe build
links a tracing collector.

## P4.2 Complete the refinement and proof tier — the top of the continuum (§3.8)

**Goal.** Take 3.1's staged refinement discharge to its full form, then add the proof tier the spec
names: **`total`** (pure *and* proven terminating) and the **provable subset** (SPARK/Dafny-grade) that
verifies a function cannot panic and meets a functional specification — for "avionics, crypto, kernels,
and smart contracts."

**Sub-tasks.** Mature the self-contained decision procedure (3.1) and slot a real SMT backend (Z3/CVC5)
behind the `discharge` interface as the optional accelerator for nonlinear/quantified VCs — never in the
minimal trusted path. Implement termination checking for the `total` effect (structural recursion /
well-founded measures). Add the verification layer that discharges functional-spec obligations, with
manual proof hints where the solver needs them (the spec is candid this is undecidable in general,
§11). Wire refinement-proved facts back to codegen so a proven `b != 0` *removes* the division check
(§5.4, §3.8) — verification that pays for itself in performance.

**Soundness invariant.** Runtime fallback for every `unknown` (3.1, step 2) the whole way up:
verification is opt-in, the solver may time out, and a true property may need a hint — none of which may
ever make the language unsound. Code starts at the level it needs and is promoted as it matters
(§3.8); nothing forces avionics-grade proof onto a prototype.

**Done means.** Common refinements are statically discharged; `total` is checked; the provable subset
verifies panic-freedom and functional specs with optional SMT acceleration; proven facts eliminate
runtime checks in codegen; the runtime fallback keeps everything sound; `DESIGN.md`'s refinement note
reaches its final form.

## P4.3 Complete the effect system — escaping and multi-shot, turned on

**Goal.** Finish 3.2: the codegen continuation representation (selective CPS) is escaping- and
multi-shot-capable from the first emitted handler (4.2); Part IV *turns the surface on*. Escaping
continuations make the §3.4 colorblind async handler real (Part V consumes this). Multi-shot resumes
(backtracking, `amb`, probabilistic inference) are enabled and **gated by P4.1's linearity facts** so a
twice-resumed continuation cannot duplicate an owned resource (Koka's discipline).

**Why it is now safe to enable.** Multi-shot was held back precisely until ownership could license it.
With P4.1 enforced, the type system can certify which resumptions may fire more than once. This is the
payoff of not welding the mechanism shut earlier.

**Done means.** Escaping continuations support real async/generators; multi-shot resumption works where
linearity permits and is rejected where it would duplicate ownership; "full algebraic effects" is no
longer a documented limitation; conformance gains generator, async, and backtracking cases.

## P4.4 The complete security model in the checker — closing the CWE table (§5)

**Goal.** Make every row of the spec's vulnerability-class table a *static* guarantee (or a checked
runtime guarantee where the class is inherently runtime), not a stand-in. Part I closed injection
(4.1's context-string typing). Part IV closes the rest:

- **Path traversal (22)** — rooted, capability-scoped `Path` with real containment (2.1) enforced by the
  type that reaches the filesystem sink, not by a runtime `strstr`.
- **Secret exposure (312/532)** — `Secret<T>` cannot be printed, logged, or serialized; constant-time
  compare; the only exit is a greppable `reveal`. Enforced in the checker, including in the debugger's
  recordings (P2.4) and the doc examples (P2.6).
- **Trust typing (multiple)** — `Untrusted<T>` cannot reach any sink without a validator, enforced
  statically; the interpreter's "parsing sees through Untrusted" convenience becomes an explicit,
  checked validation boundary.
- **Integer overflow (190), bounds (125/787)** — checked arithmetic by default (present); refinements
  (P4.2) *prove away* the checks where provable; bounds elision follows the same proof.
- **Uninitialized use (457/908)** — definite-assignment analysis added to the checker.
- **Unhandled error (754/252)** — the effect-row check (4.1) already forbids dropping a `Fail`; complete
  it so every fallible call is handled or propagated.
- **Data races (362)** — the unique-borrow rule (P4.1) plus `Send`/`Share`, enforced; concurrency that
  shares mutable state across tasks is a compile error.
- **Ambient authority / privilege (269)** — the capability model (§5.1) fully enforced, *including across
  dependencies* (P2.2b): no ambient `Io`, authority only by passed value.

**The `db.run` keystone, finished.** The one stand-in the spec leans on hardest — "concatenating user
input into a query is a *type error*" — is fully static: `db.run` accepts only a `Sql` value, a plain
string is rejected at compile time, and the negative conformance case asserts the rejection with a code
and span. Injection cannot type-check, witnessed.

**Done means.** Every CWE row in the spec table is a static rejection (or a checked runtime guarantee
for inherently-runtime classes), each with a negative conformance case asserting code + span; the
capability model is enforced end to end including across dependency boundaries; `DESIGN.md`'s honesty
notes for these classes are replaced by "enforced."

## P4.5 The fuzzing tier (§3.8, step 4)

**Goal.** "The same harness drives coverage-guided fuzzing of any function from one annotation." Extend
the property runner (present, with shrinking) into coverage-guided fuzzing: instrument the compiled
binary (P3.1) for coverage, drive inputs to maximize it, and shrink failures the way property tests
already do (`n=0`).

**Why it composes.** Fuzzing, property testing, and unit testing are one dial in the spec; they share the
runner, the generators, and the shrinker. Effect-certified determinism (§3.8) makes fuzz findings
reproducible and cacheable.

**Done means.** `test fuzz` (or one annotation on a function) runs coverage-guided fuzzing using the
shared generator/shrinker; findings are deterministic and reproducible; the tier is documented as the
fourth point on the assurance continuum.

## P4.6 No undefined behavior — the audit and the `unsafe` boundary tooling (§3.15, §4.7)

**Goal.** Substantiate "safe Keel has no undefined behavior": every operation is well-defined, a checked
error, or a panic. UB exists only inside `unsafe`, minimized and localized. Provide tooling that
concentrates audit on that small surface — a checker for `unsafe` blocks that lists the obligations the
programmer has assumed, and sanitizer integration (ASan/UBSan/TSan on the C and LLVM targets) for
testing unsafe code.

**Done means.** A documented guarantee, backed by conformance cases, that safe code has no UB; `unsafe`
blocks are enumerable and their obligations are surfaced; sanitizer builds are a supported testing mode;
`DESIGN.md` states the boundary precisely.

---

# PART V — CONCURRENCY AND DISTRIBUTION

The interpreter's `parallel scope` runs tasks inline (`Scope.spawn` is a stand-in) and ships only the
synchronous `Io` handler. The spec promises far more (§3.4, §7.11, §8): colorblind async (the same body
runs sync or async by the caller's handler), structured concurrency by default, the Erlang
actor/supervision model with *statically typed* messages, and production hot code reload. Part V builds
the concurrency runtime on top of P4.3's escaping continuations and P4.1's `Send`/`Share` — it cannot
precede them, because real scheduling *is* "store a continuation and resume it later," and safe sharing
*is* the ownership rules.

## P5.1 The `Io` scheduler — colorblind async, realized (§3.4)

**Goal.** Deliver the spec's defining concurrency claim: "the same body runs sync under a blocking
handler or async under an event-loop handler — the caller decides, so the red/blue split is gone."
Concretely: an `Io` effect whose handler is swappable. A blocking handler runs I/O synchronously; an
event-loop handler suspends the computation at an I/O point (capturing an *escaping* continuation,
P4.3) and resumes it when the I/O completes.

**Why it depends on Part IV.** An async scheduler is precisely "storing a continuation to call later,"
which the one-shot-in-scope interpreter could not express. With escaping continuations turned on
(P4.3) and lowered via selective CPS (4.2), the scheduler becomes a *handler*, not a language change —
exactly the spec's framing. There is no `async`/`await` keyword; asynchrony is which handler `serve`
installs.

**Done means.** A function performing `Io` runs unchanged under a blocking handler and under an
event-loop handler; the flagship example's `serve(sys.net.listen(8080), handler)` runs as a real async
server with one handler and as a synchronous loop with another, same source; conformance cases assert
identical observable results across both handlers.

## P5.2 Structured concurrency runtime (§7.11)

**Goal.** Make `parallel scope s:` actually schedule concurrent tasks (not run inline) under the
structured-concurrency rule: a scope cannot exit until its children finish, so task leaks are
structurally impossible. Add cancellation that propagates through the scope, and deterministic cleanup
(deterministic destruction, §4.5) when a scope unwinds.

**Why it is safe by construction.** Data races are already a compile error via the unique-borrow rule
(P4.1) plus `Send`/`Share` (P4.4) — there is no runtime lock discipline to get wrong. The runtime's job
is scheduling and lifetime, not safety; safety was established statically. Messages between tasks move
ownership (§7.11), so no lock is needed.

**Done means.** `parallel scope` runs children concurrently on the scheduler; a scope blocks until its
children complete; cancellation propagates; values cross task boundaries only when `Send`; the
"structured concurrency" conformance cases pass on a real scheduler.

## P5.3 The actor/supervision library — Erlang's model, statically typed (§3.4, §7.11)

**Goal.** "The Erlang actor/supervision model as a library on the safe core — lightweight processes,
message passing, 'let it crash' — but with statically typed messages the BEAM never had." Build it as a
*library*, not a language feature, on top of the scheduler (P5.2) and ownership-moving messages.

**Sub-tasks.** Lightweight processes (millions, cheap) on the scheduler; typed mailboxes (a message is a
value of a checked type, moved into the actor); supervision trees with restart strategies; the
"let it crash" discipline where a panicking actor is restarted by its supervisor rather than corrupting
shared state (there is none to corrupt — actors share nothing).

**Why typed messages matter.** The BEAM's reliability came with dynamic typing; Keel keeps the
reliability and adds static message typing, so a supervisor and its children agree on protocol at
compile time. This is the spec's explicit improvement on Erlang.

**Done means.** A typed-actor library with supervision trees and restart strategies runs on the
scheduler; messages are statically typed and ownership-moved; a supervised crash restarts cleanly; the
library ships in the stdlib (Part VI) as a capability-honest module.

## P5.4 Production hot code reload (§8)

**Goal.** The spec lists Erlang's "upgrade a running system with zero downtime" as an *emergent*
capability: actor isolation puts all state behind messages, and content-addressed code (Part VI) makes
a new definition a new hash that can be swapped in atomically.

**Why it is post content-addressing.** Atomic swap-by-hash needs the content-addressed store (Part VI);
the actor isolation it relies on is P5.3. So this lands after both — a frontier item, but a tracked one,
because the spec names it.

**Done means.** A running actor system accepts a new version of a definition (a new hash) and migrates
to it without downtime; the mechanism reuses content-addressing and actor isolation rather than a
bespoke reload subsystem; documented with its state-migration obligations.

## P5.5 Distribution and reliability (post-1.0 frontier)

**Goal.** Extend message passing across nodes toward the spec's "million-process concurrency, nine-nines
reliability" aspiration: location-transparent typed messaging, distributed supervision, and partition
handling. This is explicitly post-1.0 and is the largest concurrency sub-project; it builds on
P5.3's typed actors.

**Done means (post-1.0).** Typed actors communicate across nodes with supervision spanning the cluster;
the reliability story is substantiated by chaos/partition testing; documented as a post-1.0 capability,
not a 1.0 gate.

---

# PART VI — STANDARD LIBRARY, INTEROP, TARGETS, AND CODE STORAGE

A language people build advanced systems with needs batteries (a broad stdlib), reach (interop and
every target), and an evolution model that does not fracture the community. The spec commits to all
three (§9, §3.13, §3.16, §3.14, §10). Part VI delivers them, each on the foundations built earlier —
capabilities (§5.1), the effect system (§3.3), and content-addressed identity (§3.16).

## P6.1 The standard library and prelude (§9)

**Goal.** "Broad and modular, auto-imported as a prelude so common work needs no imports — while every
module that confers outside authority exposes a capability type rather than ambient access." The prelude
covers collections, math, text, options/results, time, encoding, the persistent immutable collections,
exact decimal, and N-d arrays/SIMD with no imports; authority-bearing modules (filesystem, network,
process, clock, randomness) expose capability types supplied a value at runtime.

**Design rules that keep it faithful.**
- *Capability honesty is the organizing principle.* Anything that touches the world is a capability, not
  an ambient function. There is no `open(path)` that reads the filesystem from nowhere; there is
  `dir.open(path)` on a `Dir` capability you were handed (§5.1, §7.10). This is what makes "least
  privilege structural."
- *Effects are in every signature.* A stdlib function that performs `Io` or `Fail` says so, so the
  effect-row check (P4.4) and the authority audit (P2.2b) see through the stdlib, not around it.
- *Two collection families.* Value-semantic mutable collections that mutate in place under unique
  ownership (§3.9), and persistent immutable collections for structural sharing — the spec keeps both.
- *Serialization is derivation.* `derive (Json, ...)` synthesizes serializers via compile-time
  reflection (§3.7), so the stdlib's encoding modules are generated, not hand-written per type.
- *Crypto is `Secret`-aware.* Key material is `Secret<T>` (§5.3) end to end; comparisons are
  constant-time; nothing leaks through logs or serialization.

**Sub-tasks.** Specify the prelude surface; implement collections (both families), text/Unicode, time,
encoding (JSON/CBOR/etc. via derivation), math, the exact decimal already present, the N-d array/SIMD
types (shared with P3.3), networking and filesystem as capabilities, and a `Secret`-aware crypto module.
Write the stdlib *in Keel* (it self-hosts), with the shared runtime (C5) underneath for the escapers
and primitives.

**Done means.** A documented, auto-imported prelude; authority-bearing modules each expose a capability
type with no ambient access; collections, decimal, time, text, encoding, arrays/SIMD, networking, and
crypto ship; the stdlib passes conformance and its public API feeds the semver analyzer (P2.2c).

## P6.2 FFI and C interop, both directions, safe at the boundary (§3.13)

**Goal.** "A clean C ABI both directions, and — following Zig — the ability to compile and link C
directly, so Keel drops into a C/C++ codebase file by file." Plus embeddability: callable from and
embeddable in Python and other hosts.

**The non-negotiable: safe by default at the boundary.** "A foreign call is both an `Io`-class effect
and gated by a capability, so even C interop cannot smuggle in ambient authority, and the unsafe raw
call is wrapped in a checked, capability-bearing Keel signature." The raw `extern "C"` call lives in
`unsafe`; the public wrapper is safe, effectful, and capability-gated (the spec's `open_db` example).
This is also what keeps the dependency authority audit (P2.2b) honest: a dependency that calls C must
surface that C behind a capability, or it is an authority hole.

**Sub-tasks.** A C ABI in both directions; direct C compilation/linking (Zig-style) so a mixed codebase
builds with one toolchain; the safe-wrapper discipline enforced by the checker (a bare `extern` call
outside `unsafe` is rejected); a C header generator for calling Keel from C; an embedding API for hosts
like Python.

**Done means.** Keel calls C and C calls Keel; a C file links into a Keel build directly; every foreign
call is capability-gated and effect-typed at its safe boundary; a worked example drops Keel into an
existing C project file-by-file.

## P6.3 The WebAssembly target (§3.13, §10)

**Goal.** "Targets WebAssembly for the browser and edge." A WASM backend (via LLVM, P3.1) producing
modules that run in browsers and edge runtimes, with the capability model mapped onto WASM's import
boundary — host functions are the capabilities, so the sandbox and the language's authority model
reinforce each other.

**Done means.** `keel build --target=wasm` produces a working module exercised in a browser and an edge
runtime; capabilities map to WASM imports; conformance runs on the WASM target.

## P6.4 Content-addressed code storage (§3.16) — the radical one

**Goal.** Adopt Unison's model: a definition is identified by the hash of its typed AST; names are
metadata pointing at hashes; code is stored as ASTs in a database with a readable text projection. The
payoffs are the ones the spec calls non-incremental: **no builds** (perfect-incremental against the
shared cache, extending P2.7), **instant non-breaking renames** (a name is metadata, so rename touches
no hashes — this is what makes the LSP's rename trivially correct, P2.3), **no dependency-version
conflicts** (two deps needing different versions of a third reference different hashes and coexist),
**perfect test caching** (a deterministic test, effect-certified, reruns only if a dependency hash
changed), and trivial reproducibility (a hash *is* a verified identity, underpinning P2.2a).

**Honest about the cost (§11).** The code-as-database model is unfamiliar and demands tooling; the spec
mitigates with a "plain-text projection so the source is still readable and diffable." Keep that
projection first-class — `fmt` is already that projection (C3) — so developers edit and review text
while the store holds ASTs. Keel still emits ordinary AOT-native binaries (§3.11); the hash identity is
a source-and-cache concern, not a runtime one.

**Version control on definitions.** With content-addressing, the spec's "version control that operates
on definitions (content-addressed diffs, conflict-free renames)" becomes available: diffs are over the
definition graph, renames never conflict. Build this as the VCS integration the toolchain (§9) lists.

**Done means.** Definitions are stored and addressed by typed-AST hash with a readable text projection;
rename is non-breaking and conflict-free; conflicting transitive versions coexist; deterministic tests
and compilation are cached against hashes; the model's cost is documented and mitigated by the text
projection.

## P6.5 Editions and language evolution (§3.14)

**Goal.** "A module declares its edition, old editions compile forever, and editions interoperate —
decades of change with no schism and no ballast." This is the institutional answer to Python's 2→3
break and C++'s permanent baggage, and it is what lets a *production* language change without breaking
deployed systems.

**Sub-tasks.** An edition declaration per module; the compiler supports multiple editions simultaneously
and links them; a documented policy for what may change across an edition boundary (syntax and defaults
may; the core type system and ABI guarantees may not break interop); migration tooling that mechanically
upgrades a module to a newer edition where the change is mechanical.

**Done means.** Modules of different editions compile and interoperate in one build; an old edition keeps
compiling after a new one ships; a documented edition policy and a migration tool exist; the conformance
suite includes cross-edition cases.

## P6.6 Mobile targets (§10)

**Goal.** "From one codebase: mobile (Android/iOS)." Native libraries/binaries for Android and iOS via
the LLVM backend (P3.1) and cross-compilation (P3.5), with the platform glue (NDK/JNI on Android,
Objective-C/Swift interop on iOS) behind capability-gated FFI (P6.2).

**Done means.** `keel build --target=<android|ios>` produces a working artifact callable from a native
app; platform APIs are reached through capability-gated FFI; documented with a worked example.

## P6.7 The quantum circuit-description library (§10) — post-1.0 frontier

**Goal.** The spec is precise and modest here: quantum machines are coprocessors, so Keel does not "run
on" them. It provides "a hosted, type-checked circuit-description library: qubits, gates, and
measurement are Keel values compiled to a vendor-neutral IR (e.g. QIR) and dispatched to a simulator or
hardware backend, with classical control flow staying in ordinary Keel." This is explicitly
forward-looking, not a 1.0 gate.

**Done means (post-1.0).** A typed circuit-description library lowers to QIR, runs on a simulator and at
least one hardware backend, with classical control in ordinary Keel; documented as the hybrid,
post-1.0 capability the spec describes — neither more nor less.

---

# PART VII — PRODUCTION READINESS, ASSURANCE-AT-SCALE, AND GOVERNANCE

A language becomes *production-grade* not when the compiler works but when teams can rely on it:
stability they can plan around, a security process they can trust, observability they can operate,
documentation they can learn from, and governance that keeps the ecosystem coherent. The spec's
emergent capabilities (§8) and its single-toolchain commitment (§9) point directly at this work.

## P7.1 The language standard — the conformance suite, published as normative

**Goal.** Elevate the conformance suite (C2) from internal oracle to the **published, versioned language
standard**. Because it was built implementation-agnostic from day one and already runs against the C
oracle and the self-hosted compiler identically (4.3), it *is* the definition of Keel — the artifact
any third implementation must pass to call itself Keel.

**Done means.** A versioned standard whose normative content is the conformance suite plus the
specification prose; a documented process for proposing changes (P7.7); the claim "implementation X is
Keel" reduces to "X passes the suite."

## P7.2 Stability and compatibility policy

**Goal.** Make the spec's "evolvable without breaking" (§1, §3.14) an institutional guarantee. Define
the stability tiers (stable / experimental / deprecated), the edition cadence (P6.5), the deprecation
policy (how long an edition is supported), and ABI stability for the C FFI boundary (P6.2) so compiled
artifacts and foreign callers can depend on it.

**Why it is a 1.0 gate.** "Production" means a team can deploy and plan around the language for years.
Compiler-enforced semver (P2.2c) gives this *per library*; the edition policy gives it *for the language
itself*. Together they are the spec's "decades of change with no schism."

**Done means.** A published stability policy with tiers, an edition support window, and an ABI stability
commitment; experimental features are flagged as such in the toolchain; the policy is enforced by
tooling (a stable build refuses experimental features without an opt-in).

## P7.3 Security: response process and supply-chain assurance at scale

**Goal.** The language closes defect *classes* by construction (§5), but a production ecosystem also
needs a *process* for the residual: a security-response procedure, an advisory database, and
supply-chain assurance across the whole dependency graph, not just one package.

**Sub-tasks.** A coordinated-disclosure process and advisory feed; ecosystem-wide reproducible builds
(P2.2a) so a published artifact provably matches its source; capability auditing (P2.2b) surfaced at the
registry so a dependency's authority is visible before install; **diverse double-compilation as ongoing
practice** (4.3) — the C oracle remains the independent witness against a trusting-trust attack on the
self-hosted compiler, which is why the spec keeps it as a "reference oracle" rather than discarding it.

**Done means.** A documented security-response process and advisory database; registry-level authority
audits; reproducible builds across the ecosystem; periodic DDC of the compiler using the C oracle as the
diverse witness.

## P7.4 Production observability — tracing as a handler, not instrumentation (§8)

**Goal.** The spec's emergent capability: "because effects are explicit and typed, the runtime can trace
every effect — every I/O, every failure, every span — as structured, correlated telemetry with no manual
instrumentation. Tracing becomes a handler, not a library sprinkled through your code." Deliver
production tracing, metrics, and structured logging as *effect handlers* installed at the boundary.

**Why it is nearly free.** It is the same mechanism as the record-replay debugger (P2.4): a handler that
observes effect outcomes. In production it emits telemetry instead of a replay log. Mechanical effect
auditing (P2.2b) is the static counterpart — the signature tells you what a component can do; the tracing
handler shows you what it did.

**Done means.** A tracing/metrics/logging handler set that produces correlated, structured telemetry for
every effect with zero manual instrumentation; secrets are redacted (P4.4); the feature is documented as
the operational face of the effect system.

## P7.5 High-assurance and certification

**Goal.** Make real the spec's promise that the provable subset (§3.8) serves "avionics, crypto, kernels,
and smart contracts (where a proven-terminating contract cannot exhaust gas)." Provide a documented
certification path: the readable-C transpilation target (4.2, §9) for audits and certification regimes
that require reviewable C; the verification tier (P4.2) for functional-spec and panic-freedom proofs; and
guidance for the regulated domains the spec names.

**Done means.** A documented high-assurance workflow combining the provable subset and the readable-C
target; at least one worked example in a regulated style (e.g. a proven-terminating, panic-free
component); the certification story is substantiated, not merely claimed.

## P7.6 Documentation and learning

**Goal.** Honor the "readable as Python" promise (§1) with learning materials to match: a complete
reference manual (generated where possible from source, P2.6), a book-length guide that teaches the
*orthogonal* concepts the spec bets on (effects, regions, refinements, content-addressing) as fewer ideas
than the union they replace (§11), an indexed error-code catalogue (the 1.1 codes, each with an
explanation and a fix), migration/interop guides for incoming C/Rust/Python/Go developers, and a
browser **playground** (the REPL, P2.1, compiled to WASM, P6.3).

**Why it is a 1.0 gate.** The spec's own honest-limits section (§11) admits the learning curve is real;
the bet is that the concepts are *fewer and more orthogonal*. Documentation is how that bet is won or
lost in practice. A language no one can learn is not production-ready regardless of its guarantees.

**Done means.** A reference manual, a guide that teaches the core concepts, an error-code index, interop
guides, and a WASM playground; the editor experience (P2.3) plus the docs make the readability claim
credible to a newcomer.

## P7.7 Ecosystem and governance

**Goal.** Keep the ecosystem coherent (the spec's reason for one official toolchain, §3.12) with a
lightweight governance model: an RFC process for language and stdlib changes (feeding P7.1's standard),
a package registry (with the capability audit and semver enforcement of P2.2 built in), a curated set of
seed libraries for common domains, and a stewardship model that resists fragmentation.

**Done means.** A documented RFC/change process; a running registry with authority audits and enforced
semver; a seed set of key libraries; a stated governance model — the institutional version of "the
ecosystem never fragments."

---

# THE MASTER ORDERING (all parts, dependency-respecting)

A single timeline from the current interpreter to 1.0 and beyond. Each milestone names its hard
dependencies so the ordering is forced, not arbitrary.

**Milestone A — Self-hosting (Part I).** The Part I single ordering, ending at the three-stage fixpoint
(4.3). Output: Keel compiles Keel; the C interpreter becomes the oracle. *No production claims yet.*

**Milestone B — A usable toolchain on the self-hosted compiler (Part II).** Interactive tier (P2.1),
formatter+linter completed (P2.5), language server (P2.3), incremental cache (P2.7), doc generator
(P2.6). *Depends on:* the self-hosted frontend (A). The package manager's *resolution* (P2.2a) lands
here; its *semver* (P2.2c) and *capability audit* (P2.2b) firm up as the checker completes (D).

**Milestone C — The release backend and performance (Part III).** LLVM backend (P3.1), benchmark gate
(P3.2), numeric/SIMD (P3.3), cross-compilation (P3.5). *Depends on:* the shared IR from codegen (4.2) and
ownership facts (3.3). `no-runtime` (P3.4) lands with D's memory work.

**Milestone D — Guarantees realized (Part IV).** Close the memory model and retire the interim allocator
(P4.1), complete refinements+proof tier (P4.2), turn on escaping+multi-shot effects (P4.3), close the CWE
table in the checker (P4.4), fuzzing (P4.5), no-UB audit (P4.6). *Depends on:* the self-hosted checker
(4.1) and codegen (4.2). **This is the milestone that converts "a language" into "the language the spec
promises."** P3.4 freestanding gates on P4.1 here.

**Milestone E — Concurrency (Part V).** `Io` scheduler / colorblind async (P5.1), structured concurrency
runtime (P5.2), typed actors (P5.3). *Depends on:* escaping continuations (P4.3) and `Send`/`Share`
(P4.1/P4.4). Hot reload (P5.4) gates on content-addressing (F).

**Milestone F — Batteries, reach, evolution (Part VI).** Standard library (P6.1), C FFI (P6.2), WASM
(P6.3), content-addressed store + VCS-on-definitions (P6.4), editions (P6.5), mobile (P6.6). *Depends on:*
the self-hosted language (A) for the stdlib-in-Keel, the LLVM backend (C) for WASM/mobile, and the
capability model (D) for FFI safety.

**Milestone G — 1.0 production readiness (Part VII).** Published standard (P7.1), stability policy
(P7.2), security process (P7.3), observability (P7.4), certification path (P7.5), docs+playground (P7.6),
governance+registry (P7.7). *Depends on:* essentially all of B–F, because production readiness is the
integral of the guarantees, the tools, and the ecosystem.

**Post-1.0 frontier.** GPU array backends (P3.3), production hot code reload (P5.4), distribution/
nine-nines (P5.5), the quantum circuit library (P6.7). The spec names each; none gates 1.0.

The reordering versus a naive easy-to-hard list is the same lesson Part I taught, applied at the
program scale: **reach self-hosting before hardening guarantees (A before D), but do not ship 1.0
without the guarantees** (D is a 1.0 gate, not optional), because self-hosting proves the compiler works
and the guarantees are what make the language *Keel*.

# Production-era scope traps to resist

The Part I traps still apply within Milestone A. Beyond it:

- **Treating self-hosting as the finish line.** It is Milestone A of seven. "Keel compiles Keel" with
  runtime-checked refinements, no ownership enforcement, and a single synchronous handler is a research
  result, not a product. Milestone D is where the spec's promises become real.
- **Letting the interim allocator become permanent.** The arena/refcount fallback (4.2) is a development
  stand-in with an expiry date (P4.1). Shipping it as the default 1.0 allocator would forfeit the
  GC-free property that is the spec's reason to exist. The freestanding target (P3.4) is the forcing
  function — it has no allocator to fall back to.
- **Welding effects shut at codegen.** Identical to Part I's trap but now permanent: choose the escaping,
  multi-shot-capable representation at 4.2 or pay to demolish a shipped backend. Colorblind async (P5.1)
  and multi-shot (P4.3) both depend on the choice made once, early.
- **A second definition of any security primitive.** As the stdlib, FFI, and compiled runtime multiply
  the places escapers/decimals/checked-arithmetic live, the C5 single-source rule must hold across all
  of them, or the headline guarantees diverge silently. One runtime, written in Keel, compiled once,
  shared everywhere.
- **Optimizing before measuring, or shipping the "fast as C" claim unmeasured.** The benchmark gate
  (P3.2) is what keeps the performance claim honest; without it, performance regresses invisibly and the
  central claim rots.
- **Adding a configuration knob to the formatter, or a dialect to the language.** The spec's "exactly one
  valid layout" and "one official toolchain" are cultural commitments (P2.5, P7.7); the trap is a
  thousand small concessions that fragment the ecosystem the toolchain exists to keep coherent.
- **Postponing documentation to "after 1.0."** §11 concedes the learning curve is real; the orthogonality
  bet is won by teaching (P7.6), and a language no one can learn is not production-ready however sound.
- **Discarding the C oracle once self-hosted.** Its highest value (diverse double-compilation against
  trusting-trust, 4.3/P7.3) comes *after* it "retires." Keep it as the independent witness, exactly as §9
  frames it.

# Closing: one discipline, end to end

The same idea runs from the first span to the 1.0 standard: **a trusted definition of correct behavior,
and continuous evidence that each implementation agrees with it.** In Part I that definition is the C
oracle and the evidence is differential testing; at 1.0 the definition is the published conformance suite
and the evidence is that the interpreter, the C target, and the optimized native backend all pass it
identically — with the C oracle kept on as the independent witness the spec always intended. Every
guarantee the spec sells is preserved on that thread, never traded away: staged but never weakened
(refinements), permitted but never welded shut (effects), deferred but never abandoned (ownership), and
single-sourced so it cannot silently diverge (the security primitives). That is what it means for this
roadmap to be ambitious without diverging — it goes to the extremes the challenge asks for while holding,
at every step, to the language the specification describes.
