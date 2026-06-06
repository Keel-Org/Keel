# Keel — Stage-0 Reference Interpreter

A complete, single-file tree-walking interpreter for **Keel**, the memory-safe,
GC-free, effects-oriented systems language described in `spec/README.md`. Written
in C11 with no dependencies beyond libc/libm.

This is "Stage 0" from the language's bootstrap plan (spec §9): the reference
implementation that brings the language up so it can then be rewritten in itself.

```
make            # build bin/keel
make examples   # run every example program
make test       # run in-language unit + property tests
make bootstrap  # run the Keel lexer that is written in Keel
make fmt-check  # verify the canonical formatter is idempotent
```

## What works

| Capability | Status |
|---|---|
| Significant-indentation lexer, Pratt parser | faithful |
| Structs, enums, pattern matching, options (`T?`, `??`, `?.`) | faithful |
| Algebraic effects + handlers, `resume` / abort | faithful (one-shot, in-scope) |
| Capability-based authority, narrowing, traversal rejection | faithful in shape |
| Context-typed strings (`sql""` etc.) — injection can't type-check | faithful (the real guarantee) |
| `Untrusted<T>` validation boundaries, `Secret<T>` redaction | faithful |
| Checked integer arithmetic, exact `decimal`, array broadcasting | faithful |
| Refinement types (`int where >= 0`) | **runtime check, not an SMT proof** |
| First-class tests + property tests with shrinking | faithful |
| One canonical formatter (`keel fmt`), idempotent | faithful |
| Ownership/borrowing, comptime, content-addressing, AOT | not modeled at runtime |

See `DESIGN.md` for the full, honest account of what is faithful versus a
deliberate stand-in.

## The bootstrap demonstration

`bootstrap/lexer.keel` is a lexer for Keel **written in Keel**, running on this C
interpreter, tokenizing Keel source. It is the first concrete link in the
self-hosting chain — proof the language is already expressive enough to begin
describing itself.

```
$ ./bin/keel run bootstrap/lexer.keel
source: def square(x): return x * x
    KeywordTok -> def
    IdentTok -> square
    OpTok -> (
    ...
```

## Layout

```
src/keel.c            the entire interpreter
bootstrap/lexer.keel  a Keel lexer written in Keel (Stage-0 → Stage-1)
examples/             01 basics · 02 types/match · 03 effects ·
                      04 security · 05 tests · 06 flagship (Appendix B)
spec/README.md        the language specification
DESIGN.md             implementation notes: faithful vs. stand-in
Makefile
```
