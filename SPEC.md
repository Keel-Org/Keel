# Keel — Language Design

*This is the language design and specification: the thesis, the type / effect /
memory / security model, a runnable tour, the reference essentials, and the
appendices. For building and running the implementation see
[README.md](README.md); for how the implementation works and what it defers,
[DESIGN.md](DESIGN.md); for the self-hosting milestone, [PART1.md](PART1.md);
for the staged build plan, [ROADMAP.md](ROADMAP.md).*

Keel is a general-purpose, statically typed, ahead-of-time compiled language built on a
single wager: most of the pain in programming is not inherent but historical — the accreted
cost of compromises (a garbage collector here, a null pointer there, a textual macro, an
invisibly-unwinding exception), each reasonable at the time and none of which we still have
to accept. Keel takes the single best idea from every widely used language and combines them
into one coherent system, while making the corresponding worst idea *impossible to express*.

The result is memory-safe without a garbage collector, free of undefined behavior in safe
code, closes whole vulnerability classes at the type level, eliminates the function-coloring
problem, tracks effects and errors in function types, proves optional correctness properties
at compile time, stores code so that builds and dependency conflicts disappear, and compiles
fast enough to use interactively while emitting native code as fast as C. It is meant to be
the *only* language a team needs across the whole stack — kernel to script to browser —
retiring the two-language problem rather than living inside it.

**This repository contains the language design** (below, and in full in the spec) **and a
working implementation that is now self-hosting.** There is a **Stage-0 reference
interpreter** in C — the oracle that defines correct behavior — and a **Keel→C compiler
written in Keel** that compiles itself to a byte-identical fixpoint and agrees with the
interpreter on a behavioral-equivalence corpus. The interpreter runs the conformance and equivalence corpora, formats Keel canonically, runs property tests with shrinking, enforces real
capability containment, and closes the injection class with a single shared escaper. A short,
honest account of *what is realized today versus what is deferred to a later part* runs
alongside each section and is consolidated in `DESIGN.md` and `PART1.md`.

---

---

## 1. Design thesis

The 2026 landscape is instructive. Python leads on readability and an unmatched scientific
ecosystem but is slow and dynamically unsafe. C and C++ own performance and control but bleed
memory-safety CVEs. Rust proved memory safety without a garbage collector is possible — yet
its learning curve, borrow-checker friction, async story, and compile times are its
most-cited complaints. TypeScript bolted static types onto JavaScript, but the types are
erased and unsound. Go won on simplicity, fast builds, and single-binary deployment while
frustrating everyone with `if err != nil` and a weak type system.

Each is a *partial* answer. Keel's goal is their union under one consistent rule set:

1. **Zero defect classes by construction** — memory-safety bugs, data races, null
   dereferences, integer overflow, injection, path traversal, uninitialized reads, and
   unhandled errors are impossible in safe code, enforced by the type system rather than by
   discipline or tooling.
2. **No garbage collector, no undefined behavior** — deterministic, predictable,
   real-time-capable, embeddable in a kernel.
3. **One language, whole stack** — the same source serves firmware, operating systems,
   backends, scientific computing, the browser (WebAssembly), and interactive scripting.
4. **Readable as Python, fast as C, safe beyond Rust** — compiling fast enough to use as a REPL.
5. **Evolvable without breaking** — decades of change with no Python-2-to-3 schism.

---

## 2. Design lineage: what Keel keeps and what it eliminates

For each major language, the one strength Keel doubles down on and the one weakness it
deletes. The "how" is detailed in §3.

### Systems languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| C | Minimal core, stable ABI, total control, universal portability | Undefined behavior, memory unsafety, no modules, weak types |
| C++ | Zero-cost abstractions, RAII, deep expressiveness | Accidental complexity, UB, template error walls, slow compiles |
| Rust | Memory safety without GC, traits, fearless concurrency, cargo-grade tooling | Borrow-checker friction, lifetime verbosity, `Rc<RefCell>` ceremony, async coloring, slow builds |
| Zig | `comptime`, colorblind async, superb C interop, no hidden control flow/allocation | No compile-time memory-safety guarantee |
| D | Powerful compile-time metaprogramming, fast compilation | Optional-but-pervasive GC that split the community |
| Nim | Python-like syntax compiling to native | Small ecosystem, macro complexity |
| Ada / SPARK | Strong typing, real-time guarantees, a *formally provable* subset | Verbosity, dated tooling, niche isolation |

