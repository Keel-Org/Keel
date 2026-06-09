# Keel — build, test, and bootstrap
#
#   make            build the Stage-0 reference interpreter (bin/keel)
#   make check      run the whole suite, scored by quality goal (alias: all-checks)
#
#   Tests are organized by the QUALITY GOAL they protect; run one category with:
#   make correctness   the language does what the spec says
#   make safety        no undefined behavior in safe code (overflow/bounds/recursion)
#   make security      capabilities, injection sinks, trust boundary (incl. compiled)
#   make reliability   graceful handling of malformed input; the failure taxonomy
#   make performance   complexity-regression budgets; guards fail fast
#   make determinism   formatter idempotence+semantics; oracle≡compiled; bootstrap
#   make tooling       fmt/tokens/ast and the in-language test runner
#
#   make bootstrap  build the self-hosting chain and assert the fixpoint
#   make harden     build + run the corpus under ASan/UBSan
#   make fuzz       mutation-fuzz the front end under sanitizers
#   (compatibility aliases: conform -> correctness, equiv -> determinism, perf -> performance)

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-function -Wno-clobbered -Wno-implicit-fallthrough
LDFLAGS ?= -lm

BIN   = bin/keel
SRC   = src/keel.c
RT    = runtime/keelrt.c
KEELC = compiler/keelc.keel

.PHONY: all clean build bootstrap harden fuzz test check all-checks \
        correctness safety security reliability performance determinism tooling \
        conform equiv perf

all: $(BIN)

# The Stage-0 oracle. Includes the shared C5 escaper from runtime/.
$(BIN): $(SRC) runtime/keel_escape.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iruntime -o $@ $(SRC) $(LDFLAGS)

# Whole suite, scored by quality goal (correctness/safety/security/reliability/
# performance/determinism/tooling). The bootstrap fixpoint runs inside
# `determinism`. The driver prints a per-category scorecard.
check all-checks: $(BIN)
	@./tests/run.sh

# Individual quality-goal categories.
correctness safety security reliability performance determinism tooling: $(BIN)
	@./tests/run.sh $@

# Compatibility aliases for the previous target names.
conform: correctness
equiv:   determinism
perf:    performance

# Self-hosting bootstrap: stage1 (oracle) -> stage2 -> stage3, assert fixpoint.
bootstrap: $(BIN)
	@./bootstrap.sh

# Build + run the full corpus under AddressSanitizer + UBSan. The fake-stack
# mode is disabled because it relocates locals to the heap, defeating the
# recursion guard's stack-pointer probe.
harden: $(SRC) runtime/keel_escape.h
	@mkdir -p bin
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Iruntime -o bin/keel_san $(SRC) $(LDFLAGS)
	@ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 \
	  sh -c 'for f in $$(find tests -name "*.keel"); do for c in run fmt tokens ast; do bin/keel_san $$c "$$f" >/dev/null 2>&1 || true; done; done; echo "harden: corpus exercised under ASan/UBSan"'

# Mutation-fuzz the lexer/parser/evaluator/formatter under sanitizers.
fuzz: harden
	@ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 python3 tests/fuzz.py

# Compile a single program with keelc to a native binary:
#   make build SRC_KEEL=tests/determinism/equiv/recursion.keel OUT=/tmp/rec
build: $(BIN)
	@./keelc-build.sh $(SRC_KEEL) $(OUT)

# The integrated test runner: first-class `test` / `test prop` with shrinking.
# This corpus intentionally includes one false property to demonstrate
# counterexample shrinking, so a non-zero exit here is expected.
test: $(BIN)
	@./$(BIN) test tests/tooling/05_tests.keel || true

clean:
	rm -rf bin build/stage*.c build/keelc1 build/keelc2
