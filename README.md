# Keel

Keel is a general-purpose, statically typed, ahead-of-time compiled language built on a
single wager: most of the pain in programming is historical, not inherent. It aims to take
the best idea from every widely used language while making the corresponding worst idea
*impossible to express* — memory-safe without a garbage collector, free of undefined behavior
in safe code, whole vulnerability classes closed at the type level, effects and errors tracked
in function types, and one canonical toolchain. The full design is in **[SPEC.md](SPEC.md)**.

This repository is a working implementation that is now **self-hosting**: a compiler written
in Keel compiles itself to a byte-identical fixpoint and produces output equivalent to the C
reference interpreter. The short account is in **[PART1.md](PART1.md)**.

## Quick start

```
make            # build bin/keel (single C11 file, no deps beyond libc/libm)
make conform    # the conformance suite (the executable specification)
make bootstrap  # build the self-hosting chain and assert the fixpoint
make equiv      # check the self-hosted compiler agrees with the interpreter
make all-checks # conform + bootstrap + equiv
make test       # the integrated test runner (unit + property, with shrinking)

./bin/keel run    tests/compiled/recursion.keel
./bin/keel fmt    tests/compiled/strings.keel
./bin/keel tokens tests/compiled/patterns.keel
./bin/keel ast    tests/compiled/patterns.keel
./bin/keel test   tests/property/05_tests.keel

# compile a Keel program to a native binary with the Keel-written compiler
./keelc-build.sh tests/compiled/recursion.keel /tmp/rec && /tmp/rec
```

The CLI mirrors the toolchain vision in miniature: `run` (interpret), `test` (the integrated
runner), `fmt` (the one canonical formatter), and `tokens`/`ast` (differential-testing dumps).

---

## Implementation status

| Area | Status |
| --- | --- |
| Significant-indentation lexer, Pratt parser | implemented |
| Structs, enums, pattern matching, options (`T?`, `??`, `?.`) | implemented |
| Algebraic effects + handlers, `resume` / abort | implemented (one-shot, in-scope) |
| Capability-based authority, narrowing, traversal rejection | implemented (runtime-enforced) |
| Context-typed strings (`sql""`, `html""`, `shell""`, `url""`) | implemented (the real guarantee) |
| `Untrusted<T>` validation boundaries, `Secret<T>` redaction | implemented |
| Checked integer arithmetic, exact `decimal`, array broadcasting | implemented |
| First-class `test` + `test prop` with shrinking | implemented |
| One canonical formatter (`keel fmt`), idempotent | implemented |
| Refinement types (`int where >= 0`) | runtime check — **not** an SMT proof |
| Ownership/borrowing, `comptime`, content-addressing, AOT/JIT, FFI | not modeled at runtime |

---

## Repository layout

```
src/keel.c            the Stage-0 reference interpreter (C11, the oracle)
compiler/keelc.keel   the Keel→C compiler, written in Keel (self-hosting)
runtime/              keelrt.{h,c} (runtime for compiled programs) +
                      keel_escape.h (the single-source context-string escapers)
tests/                conform.sh + equiv.sh, with positive/ negative/ golden/
                      property/ cases and the compiled/ equivalence corpus
bootstrap.sh          build the self-hosting chain and assert the fixpoint
keelc-build.sh        compile one .keel file to a native binary
README.md             this file
SPEC.md               the language design and specification
DESIGN.md             implementation notes: faithful vs. stand-in, in detail
PART1.md              the Part I self-hosting account
Makefile              build · conform · bootstrap · equiv · all-checks · test
```

---

## Documentation

- **[SPEC.md](SPEC.md)** — the language design and specification: thesis, lineage, the
  fundamental concepts (memory, types, effects, errors, refinement, numerics, …), the memory
  and security models, a runnable tour, the reference essentials, and the reserved-word and
  flagship-example appendices.
- **[DESIGN.md](DESIGN.md)** — implementation notes: each piece of the interpreter, compiler,
  and runtime, and exactly where the implementation is faithful versus a deliberate,
  documented stand-in for a later part.
- **[PART1.md](PART1.md)** — the Part I self-hosting account: the bootstrap fixpoint, the
  Keel-Core subset, and what the conformance and equivalence suites check.
- **[ROADMAP.md](ROADMAP.md)** — the staged build plan.

## License

See [LICENSE](LICENSE).