### Managed / enterprise languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Java | Write-once portability, vast ecosystem, virtual threads | GC pauses, `null` (NPE), type erasure, boilerplate, slow startup |
| C# | LINQ-style expressiveness, value types, async pioneer | GC, async function coloring, runtime dependency |
| Kotlin | Null safety, conciseness, structured concurrency | JVM dependency, GC, compile speed |
| Go | Fast builds, single static binary, lightweight concurrency, batteries-included stdlib | GC latency, `if err != nil` ceremony, anemic types, `nil` |
| Swift | Value semantics, protocols, safety, deterministic ARC | Apple-centricity, ARC overhead and retain cycles, compile speed |

### Dynamic / scripting languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Python | Readability, batteries-included, dominant AI/ML/science ecosystem, REPL | Runtime speed (GIL), dynamic-typing bugs, packaging hell |
| JavaScript | Universal browser reach, event-loop concurrency | Type-coercion insanity, no static guarantees, npm supply-chain disasters |
| TypeScript | Structural static typing, superb editor DX | Erased, unsound types; still on the JS runtime |
| Ruby | Developer happiness, expressiveness, metaprogramming | Slow execution, GC, dynamic-typing bugs |

### Functional languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Haskell | Typeclasses, higher-kinded types, purity, correctness | Lazy-evaluation space leaks, opaque performance, GC |
| OCaml | Fast inference, ML modules, *algebraic effects* (OCaml 5) | Small ecosystem, dated syntax/tooling, GC |
| Koka / Eff / Effekt | Effects + handlers as the *one* general control mechanism; the `total` effect | Research-only maturity, tiny ecosystems |
| Erlang | Actor model, supervision, hot code reload, nine-nines reliability | Dynamic typing, weak per-core throughput |
| Lisp / Scheme | Homoiconic macros, the *condition/restart* system, REPL | Untyped/unhygienic macros, runtime-only errors |
| Unison | **Content-addressed code**: no builds, instant renames, no dependency conflicts | Bespoke runtime, immature ecosystem |

### Scientific, array, declarative, low-level

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Julia | Solves the two-language problem, multiple dispatch, native arrays/broadcasting | JIT warm-up latency, GC, large binaries |
| Fortran | Numerical performance, first-class arrays, no-alias guarantees | Dated ergonomics, isolation |
| APL / J / K / q | Whole-array rank-polymorphic operations, extreme concision | Cryptic glyph soup, unreadability |
| COBOL | Exact decimal arithmetic, longevity | Verbosity, archaic ergonomics |
| SQL | Declarative, optimized set operations | Dialect fragmentation, string-built injection |
| Assembly | Ultimate control, peak performance, direct ISA access | Non-portability, no abstraction, no safety |

Assembly's strengths are captured without making the language unsafe: inline assembly and
raw hardware access live in `unsafe` and `no-runtime` mode (§4.7, §9), comptime gives
zero-overhead abstraction, and everything outside those blocks stays portable and safe.

---

## 3. The fundamental concepts

Each subsection states the spectrum the field occupies, the failure modes at its ends, and
Keel's resolution. Where the Stage-0 interpreter realizes a concept directly, or stands in
for it pending the self-hosting compiler, that is noted in italics.

### 3.1 Memory management

**Spectrum.** Manual (`malloc`/`free`) → tracing GC (pauses, no determinism) → reference
counting/ARC (cycle leaks) → ownership/borrowing (safe, no GC, but ergonomic friction).

**Resolution.** Compile-time ownership and borrowing with deterministic destruction is the
base — the only known way to get safety *and* no GC *and* predictable latency — with Rust's
three biggest ergonomic costs removed: lifetimes are **inferred** except at published
boundaries; **regions/arenas** give whole graphs one shared lifetime freed at once; and
**generational references** (`shared<T>`) make shared mutable graphs safe by catching a stale
access deterministically rather than corrupting memory. Ownership when natural, regions when
ownership is awkward, generational sharing when graphs demand it; all safe, all GC-free.

*Interpreter: ownership/borrowing is a compile-time discipline, and a tree-walking
interpreter has nothing to be unsafe about, so borrows evaluate to their referent and
`region`/`scope` blocks simply execute. The interpreter itself is GC-free (a tracked arena
freed at exit), faithful to the language's no-collector stance, but it neither demonstrates
nor enforces the borrow rules — that is the future type checker's job.*

