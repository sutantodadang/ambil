#!/usr/bin/env bash
# test/test_regex.sh — sanity-check the embedded re_tiny regex engine.
#
# Each case runs `ambil -E PATTERN file` and compares the match count to
# `grep -cE PATTERN file`. We use the SUBSET of regex that re_tiny supports
# (no groups, no alternation) so the baselines agree.

set -eu

BIN="${BIN:-build/ambil}"
if [[ ! -x "$BIN" && -x "${BIN}.exe" ]]; then BIN="${BIN}.exe"; fi
if [[ ! -x "$BIN" ]]; then echo "missing $BIN" >&2; exit 2; fi

to_host_path() {
    local p="$1"
    if [[ "$BIN" == *.exe ]]; then
        command -v cygpath >/dev/null 2>&1 && { cygpath -w "$p"; return; }
        command -v wslpath >/dev/null 2>&1 && { wslpath -w "$p"; return; }
    fi
    printf '%s\n' "$p"
}

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

cat > "$TMP" <<'EOF'
hello world
HELLO World
foo 42 bar
foo bar baz
404 not found
status: 200 ok
error: timeout
ERROR: TIMEOUT
abc123
test_function_name
TestFunctionName
__init__
the quick brown fox
THE QUICK BROWN FOX
class Foo:
def bar():
    return 42
[INFO] heartbeat
EOF

HOST="$(to_host_path "$TMP")"
PASS=0; FAIL=0

check() {
    local label="$1" pat="$2" flags="${3:-}" baseline="${4:-}"
    local a g
    a=$("$BIN" --no-color $flags -E -c "$pat" "$HOST" 2>/dev/null | awk -F: '{s+=$NF} END{print s+0}')
    # baseline overrides the auto-derived `grep -E` count when the pattern
    # uses ambil/re_tiny extensions that POSIX ERE doesn't (e.g., \d \w \s).
    if [[ -n "$baseline" ]]; then
        g="$baseline"
    elif [[ "$flags" == *"-i"* ]]; then
        g=$(grep -cEi "$pat" "$TMP" 2>/dev/null || true)
    else
        g=$(grep -cE  "$pat" "$TMP" 2>/dev/null || true)
    fi
    if [[ "$a" == "$g" ]]; then
        echo "ok   [$label] $a matches"; PASS=$((PASS+1))
    else
        echo "FAIL [$label] ambil=$a expected=$g pat=$pat"; FAIL=$((FAIL+1))
    fi
}

check "literal-anchored"    "^hello"
check "dot-star"            "foo.*bar"
check "digits"              "[0-9]+"
check "class-range-ci"      "[A-Z]+" "-i"
check "wordchar"            "\\w+"  ""  "$(grep -cE '[A-Za-z0-9_]+' "$TMP" || true)"
check "neg-class"           "[^a-z]+"
check "anchor-end"          "fox$"
check "optional-plus"       "ab?c"
# POSIX ERE doesn't accept \d \s — compute baseline with equivalent classes.
check "shorthand-d"         "\\d\\d\\d"  ""  "$(grep -cE '[0-9][0-9][0-9]' "$TMP" || true)"
check "shorthand-s"         "\\s+"        ""  "$(grep -cE '[[:space:]]+' "$TMP" || true)"

echo
echo "regex: passed=$PASS failed=$FAIL"
[[ "$FAIL" -eq 0 ]] || exit 1
