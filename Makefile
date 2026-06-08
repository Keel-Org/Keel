# Keel — build, test, and bootstrap
#
#   make            build the Stage-0 reference interpreter (bin/keel)
#   make conform    run the conformance suite (executable specification)
#   make bootstrap  build the self-hosting chain and assert the fixpoint
#   make equiv      check the self-hosted compiler agrees with the oracle
#   make examples   run every example program
#   make test       run the in-language test suites
#   make all-checks conform + bootstrap + equiv

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-function
LDFLAGS ?= -lm
RTFLAGS ?= -std=c11 -w -Iruntime

BIN      = bin/keel
SRC      = src/keel.c
RT       = runtime/keelrt.c
KEELC    = compiler/keelc.keel
EXAMPLES = $(wildcard examples/*.keel)

.PHONY: all clean conform bootstrap equiv examples test all-checks fmt-check

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

# Compile a single program with keelc to a native binary:  make build SRC=x.keel OUT=x
build: $(BIN)
	@./keelc-build.sh $(SRC_KEEL) $(OUT)

examples: $(BIN)
	@for f in $(EXAMPLES); do \
		echo "── $$f ───────────────────────────"; \
		./$(BIN) run $$f; echo; \
	done

test: $(BIN)
	@echo "── examples/05_tests.keel ──"; ./$(BIN) test examples/05_tests.keel; \
	echo; echo "── examples/06_flagship.keel ──"; ./$(BIN) test examples/06_flagship.keel

all-checks: conform bootstrap equiv
	@echo "all checks passed"

fmt-check: $(BIN)
	@for f in $(EXAMPLES); do \
		./$(BIN) fmt $$f > /tmp/_k1 2>/dev/null; \
		./$(BIN) fmt /tmp/_k1 > /tmp/_k2 2>/dev/null; \
		if diff -q /tmp/_k1 /tmp/_k2 >/dev/null; then echo "ok   $$f"; \
		else echo "FAIL $$f (not idempotent)"; fi; \
	done

clean:
	rm -rf bin build/stage*.c build/keelc1 build/keelc2