### 3.2 Type system and inference

**Spectrum.** Dynamic (fails at runtime) → unsound static (Java/TS erasure) → sound static
(Rust/OCaml/Swift) → dependent/refinement (provable, but heavy).

**Resolution.** Sound, static, *inferred* typing with dynamic-language ergonomics. Local
inference means most code carries no annotations while every type is checked and never
erased. Generics are real, monomorphized, and zero-cost, with trait constraints checked
*before* instantiation so an error names the unmet constraint instead of dumping a template
trace. Structural typing sits beside nominal typing, chosen per declaration. An optional
refinement layer (§3.8) adds compile-time proofs for code that wants them.

### 3.3 Algebraic effects — the unifying mechanism

**The problem.** Exceptions, async/await, generators, dependency injection, and dynamic
scoping are the same thing in different costumes: a computation that suspends and yields
control to an outer handler. Treating them separately is why async colors functions, why
exceptions are invisible control flow, and why faking I/O for tests needs a mocking
framework. Koka's "minimal but general" principle showed one mechanism subsumes them all.

**Resolution.** One **algebraic effects and handlers** system underlies everything. A
function's type records the effects it may perform; the caller's installed handler decides
what those effects *do*.

```keel
def read_config(dir: Dir); Config / {Io, Fail<ConfigError>}:
    text = dir.open("config.toml")?      # `?` performs Fail; `open` performs Io
    return parse(text)?
```

Effects are **inferred** within a module and written only at boundaries, so everyday code
stays clean. From this one mechanism Keel derives error propagation (§3.5), colorblind async
(§3.4), iterators, scoped configuration, and framework-free testable I/O. A function whose
effect set is empty is **`total`**: pure *and* proven to terminate, safe to memoize, reorder,
and parallelize.

*Interpreter: implemented as a genuine effect mechanism using the setjmp/longjmp
handler-stack technique. Performing `Fail` searches outward for the nearest matching handler;
a handler clause may fall off the end (aborting to the `execute` site with its value) or
evaluate `resume v` (continuing the original computation). Both are implemented and exercised by the test corpus. The honest limit is that resumptions are one-shot and in-scope,
not multi-shot continuations — sufficient for `Fail`, condition/restart, and `Io`.*

### 3.4 Concurrency and async

**Spectrum.** Threads + locks (races, deadlocks) → async/await (function coloring) → green
threads (leaks, races) → actors/CSP (safe messages, less control) → colorblind async.

**Resolution.** Data races are a compile error by construction — the unique-borrow rule (§4)
forbids two tasks from holding mutable access to one value. On top of that: **no function
coloring** (asynchrony is the `Io` effect; the same body runs sync under a blocking handler
or async under an event-loop handler — the caller decides); **structured concurrency by
default** (tasks spawn into a `scope` that cannot exit until its children finish); and the
**Erlang actor/supervision model** as a library on the safe core, with statically typed
messages the BEAM never had.

*Interpreter: `parallel scope` and `spawn` parse and run sequentially under a `Scope`
capability — the structured-concurrency shape is present; the scheduler is not.*

### 3.5 Error handling

**Spectrum.** Error codes (silently ignorable) → exceptions (invisible, expensive) →
`Result`/`Option` values → conditions/restarts (Lisp: recover *at the error site* without
unwinding — the most powerful model, almost universally forgotten).

**Resolution.** Errors are values via the `Fail<E>` effect; a fallible function says so in
its type and a failure cannot be silently dropped. `?` propagates with no ceremony. And Keel
revives **condition/restart** through effect handlers: a handler can inspect a failure and
`resume` the computation at the point it failed with a supplied value — retry, substitute,
skip — recovery exceptions cannot express.

*Interpreter: `?` propagation, `execute`/`handle`, and `resume` all work; a function boundary
converts an unhandled `Fail` into a propagating failure value.*

### 3.6 Null and absence

**Resolution.** No null. Absence is `T?`, unusable as `T` until checked; `?.` accesses through
it and `??` supplies a default. Null dereference ceases to exist as a category.

*Interpreter: `T?`, `?.`, and `??` are implemented.*

### 3.7 Metaprogramming, generics, reflection, and derivation

