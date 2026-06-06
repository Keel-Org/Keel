# Keel — Language Design Specification

## Abstract

Keel is a general-purpose, statically typed, ahead-of-time compiled language built on a single wager: most of the pain in programming is not inherent but historical — the accreted cost of compromises (a garbage collector here, a null pointer there, a textual macro, an invisibly-unwinding exception) each of which was reasonable at the time and none of which we still have to accept. Keel takes the single best idea from every widely used language and combines them into one coherent system, while making the corresponding worst idea *impossible to express*.

The result is memory-safe without a garbage collector, free of undefined behavior in safe code, closes whole vulnerability classes at the type level, eliminates the function-coloring problem, tracks effects and errors in function types, proves optional correctness properties at compile time, stores code so that builds and dependency conflicts disappear, and compiles fast enough to use interactively while emitting native code as fast as C. It is meant to be the *only* language a team needs across the whole stack — kernel to script to browser — retiring the two-language problem rather than living inside it.

This is a design specification, not a tutorial. Where a decision settles a long-running trade-off in the field, the rationale is stated inline; Appendix C records the major alternatives considered and rejected.

---

## 1. Design Thesis

The 2026 landscape is instructive. Python leads on readability and an unmatched scientific ecosystem but is slow and dynamically unsafe. C and C++ own performance and control but bleed memory-safety CVEs. Rust is the most admired language precisely because it proved memory safety without a garbage collector is possible — yet its learning curve, borrow-checker friction, async story, and compile times are its most-cited complaints. TypeScript became the most-contributed language on GitHub by bolting static types onto JavaScript, but the types are erased and unsound. Go won on simplicity, fast builds, and single-binary deployment while frustrating everyone with `if err != nil` and a weak type system.

Each is a *partial* answer. Keel's goal is their union under one consistent rule set:

1. **Zero defect classes by construction** — memory-safety bugs, data races, null dereferences, integer overflow, injection, path traversal, uninitialized reads, and unhandled errors are impossible in safe code, enforced by the type system rather than by discipline or tooling.
2. **No garbage collector, no undefined behavior** — deterministic, predictable, real-time-capable, embeddable in a kernel.
3. **One language, whole stack** — the same source serves firmware, operating systems, backends, scientific computing, the browser (WebAssembly), and interactive scripting.
4. **Readable as Python, fast as C, safe beyond Rust** — compiling fast enough to use as a REPL.
5. **Evolvable without breaking** — decades of change with no Python-2-to-3 schism.

---

## 2. Design Lineage: What Keel Keeps and What It Eliminates

The audit below spans the top languages in use today (TIOBE / Stack Overflow / GitHub, 2026) plus assembly: for each, the one strength Keel doubles down on and the one weakness it deletes. The "how" is detailed in §3.

### 2.1 Systems languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| C | Minimal core, stable ABI, total control, universal portability | Undefined behavior, memory unsafety, no modules, weak types |
| C++ | Zero-cost abstractions, RAII, deep expressiveness | Accidental complexity, UB, template error walls, slow compiles |
| Rust | Memory safety without GC, traits, fearless concurrency, cargo-grade tooling | Borrow-checker friction, lifetime verbosity, `Rc<RefCell>` ceremony, async coloring, slow builds |
| Zig | `comptime`, colorblind async, superb C interop and cross-compilation, no hidden control flow/allocation | No compile-time memory-safety guarantee |
| D | Powerful compile-time metaprogramming, fast compilation | Optional-but-pervasive GC that split the community |
| Nim | Python-like syntax compiling to native | Small ecosystem, macro complexity |
| Ada / SPARK | Strong typing, real-time guarantees, a *formally provable* subset | Verbosity, dated tooling, niche isolation |
| Pascal / Delphi | Fast compilation, readability, rapid development | Proprietary baggage, ecosystem isolation |

### 2.2 Managed / enterprise languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Java | Write-once portability, vast ecosystem, backward compatibility, virtual threads | GC pauses, `null` (NPE), type erasure, boilerplate, slow startup |
| C# | LINQ-style expressiveness, value types, async pioneer | GC, async function coloring, runtime dependency |
| Kotlin | Null safety, conciseness, structured concurrency | JVM dependency, GC, compile speed |
| Scala | Expressive type system, FP + OOP fusion | Accidental complexity, implicits confusion, slow compile, JVM/GC |
| Go | Fast builds, single static binary, lightweight concurrency, batteries-included stdlib | GC latency, `if err != nil` ceremony, anemic types, `nil` |
| Swift | Value semantics, protocols, safety, deterministic ARC (no tracing GC) | Apple-centricity, ARC overhead and retain cycles, compile speed |
| Dart | AOT + JIT enabling hot reload, null safety | Framework lock-in, GC |

