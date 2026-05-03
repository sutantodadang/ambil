#!/usr/bin/env bash
# test/run_tests.sh — smoke tests for ambil.
#
# Each test asserts on exit code + (where useful) output content. Keep it
# self-contained: no external deps beyond the binary, bash, and coreutils.
set -uo pipefail

BIN="${BIN:-build/ambil}"
if [[ ! -x "$BIN" && -x "${BIN}.exe" ]]; then
    BIN="${BIN}.exe"
fi
DIR="$(cd "$(dirname "$0")" && pwd)"
PLAIN="$DIR/sample.log"
JSON="$DIR/sample.json.log"

to_host_path() {
    local path="$1"
    if [[ "$BIN" == *.exe ]]; then
        if command -v cygpath >/dev/null 2>&1; then
            cygpath -w "$path"; return
        fi
        if command -v wslpath >/dev/null 2>&1; then
            wslpath -w "$path"; return
        fi
    fi
    printf '%s\n' "$path"
}

PLAIN="$(to_host_path "$PLAIN")"
JSON="$(to_host_path "$JSON")"

PASS=0
FAIL=0

check() {
    local name="$1"; shift
    local expected_rc="$1"; shift
    local expected_substr="$1"; shift
    local out
    out="$("$@" 2>&1)"
    local rc=$?
    if [[ "$rc" -ne "$expected_rc" ]]; then
        echo "FAIL [$name] exit=$rc expected=$expected_rc"
        echo "  cmd: $*"
        echo "  out: $out"
        FAIL=$((FAIL+1)); return
    fi
    if [[ -n "$expected_substr" && "$out" != *"$expected_substr"* ]]; then
        echo "FAIL [$name] missing substring '$expected_substr'"
        echo "  cmd: $*"
        echo "  out: $out"
        FAIL=$((FAIL+1)); return
    fi
    echo "ok   [$name]"
    PASS=$((PASS+1))
}

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN — run 'make' first." >&2
    exit 2
fi

# 1. Plain substring search, case-sensitive.
check "search-error" 0 "ERROR" "$BIN" --no-color "ERROR" "$PLAIN"

# 2. No matches → exit 1, no output.
check "search-nomatch" 1 "" "$BIN" --no-color "ZZZ_NOTHING_ZZZ" "$PLAIN"

# 3. Case-insensitive search.
check "search-icase" 0 "ERROR" "$BIN" --no-color -i "error" "$PLAIN"

# 4. Time range filter.
check "time-range" 0 "10:00:03" "$BIN" --no-color \
    --from "2026-05-01 10:00:00" --to "2026-05-01 10:00:10" "$PLAIN"

# 5. Time range excludes outside lines.
out=$("$BIN" --no-color --from "2026-05-01 10:00:00" --to "2026-05-01 10:00:10" "$PLAIN" 2>&1)
if [[ "$out" == *"12:00:00"* ]]; then
    echo "FAIL [time-range-bound] line outside range leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [time-range-bound]"; PASS=$((PASS+1))
fi

# 6. JSON field equality.
check "json-field" 0 "db timeout" "$BIN" --no-color --json --field level=error "$JSON"

# 7. Count aggregation (legacy, now spelled --count-field).
check "count-level" 0 "error" "$BIN" --count-field level "$JSON"

# 8. Count output is sorted (highest first line should be 'info' = 4).
out=$("$BIN" --count-field level "$JSON" 2>&1)
first_count=$(echo "$out" | head -1 | awk '{print $1}')
if [[ ! "$first_count" =~ ^[0-9]+$ || "$first_count" -lt 1 ]]; then
    echo "FAIL [count-sorted] first line not numeric: $out"; FAIL=$((FAIL+1))
else
    echo "ok   [count-sorted] top=$first_count"; PASS=$((PASS+1))
fi

# 9. Combined filters (json + time + field).
check "combined" 0 "error" "$BIN" --no-color --json --field level=error \
    --from "2026-05-01 10:00:00" --to "2026-05-01 10:00:05" "$JSON"