**Spectrum.** Textual macros (unhygienic, type-blind) → templates (Turing-complete but
unreadable errors) → hygienic macros → compile-time execution (`comptime`) → *runtime*
reflection (flexible but costly).

**Resolution.** Everything happens at **compile time**, at zero runtime cost: `comptime` runs
ordinary Keel during compilation (no separate macro dialect); typed hygienic macros operate
on the *typed* AST; compile-time reflection inspects types to generate serializers, ORMs, and
RPC stubs with no runtime tax; and **derivation** is the everyday face — `derive (Eq, Hash,
Json, Debug)` synthesizes implementations from a type's structure.

```keel
derive (Eq, Hash, Json)
struct Point:
    x: int
    y: int
```

*Interpreter: `derive (...)` parses and is recorded on the type; `comptime` blocks parse and
evaluate immediately; full staged compilation and typed macros are out of scope for Stage 0.*

### 3.8 The assurance continuum — testing, refinement, and proof

Keel treats testing, static typing, and formal verification as **one continuous dial** of
confidence, so you pay exactly as much rigor as a given piece of code warrants:

1. **Types** — the baseline; sound and non-erased.
2. **Unit tests** — first-class `test` blocks beside the code, run by the integrated runner;
   deterministic ones are cached perfectly via the effect system + content-addressing.
3. **Property-based tests** — `test prop` states a law over generated inputs; the runner
   searches for and shrinks counterexamples.
4. **Fuzzing** — the same harness drives coverage-guided fuzzing from one annotation.
5. **Refinement types** — predicates (`int where != 0`, `array[T] where len > 0`) discharged
   by a built-in SMT solver, *proving* at compile time that division-by-zero, out-of-bounds,
   overflow, and broken invariants cannot occur.
6. **Totality and full proof** — the `total` effect proves termination; the provable subset
   verifies a function cannot panic and meets a functional specification.

```keel
test prop "reverse is its own inverse" (xs: array[int]):
    assert reverse(reverse(xs)) == xs

def divide(a: int, b: int where != 0); int:
    return a / b        # the compiler proved b != 0; no check, no panic
```

*Interpreter: `test` and `test prop` are first-class; the property runner generates inputs by
type, runs ~100 cases, and shrinks counterexamples, minimizing a deliberately false property to its boundary. Refinement types are the one important
**stand-in**: predicates are checked **at runtime** at binding, construction, and validation
boundaries, not discharged by an SMT solver — the observable rejection at the boundary
matches, but it is a dynamic check, not a static proof. Fuzzing and the proof tier are not
implemented.*

### 3.9 Mutability, aliasing, and value semantics

**Resolution.** Immutable bindings by default; values have value semantics — but because
ownership tracks unique access, Keel mutates **in place** when it holds the only reference.
You get Clojure's safety with C's update speed. Persistent immutable collections remain in
the stdlib for code that wants structural sharing.

*Interpreter: immutable-by-default with `mut` for reassignment is enforced; in-place mutation
optimization is moot in a tree-walker.*

### 3.10 Numeric and array computing — retiring the two-language problem

**Resolution.** First-class N-dimensional arrays with **broadcasting** and slicing compile to
vectorized native code; **portable SIMD** is a first-class type; whole-array rank-polymorphic
operations read as ordinary arithmetic; an **exact decimal** type makes money math correct by
default. Because pure functions are provably pure, the optimizer parallelizes and reorders
kernels safely.

*Interpreter: array broadcasting (`[1,2,3] * 10`), slicing (`xs[lo -> hi]`), and an exact
`decimal` type (scaled-integer arithmetic) are implemented; SIMD and GPU backends are not.*

### 3.11 Compilation model and speed

**Resolution.** A **tiered** toolchain from one source tree: a built-in interpreter and JIT
give Python-grade interactivity — REPL, hot reload, instant tests — while release builds
AOT-compile through LLVM to native as fast as C. Content-addressed storage (§3.16) takes
incremental compilation to its limit.

*Interpreter: this repository is the interpreter tier — the readable oracle, by design. There
is no JIT or AOT backend yet.*

### 3.12 Tooling, packaging, and supply-chain security

**Resolution.** One official toolchain (compiler, formatter, linter, package manager, test
runner, doc generator, language server). Beyond reproducible, content-addressed builds:
**capability-sandboxed dependencies** — a library gets *no ambient authority* and can only do
what the importer grants by passing a capability (§5.1) — and **compiler-enforced semantic
versioning** — the compiler diffs a package's public API across versions and *computes* the
correct semver bump.