### 2.3 Dynamic / scripting languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Python | Readability, batteries-included, dominant AI/ML/science ecosystem, REPL | Runtime speed (GIL), dynamic-typing bugs, packaging hell, two-language problem |
| JavaScript | Universal browser reach, event-loop concurrency | Type-coercion insanity, no static guarantees, npm supply-chain disasters |
| TypeScript | Structural static typing, superb editor DX | Erased, unsound types; still on the JS runtime |
| Ruby | Developer happiness, expressiveness, metaprogramming | Slow execution, GC, dynamic-typing bugs |
| Lua | Tiny, embeddable, fast (LuaJIT) | Minimal stdlib, dynamic typing |
| PHP / Perl / Bash | Trivial web deploy; text/regex power; ubiquitous glue | Inconsistent stdlib; write-only unreadability; quoting/injection nightmares |

### 2.4 Functional languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Haskell | Typeclasses, higher-kinded types, purity, correctness | Lazy-evaluation space leaks, opaque performance, GC |
| OCaml | Fast inference, ML modules, *algebraic effects* (OCaml 5) | Small ecosystem, dated syntax/tooling, GC |
| Koka / Eff / Effekt | Effects + handlers as the *one* general control mechanism; the `total` effect (pure **and** terminating) | Research-only maturity, tiny ecosystems |
| Erlang | Actor model, supervision, hot code reload, million-process concurrency, nine-nines reliability | Dynamic typing, weak per-core throughput |
| Elixir | The BEAM concurrency model with humane syntax | Dynamic typing, runtime performance ceiling |
| Clojure | Persistent immutable data structures, REPL-driven dev | Dynamic typing, JVM/GC, opaque errors |
| Lisp / Scheme / Racket | Homoiconic macros, the *condition/restart* system, REPL | Untyped/unhygienic macros, runtime-only errors |
| Unison | **Content-addressed code**: no builds, instant non-breaking renames, no dependency conflicts, perfect test caching | Bespoke runtime, immature ecosystem |

### 2.5 Scientific, array, and data languages

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| Julia | Solves the two-language problem, multiple dispatch, native arrays/broadcasting | JIT warm-up latency, GC, large binaries |
| Fortran | Numerical performance, first-class arrays, no-alias guarantees | Dated ergonomics, isolation |
| R / MATLAB | Statistics & visualization; matrix-first ergonomics | Slow, quirky; proprietary and costly |
| APL / J / K / q | Whole-array (rank-polymorphic) operations, extreme concision | Cryptic glyph soup, unreadability |
| COBOL | Exact decimal arithmetic, longevity | Verbosity, archaic ergonomics |

### 2.6 Declarative, contract, and low-level

| Language | Strength Keel doubles down on | Weakness Keel eliminates |
| --- | --- | --- |
| SQL | Declarative, optimized set operations | Dialect fragmentation, string-built injection |
| Solidity | Deterministic execution, smart-contract model | Reentrancy/overflow footguns |
| **Assembly** | Ultimate control, peak performance, direct ISA access, zero overhead | Non-portability, no abstraction, no safety, unreadability |

Assembly's strengths are captured without making the language unsafe: inline assembly and raw hardware access live in `unsafe` and `no-runtime` mode (§4.7, §9), comptime gives zero-overhead abstraction, and everything outside those blocks stays portable and safe.

---

## 3. The Fundamental Concepts

Each subsection states the spectrum the field occupies, the failure modes at its ends, and Keel's resolution — improving the concept, fixing it, or adopting it unchanged when already right.

### 3.1 Memory management

**Spectrum.** Manual (`malloc`/`free`) → tracing GC (pauses, no determinism) → reference counting/ARC (cycle leaks, refcount traffic) → ownership/borrowing (safe, no GC, but ergonomic friction and hard for cyclic data).

**Resolution.** Compile-time ownership and borrowing with deterministic destruction is the base — the only known way to get safety *and* no GC *and* predictable latency — with Rust's three biggest ergonomic costs removed: lifetimes are **inferred** except at published boundaries; **regions/arenas** give whole graphs one shared lifetime freed at once (the case where people reach for `unsafe` or `Rc<RefCell>`); and **generational references** (`shared<T>`) make shared mutable graphs safe by catching a stale access deterministically rather than corrupting memory — no collector, no borrow-panic ceremony. Ownership when natural, regions when ownership is awkward, generational sharing when graphs demand it; all safe, all GC-free. (Detail in §4.) *Why not GC — Appendix C.*

### 3.2 Type system and inference

**Spectrum.** Dynamic (fails at runtime) → unsound static (Java/TS erasure: types lie at runtime) → sound static (Rust/OCaml/Swift) → dependent/refinement (provable, but heavy).