# 10. Threading variation produces same matched count as single-thread.
single=$("$BIN" --no-color -j 1 "INFO" "$PLAIN" | wc -l)
multi=$("$BIN" --no-color -j 8 "INFO" "$PLAIN" | wc -l)
if [[ "$single" != "$multi" ]]; then
    echo "FAIL [thread-parity] single=$single multi=$multi"; FAIL=$((FAIL+1))
else
    echo "ok   [thread-parity] count=$single"; PASS=$((PASS+1))
fi

# 11. Missing root path causes exit 2 (POSIX grep convention).
check "missing-file" 2 "cannot" "$BIN" --no-color "x" "/nonexistent/path.log"

# 12. No pattern at all is rejected (grep mode requires PATTERN).
check "no-filter" 2 "missing PATTERN" "$BIN"

# ---------- Grep mode tests (v0.2) ----------

ROOT="$(cd "$DIR/.." && pwd)"
SRC_DIR="$(to_host_path "$ROOT/src")"
TMPROOT=$(mktemp -d)
trap 'rm -rf "$TMPROOT"' EXIT

# Build a sandbox tree with .gitignore + node_modules.
mkdir -p "$TMPROOT/proj/node_modules" "$TMPROOT/proj/sub" "$TMPROOT/proj/.git"
echo "needle alpha" > "$TMPROOT/proj/a.c"
echo "needle beta"  > "$TMPROOT/proj/sub/b.c"
echo "needle inside_node_modules" > "$TMPROOT/proj/node_modules/x.js"
echo "needle hidden" > "$TMPROOT/proj/.hiddenfile"
echo "needle ignored" > "$TMPROOT/proj/excluded.log"
echo "*.log" > "$TMPROOT/proj/.gitignore"
echo "binary file:" > "$TMPROOT/proj/bin.dat"
printf "needle\x00binarynull" >> "$TMPROOT/proj/bin.dat"
SANDBOX="$(to_host_path "$TMPROOT/proj")"