*Interpreter: ships `run`, `test`, and the canonical `fmt`. The package manager and semver
analysis are future work.*

### 3.13 Interop, FFI, and adoption

**Resolution.** A clean C ABI both directions, and — following Zig — the ability to compile
and link C directly, so Keel drops into a C/C++ codebase file by file. It targets WebAssembly
and can be embedded in or called from other hosts. FFI is **safe by default at the boundary**:
a foreign call is both an `Io`-class effect and gated by a capability.

```keel
extern "C":
    def sqlite3_open(path: cstring, out db: *Db); int
```

*Interpreter: `extern` blocks parse to no-ops; real FFI is future work.*

### 3.14 Syntax, formatting, and language evolution

**Resolution.** Python-style significant indentation with keyword operators (`and`/`or`/`not`),
made robust by a **single canonical formatter** — exactly one valid layout, so style
arguments and whitespace fragility end. For longevity, **editions**: a module declares its
edition, old editions compile forever, and editions interoperate.

*Interpreter: the canonical formatter (`keel fmt`) is implemented and idempotent —
`fmt(fmt(x)) == fmt(x)`, verified by the conformance suite's property category — and
semantics-preserving. Editions are not yet modeled.*

### 3.15 Undefined behavior

**Resolution.** Safe Keel has **no undefined behavior**: every operation is well-defined, a
checked error, or a panic — never silent corruption. Overflow is checked, indexing is
bounds-checked, casts are checked or explicit. UB can exist only in `unsafe`, minimized and
localized.

*Interpreter: integer arithmetic uses overflow-checked builtins and traps rather than
wrapping; indexing is bounds-checked.*

### 3.16 Content-addressed code

**The idea.** Following Unison, a definition is identified by the **hash of its typed syntax
tree**, not by its name; code is stored as ASTs in a database rather than as text files. The
consequences are not incremental: **no builds** (perfect-incremental compilation against a
shared cache), **instant non-breaking renames**, **no dependency-version conflicts**,
**perfect test caching**, and trivial reproducibility. Keel adopts the content-addressed
identity and storage model while still emitting ordinary AOT-native binaries.

*Interpreter: not implemented — the most forward-looking part of the design.*

---

## 4. Memory model

Keel manages memory at compile time through ownership, borrowing, and deterministic
destruction. No tracing garbage collector, no mandatory reference counting; a finished
program carries no collector and pays no collection pause.

- **Ownership and moves** — every value has exactly one owner; when the owner's scope ends,
  the value is destroyed immediately (RAII). Assigning or passing an owned heap value *moves*
  it, after which the source is unusable. Small plain values are `Copy`.
- **Borrowing** — a shared borrow `&x` grants read-only access to many readers; a unique
  borrow `&mut x` grants read-write access and must be the only live reference while held.
  "No mutation through an aliased reference" is what eliminates use-after-free and data races
  at compile time. Unary `&` is *borrow*; binary `&` is *bitwise and* — disambiguated by
  position.
- **Regions and shared graphs** — `region r:` gives a whole graph one lifetime freed at once;
  `shared<T>` is a generational reference whose stale access fails deterministically rather
  than corrupting memory.
- **Deterministic destruction** — ordered, reverse of construction; files, sockets, and locks
  release exactly when their owner leaves scope.
- **The `unsafe` boundary and `no-runtime` mode** — allocators, MMIO, FFI, and chip registers
  live in `unsafe` blocks; `no-runtime` mode drops every assumption of OS, allocator, or
  runtime, for kernels, bootloaders, and microcontrollers. The boundary is always visible.

---

## 5. Security model

Keel makes a dangerous operation require a value of a type producible only by a safe path, so
the unsafe way does not type-check — closing whole entries on the CWE Top 25.

- **Capability-based authority** — no ambient access to the outside world: `main` receives
  root capabilities and hands narrowed ones onward, so a signature honestly states what code
  can touch. Authority extends to dependencies: an imported library gets only what you pass it.

  ```keel
  def main(sys: System):
      logs = sys.fs.subtree("/var/log")   # narrowed to one directory
      run(logs)                            # can reach nothing else
  ```

