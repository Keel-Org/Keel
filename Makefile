# Keel — Stage-0 reference interpreter
# A tree-walking interpreter for the Keel language, written in C11.

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
LDFLAGS ?= -lm

BIN      = bin/keel
SRC      = src/keel.c
EXAMPLES = $(wildcard examples/*.keel)

.PHONY: all clean test examples bootstrap fmt-check run

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Run every example program.
examples: $(BIN)
	@for f in $(EXAMPLES); do \
		echo "── $$f ───────────────────────────"; \
		./$(BIN) run $$f; \
		echo; \
	done

# Run the in-language test suites (unit + property tests).
test: $(BIN)
	@echo "── examples/05_tests.keel ──"; ./$(BIN) test examples/05_tests.keel; \
	echo; echo "── examples/06_flagship.keel ──"; ./$(BIN) test examples/06_flagship.keel

# Run the self-hosting demonstration: a Keel lexer, written in Keel,
# tokenizing Keel source on the C reference interpreter.
bootstrap: $(BIN)
	./$(BIN) run bootstrap/lexer.keel

# Verify the canonical formatter is idempotent on every example.
fmt-check: $(BIN)
	@for f in $(EXAMPLES) bootstrap/lexer.keel; do \
		./$(BIN) fmt $$f > /tmp/_k1 2>/dev/null; \
		./$(BIN) fmt /tmp/_k1 > /tmp/_k2 2>/dev/null; \
		if diff -q /tmp/_k1 /tmp/_k2 >/dev/null; then echo "ok   $$f"; \
		else echo "FAIL $$f (not idempotent)"; fi; \
	done

clean:
	rm -rf bin
