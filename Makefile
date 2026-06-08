# Keel — build, test, and bootstrap
#
#   make            build the Stage-0 reference interpreter (bin/keel)
#   make conform    run the conformance suite (the executable specification)
#   make bootstrap  build the self-hosting chain and assert the fixpoint
#   make equiv      check the self-hosted compiler agrees with the oracle
#   make all-checks conform + bootstrap + equiv
#   make test       run the in-language test runner (unit + property/shrinking)

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-function -Wno-clobbered -Wno-implicit-fallthrough
LDFLAGS ?= -lm

BIN   = bin/keel
SRC   = src/keel.c
RT    = runtime/keelrt.c
KEELC = compiler/keelc.keel

.PHONY: all clean conform bootstrap equiv build test all-checks

all: $(BIN)

# The Stage-0 oracle. Includes the shared C5 escaper from runtime/.
$(BIN): $(SRC) runtime/keel_escape.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Iruntime -o $@ $(SRC) $(LDFLAGS)

# Conformance suite: positive / negative / golden / property (Tier 1.2 / C2).
conform: $(BIN)
	@./tests/conform.sh

# Self-hosting bootstrap: stage1 (oracle) -> stage2 -> stage3, assert fixpoint.
bootstrap: $(BIN)
	@./bootstrap.sh

# Behavioral equivalence: oracle vs self-hosted-compiled, over the corpus.
equiv: $(BIN)
	@./tests/equiv.sh

all-checks: conform bootstrap equiv
	@echo "all checks passed"

# Compile a single program with keelc to a native binary:
#   make build SRC_KEEL=tests/compiled/recursion.keel OUT=/tmp/rec
build: $(BIN)
	@./keelc-build.sh $(SRC_KEEL) $(OUT)

# The integrated test runner: first-class `test` / `test prop` with shrinking.
# This corpus intentionally includes one false property to demonstrate
# counterexample shrinking, so a non-zero exit here is expected.
test: $(BIN)
	@./$(BIN) test tests/property/05_tests.keel || true

clean:
	rm -rf bin build/stage*.c build/keelc1 build/keelc2