- **Typed, context-aware strings** — plain `string` is inert and cannot reach a query engine,
  shell, HTML sink, or URL builder. Those accept only their context type, built only through
  parameterized builders that escape interpolated data. Concatenating user input into a query
  is a *type error* — closing XSS, SQL injection, command injection, and format-string defects.

  ```keel
  q: Sql = sql"SELECT * FROM users WHERE id = #user_id"   # bound, not spliced
  db.run("SELECT ... " + user_id)   # COMPILE ERROR: string is not Sql
  ```

- **Trust typing, paths, and secrets** — external input arrives as `Untrusted<T>` and cannot
  reach a sink until a validator converts it to `T`. Filesystem access uses a `Path` rooted in
  a directory capability; escaping via `..` is rejected. A `Secret<T>` cannot be printed,
  logged, or serialized; the only exit is an explicit, greppable `reveal`.

- **Checked arithmetic, bounds, initialization** — overflow is an error/panic, never silent
  wraparound; indexing is bounds-checked; definite-assignment analysis forbids reading
  uninitialized bindings.

**What the language does not promise.** Keel closes *classes* of defects; it does not make
programs correct. It cannot detect a wrong authorization policy, a miscomputed price, or a
misused protocol. The claim is narrow and strong: memory, injection, null, overflow,
traversal, race, and unchecked-error categories are closed by construction; anything requiring
program *intent* remains the developer's responsibility.

### Vulnerability-class summary

| Weakness (CWE) | Closed by | In the interpreter |
| --- | --- | --- |
| OOB read/write (125/787) | Bounds checks + ownership; refinements prove away the check | bounds-checked indexing ✓ |
| Use-after-free / double-free (416) | Single owner, deterministic drop | n/a in tree-walker |
| Uninitialized use (457/908) | Definite-assignment analysis | immutable-by-default ✓ |
| Null dereference (476) | No null; `T?` | `T?`, `?.`, `??` ✓ |
| Integer overflow (190) | Checked arithmetic by default | overflow-trapping ✓ |
| Injection — SQL/XSS/command/code (89/79/78/94) | Context types + parameterized builders | `sql""`/`html""`/`shell""`/`url""` ✓ |
| Format string (134) | Type-checked interpolation only | context-aware interpolation ✓ |
| Path traversal (22) | Rooted, capability-scoped paths | `..` rejected as catchable `Fail` ✓ |
| Ambient authority / privilege (269) | Capability-based I/O, incl. dependencies | `System` narrowing ✓ |
| Secret exposure (312/532) | `Secret<T>` | redaction + explicit `reveal` ✓ |
| Unhandled error (754/252) | `Fail<E>` must be handled or propagated | `?` + boundary ✓ |
| Data races (362) | Unique-borrow rule + `Send`/`Share` | compile-time concern (deferred) |
| Supply-chain compromise | Capability-sandboxed deps + enforced semver | toolchain (future) |

---

## 6. A tour of the language (runnable)

Each program below runs on the interpreter today.

**Basics — checked arithmetic, ranges, higher-order functions, interpolation, broadcasting**
(`01_basics.keel`):

```keel
def factorial(n: int); int:
    mut acc = 1
    mut i = 1
    loop till i > n; ; i++:
        acc = acc * i
    return acc

doubled = range(1, 6).map((x): x * 2)     # [2, 4, 6, 8, 10]
println("broadcast:", [1, 2, 3] * 10)     # [10, 20, 30]
```

**Algebraic types, pattern matching, options** (`02_types_match.keel`):

```keel
derive (Eq, Debug)
enum Shape:
    Circle(radius: f64 where > 0)
    Rect(w: f64, h: f64)
    Point

def area(s: Shape); f64:
    check s:
        is Circle(r): return 3.14159 * r * r
        is Rect(w, h): return w * h
        is Point: return 0.0

name = missing?.name ?? "anonymous"        # optional chaining + coalescing
```

**Effects with condition/restart** (`03_effects.keel`):

```keel
port = execute:
    parse_port("99999")?
handle Fail as e:
    println("config error:", e, "-> using default")
    resume 8080                            # recover at the failure site
```

**Capabilities, trust typing, secrets, injection-as-type-error** (`04_security.keel`,
`06_flagship.keel`). The flagship is the spec's Appendix B, adapted to run end-to-end:

