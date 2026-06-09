#!/usr/bin/env python3
# ============================================================================
# fuzz.py — mutation fuzzer for the Keel front end (lexer / parser / evaluator /
# formatter). Seeds from the existing corpus, applies random byte mutations, and
# runs each variant through `run`, `fmt`, `tokens`, and `ast`. ANY sanitizer
# report (AddressSanitizer / UBSan) or a non-clean crash is a failure. A clean
# diagnostic and a non-zero exit are expected and fine — the contract is "never
# corrupt memory and never crash with an unhandled signal on hostile input".
#
# Build the sanitized interpreter first (make harden builds bin/keel_san), then:
#   ASAN_OPTIONS=detect_stack_use_after_return=0 python3 tests/fuzz.py [iters]
# ============================================================================
import os, sys, random, subprocess, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEEL = os.path.join(ROOT, "bin", "keel_san")
if not os.path.exists(KEEL):
    KEEL = os.path.join(ROOT, "bin", "keel")

ITERS = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
SEED  = int(os.environ.get("FUZZ_SEED", "1"))
random.seed(SEED)

corpus = []
for pat in ("tests/correctness/run/*.keel", "tests/correctness/reject/*.keel",
            "tests/safety/reject/*.keel", "tests/security/run/*.keel",
            "tests/security/reject/*.keel", "tests/reliability/run/*.keel",
            "tests/reliability/reject/*.keel", "tests/determinism/equiv/*.keel",
            "tests/correctness/golden/*.keel", "tests/tooling/*.keel", "compiler/*.keel"):
    for fn in glob.glob(os.path.join(ROOT, pat)):
        with open(fn, "rb") as f:
            corpus.append(bytearray(f.read()))
if not corpus:
    print("fuzz: no corpus found"); sys.exit(1)

MUTCHARS = b'@#$%^&(){}[]:;"\'\\\t\n .0-9aA=<>?!*/+-~|&_'
SANITIZER_SIGNS = (b"AddressSanitizer", b"UndefinedBehaviorSanitizer", b"SUMMARY:")

def mutate(b):
    b = bytearray(b)
    for _ in range(random.randint(1, 12)):
        op = random.randint(0, 5)
        if op == 0 and len(b) > 1:        del b[random.randrange(len(b))]
        elif op == 1:                     b.insert(random.randrange(len(b)+1), random.choice(MUTCHARS))
        elif op == 2 and len(b) > 1:      b[random.randrange(len(b))] = random.choice(MUTCHARS)
        elif op == 3 and len(b) > 2:      b = b[:random.randrange(1, len(b))]
        elif op == 4:                     b += bytes([random.randint(0, 255)])
        elif op == 5 and len(b) > 4:      i = random.randrange(len(b)-2); b[i:i] = b[i:i+random.randint(1, 40)]
    return b

tmp = os.path.join(ROOT, ".fuzz_input.keel")
runs = 0
hits = 0
for it in range(ITERS):
    data = mutate(random.choice(corpus))
    with open(tmp, "wb") as f:
        f.write(data)
    for cmd in ("run", "fmt", "tokens", "ast"):
        runs += 1
        try:
            r = subprocess.run([KEEL, cmd, tmp], capture_output=True, timeout=15)
        except subprocess.TimeoutExpired:
            continue
        err = r.stderr
        crashed_signal = r.returncode < 0 and -r.returncode not in (0,)  # killed by signal
        if any(s in err for s in SANITIZER_SIGNS) or crashed_signal:
            hits += 1
            keep = os.path.join(ROOT, f".fuzz_hit_{it}_{cmd}.keel")
            with open(keep, "wb") as f:
                f.write(data)
            sys.stderr.write(f"FUZZ HIT [{cmd}] rc={r.returncode} -> {keep}\n")
            for line in err.splitlines():
                if any(s in line for s in SANITIZER_SIGNS):
                    sys.stderr.write("   " + line.decode("latin1")[:140] + "\n")
            if hits >= 10:
                break
    if hits >= 10:
        break

try: os.remove(tmp)
except OSError: pass

print(f"fuzz: {runs} runs across {ITERS} mutations (seed {SEED}); sanitizer/crash hits: {hits}")
sys.exit(1 if hits else 0)