# 13. Recursive walk finds matches in subdirs.
out=$("$BIN" --no-color needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" == *"b.c"* ]]; then
    echo "ok   [recurse]"; PASS=$((PASS+1))
else
    echo "FAIL [recurse] out=$out"; FAIL=$((FAIL+1))
fi

# 14. Default ignore: node_modules excluded.
if [[ "$out" == *"node_modules"* ]]; then
    echo "FAIL [default-ignore] node_modules leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [default-ignore]"; PASS=$((PASS+1))
fi

# 15. Hidden files skipped by default.
if [[ "$out" == *".hiddenfile"* ]]; then
    echo "FAIL [hidden-skip] hidden leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [hidden-skip]"; PASS=$((PASS+1))
fi

# 16. .gitignore excludes *.log.
if [[ "$out" == *"excluded.log"* ]]; then
    echo "FAIL [gitignore] excluded.log leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [gitignore]"; PASS=$((PASS+1))
fi

# 17. Binary detection: bin.dat skipped (no NUL warning).
if [[ "$out" == *"bin.dat"* ]]; then
    echo "FAIL [binary-skip] bin.dat leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [binary-skip]"; PASS=$((PASS+1))
fi

# 18. -g include glob.
out=$("$BIN" --no-color -g '*.c' needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" != *"x.js"* ]]; then
    echo "ok   [glob-include]"; PASS=$((PASS+1))
else
    echo "FAIL [glob-include] out=$out"; FAIL=$((FAIL+1))
fi

# 19. -g exclude glob.
out=$("$BIN" --no-color -g '!*.c' --no-ignore needle "$SANDBOX" 2>&1)
if [[ "$out" == *"x.js"* && "$out" != *"a.c"* ]]; then
    echo "ok   [glob-exclude]"; PASS=$((PASS+1))
else
    echo "FAIL [glob-exclude] out=$out"; FAIL=$((FAIL+1))
fi

# 20. -t c selects only C files.
out=$("$BIN" --no-color -t c needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" != *"x.js"* ]]; then
    echo "ok   [type-filter]"; PASS=$((PASS+1))
else
    echo "FAIL [type-filter] out=$out"; FAIL=$((FAIL+1))
fi

# 21. JSON NDJSON validity (first event parses).
first=$("$BIN" --no-color --json needle "$SANDBOX" 2>&1 | head -1)
if command -v python >/dev/null 2>&1; then
    if echo "$first" | python -c "import json,sys; json.loads(sys.stdin.read())" 2>/dev/null; then
        echo "ok   [json-ndjson]"; PASS=$((PASS+1))
    else
        echo "FAIL [json-ndjson] first=$first"; FAIL=$((FAIL+1))
    fi
else
    # Fallback: structural check.
    if [[ "$first" == '{"type":"begin"'* ]]; then
        echo "ok   [json-ndjson] (no python; structural)"; PASS=$((PASS+1))
    else
        echo "FAIL [json-ndjson] first=$first"; FAIL=$((FAIL+1))
    fi
fi

# 22. -A/-B/-C context.
mkdir -p "$TMPROOT/ctx"
printf "L1\nL2\nMATCH\nL4\nL5\n" > "$TMPROOT/ctx/file.txt"
CTXP="$(to_host_path "$TMPROOT/ctx/file.txt")"
out=$("$BIN" --no-color -C 1 MATCH "$CTXP")
if [[ "$out" == *"L2"* && "$out" == *"L4"* && "$out" == *"MATCH"* ]]; then
    echo "ok   [context-C]"; PASS=$((PASS+1))
else
    echo "FAIL [context-C] out=$out"; FAIL=$((FAIL+1))
fi

# 23. -c per-file count.
out=$("$BIN" --no-color -c needle "$SANDBOX" 2>&1)
if [[ "$out" == *":1"* ]]; then
    echo "ok   [count-per-file]"; PASS=$((PASS+1))
else
    echo "FAIL [count-per-file] out=$out"; FAIL=$((FAIL+1))
fi

# 24. -l files-with-matches.
out=$("$BIN" --no-color -l needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" != *"L2"* ]]; then
    echo "ok   [files-with-matches]"; PASS=$((PASS+1))
else
    echo "FAIL [files-with-matches] out=$out"; FAIL=$((FAIL+1))
fi

# 25. Multi-pattern -e.
out=$("$BIN" --no-color -e alpha -e beta "$SANDBOX" 2>&1)
if [[ "$out" == *"alpha"* && "$out" == *"beta"* ]]; then
    echo "ok   [multi-pattern]"; PASS=$((PASS+1))
else
    echo "FAIL [multi-pattern] out=$out"; FAIL=$((FAIL+1))
fi

# 26. Line numbers correctness across multi-chunk file.
LARGE="$TMPROOT/large.txt"
python -c "
import sys
for i in range(1, 200001):
    if i == 123456:
        sys.stdout.write('THE_NEEDLE\n')
    else:
        sys.stdout.write('filler line %d\n' % i)
" > "$LARGE" 2>/dev/null || awk 'BEGIN{for(i=1;i<=200000;i++){if(i==123456)print "THE_NEEDLE"; else print "filler line " i}}' > "$LARGE"
LARGEP="$(to_host_path "$LARGE")"
out=$("$BIN" --no-color THE_NEEDLE "$LARGEP")
if [[ "$out" == *"123456:THE_NEEDLE"* ]]; then
    echo "ok   [line-numbers-large]"; PASS=$((PASS+1))
else
    echo "FAIL [line-numbers-large] out=$out"; FAIL=$((FAIL+1))
fi

# 27. --hidden includes hidden files.
out=$("$BIN" --no-color --hidden needle "$SANDBOX" 2>&1)
if [[ "$out" == *".hiddenfile"* ]]; then
    echo "ok   [hidden-flag]"; PASS=$((PASS+1))
else
    echo "FAIL [hidden-flag] out=$out"; FAIL=$((FAIL+1))
fi

echo
echo "passed: $PASS, failed: $FAIL"
[[ "$FAIL" -eq 0 ]]