```keel
def handle_lookup(req: Request, db: Db); Response / {Io, Fail}:
    raw = req.query("id")                                 # Untrusted<string>
    id = parse_id(raw)?                                   # drops Untrusted, proves >= 0
    rows = db.run(sql"SELECT id, name FROM users WHERE id = #id")?
    check rows.first():
        is some(u): return Response.json(u)
        is none:    return Response.not_found("no such user")
```

**Tests and property tests with shrinking** (`05_tests.keel`):

```keel
test prop "reversing twice restores the list" (xs: array[int]):
    assert reverse(reverse(xs)) == xs
```

---

## 7. Language reference (essentials)

- **Variables and mutability** — immutable by default; `mut` permits reassignment. `decimal`
  is exact (never a float for money). `nums: array[int] = [1, 2, 3]`.
- **Operators** — logical are keywords (`and or not`), bitwise are symbols (`& | ^ ~ << >>`),
  so a condition can never be misread as a bit operation. Arithmetic `+ - * / % **`;
  borrow/deref `&`, `&mut`, `*`; option `?.`, `??`; effect propagation postfix `?`; range
  `->`; ternary `cond ? a : b`.
- **Strings** — UTF-8; interpolate with `#name` or `#{expr}`. Interpolating into a context
  type escapes for that context.
- **Control flow** — `if`/`elif`/`else`; `check subj: is Pattern: ...` for exhaustive match;
  `loop till cond; init; step:` for counting; `loop through items:` binds `it`.
- **Functions and effects** — return type follows `;`, the effect set follows `/` (e.g.
  `; Config / {Io, Fail<ConfigError>}`). No `async`/`await`.
- **Composite types** — `struct`, `enum` with payload-carrying variants, refinements on
  fields (`radius: f64 where > 0`), and `derive (...)`.
- **Classes and interfaces** — fields/methods private by default; `public` exposes them.
  Sharing is by interfaces and composition; single-parent inheritance exists but is
  discouraged.
- **Error handling** — `Fail<E>` propagates with `?`; `execute`/`handle` scopes recovery; a
  handler may `resume` instead of unwinding. Ignoring a failing call is a compile error.
- **Concurrency** — `parallel scope s: s.spawn(); ...` with structured concurrency; messages
  between tasks move ownership.
- **Comptime and reflection** — `comptime:` runs ordinary Keel at compile time;
  `reflect(Type)` inspects structure.

The reference essentials above are a summary; the full rationale — every
spectrum-and-resolution choice — is the rest of this document.

---

## 8. Emergent capabilities

The strongest argument for this design is what it gives *for free*: capabilities other
languages bolt on with frameworks fall out of effects + capabilities + purity +
content-addressing as consequences.

- **Deterministic record-replay debugging** — because every interaction with the outside
  world is an explicit effect through a handler, a "recording" handler logs every outcome and
  a "replay" handler feeds them back deterministically. Any production execution, including a
  heisenbug, can be replayed exactly and stepped in reverse — without a special build.
- **Production hot code reload** — the actor model isolates state behind messages and
  content-addressed code makes a new definition a new hash that can be swapped in atomically.
- **Automatic structured observability** — tracing becomes a handler, not a library sprinkled
  through your code.
- **Perfect incremental computation and test caching** — purity plus content-addressing means
  a result is a deterministic function of input hashes.
- **Mechanical security and effect auditing** — a function's full authority is its effect set
  plus its capability parameters, so "can this library reach the network?" is a type query.

None of these is a separate feature with its own complexity budget; each is a reading of the
same underlying machinery.

---

## 9. Compilation and toolchain

Keel is bootstrapped: **a reference interpreter in C brings the language up, then the compiler
is rewritten in Keel itself (self-hosting) and the C interpreter is retired except as a
reference oracle.** This repository is Stage 0 of exactly that plan.

The production toolchain is **tiered** from one source tree: interpreter and JIT for
Python-grade interactivity (REPL, hot reload, instant tests), AOT through LLVM to native code
as fast as C for release. Content-addressed storage makes compilation perfect-incremental.
Output is a single static binary; `no-runtime` mode targets bare metal; WebAssembly reaches
the browser and edge; cross-compilation is first-class.

One official toolchain ships the compiler, the single canonical formatter, linter, package
manager (reproducible, content-addressed, capability-sandboxed dependencies,
compiler-enforced semver), test/property/fuzz runner, doc generator, and language server. The
language evolves through **editions**: old editions compile forever and interoperate.