**Resolution.** Sound, static, *inferred* typing with dynamic-language ergonomics. Local inference (not slow whole-program Hindley-Milner) means most code carries no annotations while every type is checked and never erased. Generics are real, monomorphized, and zero-cost, with trait constraints checked *before* instantiation so an error names the unmet constraint instead of dumping a template trace. Structural typing (TypeScript's best feature) sits beside nominal typing, chosen per declaration. An optional refinement layer (§3.8) adds compile-time proofs for code that wants them.

### 3.3 Algebraic effects — the unifying mechanism

**The problem.** Exceptions, async/await, generators, dependency injection, and dynamic scoping are the same thing in different costumes: a computation that suspends and yields control to an outer handler. Treating them separately is why async colors functions, why exceptions are invisible control flow, and why faking I/O for tests needs a mocking framework. Koka's "minimal but general" principle showed one mechanism subsumes them all.

**Resolution.** One **algebraic effects and handlers** system underlies everything. A function's type records the effects it may perform; the caller's installed handler decides what those effects *do*.

```keel
def read_config(dir: Dir); Config / {Io, Fail<ConfigError>}:
    text = dir.open("config.toml")?      # `?` performs Fail; `open` performs Io
    return parse(text)?
```

Effects are **inferred** within a module and written only at boundaries (the lifetime discipline again), so everyday code stays clean. From this one mechanism Keel derives error propagation (§3.5), colorblind async (§3.4), iterators, scoped configuration, and framework-free testable I/O — a test simply installs a handler that fakes the `Io` effect. A function whose effect set is empty is **`total`**: pure *and* proven to terminate (Koka's insight), safe to memoize, reorder, and parallelize.

### 3.4 Concurrency and async

**Spectrum.** Threads + locks (races, deadlocks) → async/await (function coloring: async is viral, a sync caller can't call it) → green threads (no coloring, but leaks and shared-memory races) → actors/CSP (safe messages, less control) → colorblind async (Zig's `Io`-as-parameter).

**Resolution.** Data races are a compile error by construction — the unique-borrow rule (§4) forbids two tasks from holding mutable access to one value, with no runtime lock discipline. On top of that: **no function coloring** (asynchrony is the `Io` effect; the same body runs sync under a blocking handler or async under an event-loop handler — the caller decides, so the red/blue split is gone); **structured concurrency by default** (tasks spawn into a `scope` that cannot exit until its children finish, so leaks are structurally impossible); and the **Erlang actor/supervision model** as a library on the safe core — lightweight processes, message passing, "let it crash" — but with statically typed messages the BEAM never had.

### 3.5 Error handling

**Spectrum.** Error codes (silently ignorable) → exceptions (invisible, expensive, often unchecked) → `Result`/`Option` values (explicit; Go's verbose, Rust's `?` good) → conditions/restarts (Lisp: recover *at the error site* without unwinding — the most powerful model, almost universally forgotten).

**Resolution.** Errors are values via the `Fail<E>` effect; a fallible function says so in its type and a failure cannot be silently dropped. `?` propagates with no ceremony (no `if err != nil`). Bugs `panic` and unwind. And Keel revives **condition/restart** through effect handlers: a handler can inspect a failure and `resume` the computation at the point it failed with a supplied value — retry, substitute, skip — recovery exceptions cannot express.

### 3.6 Null and absence

**Spectrum.** Null everywhere (the billion-dollar mistake) → nullable-by-annotation (bolted on) → no null, only options.

**Resolution.** No null. Absence is `T?`, unusable as `T` until checked; `?.` accesses through it and `??` supplies a default. Null dereference ceases to exist as a category.

### 3.7 Metaprogramming, generics, reflection, and derivation

**Spectrum.** Textual macros (C preprocessor: unhygienic, type-blind) → templates (C++: Turing-complete but unreadable errors) → hygienic macros (Scheme/Rust) → compile-time execution (Zig/D `comptime`) → *runtime* reflection (Java/C#/Python: flexible but costly — it defeats monomorphization, bloats binaries, and punches holes in safety).

**Resolution.** Everything happens at **compile time**, at zero runtime cost:

- **`comptime`** runs ordinary Keel — the same language, not a macro dialect — during compilation, used for constant folding, specialization, and implementing generics themselves (so there is no separate template language).
- **Typed hygienic macros** operate on the *typed* AST: hygienic (no capture), type-checked (errors point at your code), unable to build ill-typed programs.
- **Compile-time reflection** lets code inspect types — fields, variants, attributes — during compilation. Combined with comptime this generates serializers, ORMs, and RPC stubs with no runtime reflection tax.
- **Derivation** is the everyday face of this: `derive (Eq, Hash, Json, Debug)` synthesizes those implementations from a type's structure, replacing boilerplate. Optional, opt-in *runtime* type information exists only where explicitly requested, so it is never a tax you didn't ask for.

```keel
derive (Eq, Hash, Json)
struct Point:
    x: int
    y: int
```

### 3.8 The assurance continuum — testing, refinement, and proof

**The problem.** Other languages treat testing, static typing, and formal verification as separate worlds with hard walls between them. Keel treats them as **one continuous dial** of confidence, so you pay exactly as much rigor as a given piece of code warrants — and unit testing takes its natural place on that dial rather than being the only tool.

The continuum, from least to most assurance:

1. **Types** — the baseline; sound and non-erased, already ruling out most defect classes (§3.2).
2. **Unit tests** — first-class `test` blocks live beside the code, run by the integrated runner. Because the effect system can certify a test performs no uncontrolled I/O, deterministic tests are **cached perfectly** and rerun only when a dependency's hash changes (§3.16) — Unison's result, reached through effects + content-addressing.
3. **Property-based tests** — `test prop` states a law over generated inputs; the runner searches for and shrinks counterexamples (QuickCheck's model, built in).
4. **Fuzzing** — the same harness drives coverage-guided fuzzing of any function from one annotation.
5. **Refinement types** — predicates on types (`int where != 0`, `array[T] where len > 0`) discharged by a built-in SMT solver, *proving* at compile time and zero runtime cost that division-by-zero, out-of-bounds, overflow, and broken invariants cannot occur.
6. **Totality and full proof** — the `total` effect proves termination; the provable subset (SPARK/Dafny-grade) verifies a function cannot panic and meets a functional specification — for avionics, crypto, kernels, and smart contracts (where a proven-terminating contract cannot exhaust gas).

```keel
test "deposit increases balance":
    mut acct = BankAccount()
    acct.deposit(100)
    assert acct.balance == 100

test prop "reverse is its own inverse" (xs: array[int]):
    assert reverse(reverse(xs)) == xs

def divide(a: int, b: int where != 0); int:
    return a / b        # the compiler proved b != 0; no check, no panic
```

Code starts at the level it needs and is promoted as it matters; nothing forces avionics-grade proof onto a prototype, and nothing stops a prototype from being hardened later.

### 3.9 Mutability, aliasing, and value semantics

**Spectrum.** Mutable-everything with reference aliasing (spooky action at a distance) → immutable-everything (safe, but persistent-structure overhead on every update) → value semantics with controlled mutation.

**Resolution.** Immutable bindings by default; values have value semantics (assignment copies meaning, not a shared reference), killing aliasing bugs — but because ownership tracks unique access, Keel mutates **in place** when it holds the only reference. You get Clojure's safety with C's update speed, instead of paying persistent-structure cost unconditionally. Persistent immutable collections remain in the stdlib for code that wants structural sharing.

### 3.10 Numeric and array computing — retiring the two-language problem

**The problem.** "Prototype in Python, rewrite the hot path in C/CUDA" is a tax that exists only because no language was both high-level and fast. Julia proved one can be both.

**Resolution.** First-class N-dimensional arrays with **broadcasting** and slicing (NumPy/Julia/Fortran/APL ergonomics) compile to vectorized native code; **portable SIMD** is a first-class type, not an intrinsics swamp; whole-array rank-polymorphic operations read as ordinary arithmetic (APL's insight, minus the glyphs); an **exact decimal** type (COBOL's enduring contribution) makes money math correct by default. Because pure functions are provably pure (§3.3), the optimizer parallelizes and reorders kernels safely. The same array code targets CPU, SIMD lanes, and — via backends — GPUs, so the prototype *is* the production code.

### 3.11 Compilation model and speed

**Spectrum.** Interpreted (instant iteration, slow runtime) → bytecode VM + JIT (fast steady-state, warm-up + runtime) → AOT native (Go: seconds, modest opt; Rust/C++: hard opt, slow builds).

**Resolution.** A **tiered** toolchain from one source tree: a built-in interpreter and JIT give Python-grade interactivity — REPL, hot reload, instant tests — while release builds AOT-compile through LLVM to native as fast as C. Compile speed is a hard constraint (local inference, separate compilation, incremental caching, parallel codegen), and content-addressed storage (§3.16) takes incremental compilation to its limit. You prototype interactively and ship a fully optimized native binary from identical code.

### 3.12 Tooling, packaging, and supply-chain security

**The problem.** Dependency management is where ecosystems rot — `left-pad`, PyPI malware, the `xz` backdoor — while Cargo and Go modules showed how good it can be.

**Resolution.** One official toolchain (compiler, formatter, linter, package manager, test runner, doc generator, language server) so the ecosystem never fragments. Beyond reproducible, content-addressed builds, two harder moves: **capability-sandboxed dependencies** — a library gets *no ambient authority* and can only do what the importer grants by passing a capability (§5.1), so a logging crate that suddenly opens a socket simply cannot — and **compiler-enforced semantic versioning** — the compiler diffs a package's public API across versions and *computes* the correct semver bump, rejecting a "patch" that broke the API (Elm's idea). Version numbers stop lying.

### 3.13 Interop, FFI, and adoption

**The problem.** New languages die because rewriting existing code is infeasible; the ones that spread (TypeScript, Kotlin, Zig) offered seamless interop with the incumbent.

**Resolution.** A clean C ABI both directions, and — following Zig — the ability to compile and link C directly, so Keel drops into a C/C++ codebase file by file rather than demanding a rewrite. It targets WebAssembly for the browser and edge, and can be embedded in or called from Python and other hosts. Crucially, FFI is **safe by default at the boundary**: a foreign call is both an `Io`-class effect and gated by a capability, so even C interop cannot smuggle in ambient authority, and the unsafe raw call is wrapped in a checked, capability-bearing Keel signature. Cross-compilation is first-class and trivial.

```keel
extern "C":
    def sqlite3_open(path: cstring, out db: *Db); int

# The raw call is unsafe; the public wrapper is safe, effectful, and capability-gated.
def open_db(fs: Dir, name: string); Db / {Io, Fail<DbError>}:
    ...
```

### 3.14 Syntax, formatting, and language evolution

**Spectrum.** Brace wars and style bikeshedding → significant indentation (clean but copy-paste-fragile) → one canonical formatter (`gofmt`/`zig fmt`: ends the debate). Evolution: Python's 2→3 break fractured the community for a decade; C++ carries every decision forever; Rust's *editions* evolve without breaking old code.

**Resolution.** Python-style significant indentation with keyword operators (`and`/`or`/`not`), made robust by a **single canonical formatter** in the toolchain — exactly one valid layout, so style arguments and whitespace fragility end. For longevity, **editions**: a module declares its edition, old editions compile forever, and editions interoperate — decades of change with no schism and no ballast.

### 3.15 Undefined behavior

**The problem.** In C/C++, undefined behavior is not a crash but a license for the compiler to do anything, and the root of a large fraction of exploitable bugs.

**Resolution.** Safe Keel has **no undefined behavior**: every operation is well-defined, a checked error, or a panic — never silent corruption. Overflow is checked, indexing is bounds-checked, casts are checked or explicit. UB can exist only in `unsafe`, minimized and localized so audit and tooling concentrate on the small marked surface.

### 3.16 Content-addressed code

**The idea.** Following Unison (1.0, late 2025), a definition is identified by the **hash of its typed syntax tree**, not by its name; names are human-friendly metadata pointing at hashes, and code is stored as ASTs in a database rather than as text files parsed on every build.

**Why it is worth the radicalism.** The consequences are not incremental:

- **No builds.** Compilation is perfect-incremental against a shared cache keyed by hash; a definition whose hash is unchanged is never recompiled. You are almost never waiting on the compiler.
- **Instant, non-breaking renames.** A name is metadata, so renaming touches no hashes and breaks nothing — refactoring stops being a global edit.
- **No dependency-version conflicts.** Two dependencies needing different versions of a third just reference different hashes; both coexist without diamond conflicts.
- **Perfect test caching.** A deterministic test (certified by the effect system, §3.8) is rerun only if a dependency's hash changed.
- **Trivial reproducibility and code sharing** — a hash *is* an exact, content-verified identity.

Keel adopts the content-addressed *identity and storage* model and its workflow, while still emitting ordinary AOT-native binaries (§3.11) — the hash identity is a source-and-cache concern, not a runtime one. The honest cost is in Appendix C and §11: the code-as-database model is unfamiliar and demands tooling support; Keel keeps a plain-text projection so the source is still readable and diffable.

---

## 4. Memory Model

Keel manages memory at compile time through ownership, borrowing, and deterministic destruction. No tracing garbage collector, no mandatory reference counting; a finished program carries no collector and pays no collection pause.

### 4.1 Ownership and moves

Every value has exactly one owner; when the owner's scope ends, the value is destroyed immediately (RAII). Assigning or passing an owned heap value *moves* it, after which the source is unusable. Small plain values are `Copy` and duplicated instead.

```keel
name = "Omar"
greeting = name      # ownership moves
print(name)          # COMPILE ERROR: `name` was moved
```

### 4.2 Borrowing

A **shared borrow** `&x` grants read-only access to many readers at once; a **unique borrow** `&mut x` grants read-write access and must be the only live reference while held. "No mutation through an aliased reference" is what eliminates use-after-free and data races at compile time. Unary `&` is *borrow*; binary `&` is *bitwise and* — disambiguated by position.

### 4.3 Lifetimes, regions, and shared graphs

Lifetimes are inferred except at boundaries. For data outside single ownership, two safe tools instead of `unsafe`:

```keel
region r:
    root = r.alloc(Node({value: 1}))
    root.next = r.alloc(Node({value: 2}))
# the whole region — graph and all — is freed here at once

cache: shared<Table> = shared(Table())   # generational reference; a stale access
                                          # fails deterministically, never corrupts
```

### 4.4 No null

Absence is `T?`, unusable as `T` until checked; `?.` and `??` operate on it.

### 4.5 Deterministic destruction

Destruction is deterministic and ordered (reverse of construction). Files, sockets, and locks release exactly when their owner leaves scope — no finalizer queue, no nondeterministic timing.

### 4.6 Concurrency safety

A unique borrow is exclusive, so two tasks cannot hold mutable access to one value; data races are rejected at compile time. A value crosses a task boundary only if `Send` and is shared across tasks only if `Share`; both are derived and checked automatically.

### 4.7 The `unsafe` boundary and `no-runtime` mode

Allocators, MMIO, FFI, BIOS/UEFI, and chip registers live in `unsafe` blocks, where raw pointers (`*T`), pointer arithmetic, and inline assembly become available and the programmer assumes the checker's obligations. `no-runtime` (freestanding) mode drops every assumption of OS, allocator, or runtime — the mode for kernels, bootloaders, and microcontrollers. The boundary is always visible in source. Explicit layout control (`packed`, alignment, field order) is available for ABI and hardware work.

---

## 5. Security Model

Keel makes a dangerous operation require a value of a type producible only by a safe path, so the unsafe way does not type-check — closing whole entries on the 2025 CWE Top 25.

### 5.1 Capability-based authority

No ambient access to the outside world: `main` receives root capabilities and hands narrowed ones onward, so a signature honestly states what code can touch. Authority flows through values, making least privilege structural — and it extends to dependencies (§3.12): an imported library gets only what you pass it.

```keel
def main(sys: System):
    logs = sys.fs.subtree("/var/log")   # narrowed to one directory
    run(logs)                            # can reach nothing else
```

### 5.2 Typed, context-aware strings

Plain `string` is inert and cannot reach a query engine, shell, HTML sink, or URL builder. Those accept only their context type (`Sql`, `Shell`, `Html`, `Url`), built only through parameterized builders that escape interpolated data. Concatenating user input into a query is a *type error* — closing XSS, SQL injection, OS command injection, code injection, and (no `printf` format strings) format-string defects.

```keel
q: Sql = sql"SELECT * FROM users WHERE id = #user_id"   # bound, not spliced
db.run("SELECT ... " + user_id)   # COMPILE ERROR: string is not Sql
```

### 5.3 Trust typing, paths, and secrets

External input arrives as `Untrusted<T>` and cannot reach a sink until a validator converts it to `T`. Filesystem access uses a `Path` rooted in a directory capability; escaping the root via `..` is rejected (closes traversal). A `Secret<T>` cannot be printed, logged, or serialized and compares in constant time (closes secret exposure); the only exit is an explicit, greppable `reveal`.

### 5.4 Checked arithmetic, bounds, initialization

Arithmetic is checked by default — overflow is an error/panic, never silent wraparound; `wrapping_`/`saturating_`/`checked_` variants are explicit. Indexing is bounds-checked, and refinements (§3.8) can prove the check away. Definite-assignment analysis forbids reading uninitialized bindings.

### 5.5 What the language does not promise

Keel closes *classes* of defects; it does not make programs correct. It cannot detect a wrong authorization policy, a miscomputed price, or a misused protocol. CSRF and broken access control are reduced by capability typing but still depend on application logic. The claim is narrow and strong: memory, injection, null, overflow, traversal, race, and unchecked-error categories are closed by construction; anything requiring program *intent* remains the developer's responsibility.

### Vulnerability-class summary

| Weakness (CWE) | Closed by |
| --- | --- |
| OOB read/write (125/787) | Bounds checks + ownership; refinements prove away the check |
| Use-after-free / double-free (416) | Single owner, deterministic drop |
| Uninitialized use (457/908) | Definite-assignment analysis |
| Null dereference (476) | No null; `T?` |
| Integer overflow (190) | Checked arithmetic by default |
| Injection — SQL/XSS/command/code (89/79/78/94) | Context types + parameterized builders |
| Format string (134) | Type-checked interpolation only |
| Path traversal (22) | Rooted, capability-scoped paths |
| Ambient authority / privilege (269) | Capability-based I/O, incl. dependencies |
| Secret exposure (312/532) | `Secret<T>` |
| Unhandled error (754/252) | `Fail<E>` must be handled or propagated |
| Data races (362) | Unique-borrow rule + `Send`/`Share` |
| Supply-chain compromise | Capability-sandboxed deps + enforced semver |

---

## 6. Type System and Effects

Keel is statically, strongly, and soundly typed with non-erased types and local inference. Generics are monomorphized and zero-cost, constrained by traits checked before instantiation. Structural and nominal typing are both available per declaration. The refinement layer adds SMT-checked predicates (§3.8). Effects are tracked in function types — `Fail<E>`, `Io`, user-defined effects — inferred locally and declared at boundaries; an empty effect set means `total` (pure and terminating).

---

## 7. Language Reference

### 7.1 Variables, mutability, data types

Immutable by default; `mut` permits reassignment/mutation. Compound assignment and `++`/`--` (statements, not expressions) require `mut`.

```keel
age = 25                                  # inferred int, immutable
mut counter = 0
ledger: decimal = 19.99                   # exact decimal; never a float for money
nums: array[int] = [1, 2, 3, 4, 5]
```

### 7.2 Operators

Logical operators are keywords, bitwise are symbols, so a condition can never be misread as a bit operation. Comparison `== != < <= > >=`; logical `and or not`; bitwise `& | ^ ~ << >>`; arithmetic `+ - * / % **`; borrow/deref unary `&`, `&mut`, `*`; option `?.`, `??`; effect propagation postfix `?`; range `->`; ternary `cond ? a : b`.

### 7.3 Strings, interpolation, arrays

Strings are UTF-8; interpolate with `#name`. Interpolating into a context type escapes for that context. N-dimensional arrays support slicing and broadcasting:

```keel
a = [[1, 2], [3, 4]]
b = a * 2                 # broadcast → [[2, 4], [6, 8]]
row = a[0, ..]            # slice
```

### 7.4 Control flow

```keel
if status == "open": handle_open()
elif status == "pending": hold()
else: reject()

check shape:                 # exhaustive pattern match
    is Circle(r): area = 3.14159 * r ** 2
    is Rect(w, h): area = w * h

loop till n == 20; n = 0; n++:   # counting: condition; init; step
    process(n)

loop through items:               # iterate; `it` is the element
    if it == sentinel: break
    if it % 2 == 0: continue
    use(it)
```

### 7.5 Functions and effects

Return type follows `;`; the effect set (when annotated) follows `/`. There is no `async`/`await`: a function performing `Io` runs sync or async per the caller's handler (§3.4).

```keel
def greet(when: DateTime, msg = "Hello"); string:           # total (pure)
    return "#msg at #when"

def read_config(dir: Dir); Config / {Io, Fail<ConfigError>}:
    text = dir.open("config.toml")?
    return parse(text)?
```

### 7.6 Composite types, derivation, refinement

```keel
struct Point:
    public x: int
    public y: int

derive (Eq, Hash, Json)
enum Shape:
    Circle(radius: f64 where > 0)        # refinement: radius is always positive
    Rect(w: f64, h: f64)
```

### 7.7 Classes, interfaces, encapsulation

Fields/methods are private by default (secure default favoring encapsulation); `public` exposes them. Sharing is by interfaces and composition; single-parent inheritance exists but is discouraged for new code.

```keel
interface Greeter:
    greet(self); string

class BankAccount:
    private balance: decimal = 0
    public def deposit(self: &mut, amount: decimal where > 0):
        self.balance += amount        # `amount > 0` proven at compile time
```

### 7.8 Error handling and recovery

`Fail<E>` propagates with `?`; an `execute`/`handle` block scopes recovery; a handler may `resume` the failed computation (Lisp restart) instead of unwinding. Ignoring a failing call is a compile error.

```keel
execute:
    f = dir.open("data.txt")?
    parse(f.read()?)
handle Fail<IoError> as e:
    report(e)
    resume default_config        # recover at the failure site, no unwind
```

### 7.9 Tests

`test` blocks live beside code and run under the integrated runner; deterministic ones are cached (§3.8, §3.16).

```keel
test "deposit increases balance":
    mut acct = BankAccount()
    acct.deposit(100)
    assert acct.balance == 100

test prop "reverse inverts itself" (xs: array[int]):
    assert reverse(reverse(xs)) == xs
```

### 7.10 Modules, imports, capabilities

Imports bring in names and types, never authority. A large stdlib is auto-imported as a prelude; modules conferring outside authority expose capability *types* supplied a value at runtime.

### 7.11 Concurrency

```keel
parallel scope s:
    s.spawn(); render()
    s.spawn(); index()
# both tasks complete here (structured concurrency)
```

Messages between tasks move ownership, so no lock is needed; the Erlang-style actor/supervision model is a library on this core with statically typed messages.

### 7.12 Comptime, macros, reflection

```keel
comptime:
    table = build_lookup_table(256)       # ordinary Keel at compile time
    for field in reflect(Point).fields:   # compile-time reflection
        register_serializer(field)
```

---

## 8. Emergent Capabilities

The strongest argument for this design is what it gives *for free*: capabilities other languages bolt on with frameworks fall out of effects + capabilities + purity + content-addressing as consequences.

**Deterministic record-replay debugging.** Because every interaction with the outside world is an explicit effect routed through a handler (§3.3) and all authority is an explicit capability (§5.1), a "recording" handler can log every effect outcome and a "replay" handler can feed those outcomes back deterministically. Any production execution — including a heisenbug — can be replayed exactly, stepped, and reversed (time-travel debugging), without a special build. Pure code needs nothing recorded at all.

**Production hot code reload.** Erlang's defining superpower (upgrade a running system with zero downtime) is reachable here because the actor model isolates state behind messages *and* content-addressed code makes a new definition a new hash that can be swapped in atomically. High-availability systems update live.

**Automatic structured observability.** Because effects are explicit and typed, the runtime can trace every effect — every I/O, every failure, every span — as structured, correlated telemetry with no manual instrumentation. Tracing becomes a handler, not a library sprinkled through your code.

**Perfect incremental computation and test caching.** Purity (§3.3) plus content-addressing (§3.16) means a result is a deterministic function of input hashes; the compiler and runtime memoize and recompute only what changed. This is why builds and deterministic tests effectively disappear.

**Mechanical security and effect auditing.** A function's full authority is its effect set plus its capability parameters. Reading a signature — or a whole dependency's — tells you, statically and exhaustively, everything it can do. Security review of "can this library reach the network?" becomes a type query, not a code-reading exercise.

None of these is a separate feature with its own complexity budget; each is a reading of the same underlying machinery.

---

## 9. Compilation and Toolchain

Keel is bootstrapped: a reference interpreter in C brings the language up, then the compiler is rewritten in Keel itself (self-hosting) and the C interpreter is retired except as a reference oracle.

The production toolchain is **tiered** from one source tree: interpreter and JIT for Python-grade interactivity (REPL, hot reload, instant tests), AOT through LLVM to native code as fast as C for release. Content-addressed storage (§3.16) makes compilation perfect-incremental — effectively *buildless* in the steady state. Output is a single static binary; `no-runtime` mode targets bare metal; WebAssembly reaches the browser and edge; a readable-C transpilation target exists for ultra-portability and certification audits; cross-compilation is first-class.

One official toolchain ships the compiler, the single canonical **formatter**, linter, package manager (reproducible, content-addressed, capability-sandboxed dependencies, compiler-enforced semver), **test/property/fuzz runner**, doc generator, language server, and version-control integration that operates on definitions (content-addressed diffs, conflict-free renames) rather than only on text. The language evolves through **editions**: old editions compile forever and interoperate with new ones.

The **standard library** is broad and modular, auto-imported as a prelude so common work (collections, math, text, options/results, time, encoding, the persistent immutable collections, exact decimal, N-d arrays/SIMD) needs no imports — while every module that confers outside authority exposes a capability type rather than ambient access.

---

## 10. Targets

From one codebase: desktop (Windows/macOS/Linux), mobile (Android/iOS), bare-metal and embedded via `no-runtime` (firmware, BIOS/UEFI, chip-level I/O through `unsafe`), and WebAssembly.

**Quantum computing.** Quantum machines are coprocessors, not general CPUs, so Keel does not "run on" them. It provides a hosted, type-checked **circuit-description library**: qubits, gates, and measurement are Keel values compiled to a vendor-neutral IR (e.g. QIR) and dispatched to a simulator or hardware backend, with classical control flow staying in ordinary Keel. This hybrid model is the only technically coherent reading of the goal, and is forward-looking rather than shipped.

---

## 11. Honest Limits (Anti-Goals)

To stay credible, what Keel deliberately does **not** claim:

- It does not eliminate logic errors, wrong requirements, or design-level security flaws — only mechanical defect classes (§5.5).
- It does not run the whole language on a QPU (§10).
- It does not guarantee safety inside `unsafe` (§4.7); those obligations move to the programmer.
- The verification layer (§3.8) is undecidable in general; the SMT solver can time out, and some true properties need manual proof hints. Verification is opt-in precisely because it cannot be free or total.
- Content-addressed code (§3.16) is an unfamiliar mental model and demands strong tooling; Keel mitigates this with a readable text projection but does not pretend the adjustment is zero.
- Effects, regions, refinements, and content-addressing add concepts to learn. The bet is that they are *fewer, more orthogonal* concepts than the union of features they replace across C++, Rust, and the rest — but the learning curve is real, and the design accepts that trade for the guarantees.

---

## Appendix A — Reserved keywords

`and  assert  check  class  comptime  continue  def  derive  effect  elif  else  enum  execute  extern  handle  if  import  interface  is  loop  module  mut  none  not  or  parallel  private  prop  public  region  reflect  resume  return  scope  shared  spawn  struct  test  through  till  total  type  unsafe  where`

## Appendix B — Flagship example

A web request handler showing capabilities, effects (no `async`/`await`), context-typed queries, trust typing, refinement types, derivation, structured recovery, and an inline test — all in safe Keel with no garbage collector.

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

## Appendix C — Considered and rejected

Recording the major alternatives and why they were turned down keeps the design honest.

- **Garbage collection.** Rejected. The stated motivation ("automatic, memory-safe") is fully met by ownership + regions + generational references — which are *also* deterministic, pause-free, and runtime-free, and so usable in kernels, real-time systems, and embedded targets where a tracing collector cannot go. Adding GC would forfeit the single property that lets Keel be one language across the whole stack, in exchange for ergonomics that ownership inference already recovers. Memory management in Keel *is* automatic; it simply is not a collector.
- **Exceptions as control flow.** Rejected in favor of the `Fail<E>` effect plus condition/restart (§3.5). Exceptions hide control flow, are often unchecked, and cost on the happy path; effects make failure visible in the type and recoverable at the source.
- **Null.** Rejected for `T?` (§3.6). The billion-dollar mistake has a known fix.
- **Default runtime reflection.** Rejected for compile-time reflection + derivation (§3.7). Runtime reflection defeats monomorphization, bloats binaries, and undermines the capability model; it remains available only where explicitly requested.
- **Inheritance-first OOP.** Rejected for composition + interfaces (§7.7). Single-parent inheritance is retained but discouraged; deep hierarchies and the fragile-base-class and diamond problems are not worth defaulting to.
- **Lazy evaluation by default.** Rejected (Haskell's space leaks and opaque performance). Keel is strict; laziness is available explicitly where wanted.
- **Dynamic typing.** Rejected. Inference makes static typing feel dynamic to write while keeping every guarantee, so the trade dynamic languages make is unnecessary.
- **A separate macro language.** Rejected for `comptime` in the same language (§3.7), eliminating the C-preprocessor and C++-template failure modes.