### The bootstrap, concretely

`compiler/keelc.keel` is **the Keel compiler, written in Keel** — a complete
`lex → parse → codegen` pipeline that emits C against a shared runtime whose semantics mirror
the interpreter. `make bootstrap` runs the chain and asserts the fixpoint: the C interpreter
runs `keelc` to compile `keelc` (→ `stage1.c` → native `keelc1`), `keelc1` compiles `keelc`
again (→ `stage2.c` → `keelc2`), and `keelc2` compiles it once more (→ `stage3.c`). The
compiler is self-hosting because **`stage2.c == stage3.c`** byte for byte; additionally
**`stage1.c == stage2.c`**, so the interpreter and the compiled compiler emit identical code.
`make equiv` then cross-checks that Keel-Core programs produce identical output whether
interpreted by the oracle or compiled to a native binary. See `PART1.md` for the full account.

```
$ make bootstrap
Stage 1: oracle compiles keelc -> stage1.c -> keelc1
Stage 2: keelc1 compiles keelc -> stage2.c -> keelc2
Stage 3: keelc2 compiles keelc -> stage3.c
  stage2.c == stage3.c   — SELF-HOSTING FIXPOINT REACHED
  stage1.c == stage2.c   — interpreter and compiled compiler agree
```

---

## 10. Targets

From one codebase: desktop (Windows/macOS/Linux), mobile (Android/iOS), bare-metal and
embedded via `no-runtime`, and WebAssembly. **Quantum computing** is addressed as a hosted,
type-checked circuit-description library compiled to a vendor-neutral IR — a hybrid model,
forward-looking rather than shipped.

---

## 11. Honest limits

The language's deliberate anti-goals, and this implementation's stand-ins, stated plainly.

**The language does not claim** to eliminate logic errors, wrong requirements, or
design-level security flaws — only mechanical defect classes. It does not run the whole
language on a QPU. It does not guarantee safety inside `unsafe`. The verification layer is
undecidable in general; the SMT solver can time out. Content-addressed code is an unfamiliar
mental model. Effects, regions, refinements, and content-addressing add concepts to learn —
the bet is they are *fewer, more orthogonal* concepts than the union they replace.

**This Stage-0 interpreter** is faithful where it matters most — effects, context-typed
strings, capabilities, trust/secrets, checked arithmetic, pattern matching, options, and
property testing — and uses deliberate stand-ins elsewhere: refinement types are checked at
runtime rather than proven by an SMT solver; effect resumption is one-shot and in-scope, not
multi-shot; the ownership/borrow system is a compile-time discipline not modeled at runtime;
and there is no AOT backend, JIT, package manager, FFI, or content-addressed store. See
`DESIGN.md` for the complete account.

---

## Appendix A — Reserved keywords

`and  assert  check  class  comptime  continue  def  derive  effect  elif  else  enum
execute  extern  handle  if  import  interface  is  loop  module  mut  none  not  or
parallel  private  prop  public  region  reflect  resume  return  scope  shared  spawn
struct  test  through  till  total  type  unsafe  where`

## Appendix B — Flagship example

A web request handler showing capabilities, effects (no `async`/`await`), context-typed
queries, trust typing, refinement types, derivation, structured recovery, and an inline test —
all in safe Keel with no garbage collector. It runs on the interpreter today.

```keel
derive (Eq, Json)
struct User:
    id: int where >= 0
    name: string

def handle_lookup(req: Request, db: Db); Response / {Io, Fail<AppError>}:
    raw: Untrusted<string> = req.query("id")
    id: int where >= 0 = parse_id(raw)?          # validation drops Untrusted, proves >= 0
    rows = db.run(sql"SELECT id, name FROM users WHERE id = #id")?   # injection can't type-check
    check rows.first():
        is some(u): return Response.json(u)
        is none:    return Response.not_found("no such user")

test "missing id yields a clean error":
    resp = with_fake(db: empty_db): handle_lookup(req_without_id(), db)
    assert resp.status == 400

def main(sys: System):
    db = sys.net.database("postgres://localhost/app")    # authority granted explicitly
    serve(sys.net.listen(8080), (req): handle_lookup(req, db))
    # Sync or async purely by which Io handler `serve` installs — same code either way.
```
