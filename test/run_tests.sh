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

# 4. Threading variation produces same matched count as single-thread.
single=$("$BIN" --no-color -j 1 "INFO" "$PLAIN" | wc -l)
multi=$("$BIN" --no-color -j 8 "INFO" "$PLAIN" | wc -l)
if [[ "$single" != "$multi" ]]; then
    echo "FAIL [thread-parity] single=$single multi=$multi"; FAIL=$((FAIL+1))
else
    echo "ok   [thread-parity] count=$single"; PASS=$((PASS+1))
fi

# 5. Missing root path causes exit 2 (POSIX grep convention).
check "missing-file" 2 "cannot" "$BIN" --no-color "x" "/nonexistent/path.log"

# 6. No pattern at all is rejected (grep mode requires PATTERN).
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

# 7. Recursive walk finds matches in subdirs.
out=$("$BIN" --no-color needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" == *"b.c"* ]]; then
    echo "ok   [recurse]"; PASS=$((PASS+1))
else
    echo "FAIL [recurse] out=$out"; FAIL=$((FAIL+1))
fi

# 8. Default ignore: node_modules excluded.
if [[ "$out" == *"node_modules"* ]]; then
    echo "FAIL [default-ignore] node_modules leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [default-ignore]"; PASS=$((PASS+1))
fi

# 9. Hidden files skipped by default.
if [[ "$out" == *".hiddenfile"* ]]; then
    echo "FAIL [hidden-skip] hidden leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [hidden-skip]"; PASS=$((PASS+1))
fi

# 10. .gitignore excludes *.log.
if [[ "$out" == *"excluded.log"* ]]; then
    echo "FAIL [gitignore] excluded.log leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [gitignore]"; PASS=$((PASS+1))
fi

# 11. Binary detection: bin.dat skipped (no NUL warning).
if [[ "$out" == *"bin.dat"* ]]; then
    echo "FAIL [binary-skip] bin.dat leaked"; FAIL=$((FAIL+1))
else
    echo "ok   [binary-skip]"; PASS=$((PASS+1))
fi

# 12. -g include glob.
out=$("$BIN" --no-color -g '*.c' needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" != *"x.js"* ]]; then
    echo "ok   [glob-include]"; PASS=$((PASS+1))
else
    echo "FAIL [glob-include] out=$out"; FAIL=$((FAIL+1))
fi

# 13. -g exclude glob.
out=$("$BIN" --no-color -g '!*.c' --no-ignore needle "$SANDBOX" 2>&1)
if [[ "$out" == *"x.js"* && "$out" != *"a.c"* ]]; then
    echo "ok   [glob-exclude]"; PASS=$((PASS+1))
else
    echo "FAIL [glob-exclude] out=$out"; FAIL=$((FAIL+1))
fi

# 14. -t c selects only C files.
out=$("$BIN" --no-color -t c needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" != *"x.js"* ]]; then
    echo "ok   [type-filter]"; PASS=$((PASS+1))
else
    echo "FAIL [type-filter] out=$out"; FAIL=$((FAIL+1))
fi

# 15. JSON NDJSON validity (first event parses).
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

# 16. -A/-B/-C context.
mkdir -p "$TMPROOT/ctx"
printf "L1\nL2\nMATCH\nL4\nL5\n" > "$TMPROOT/ctx/file.txt"
CTXP="$(to_host_path "$TMPROOT/ctx/file.txt")"
out=$("$BIN" --no-color -C 1 MATCH "$CTXP")
if [[ "$out" == *"L2"* && "$out" == *"L4"* && "$out" == *"MATCH"* ]]; then
    echo "ok   [context-C]"; PASS=$((PASS+1))
else
    echo "FAIL [context-C] out=$out"; FAIL=$((FAIL+1))
fi

# 17. -c per-file count.
out=$("$BIN" --no-color -c needle "$SANDBOX" 2>&1)
if [[ "$out" == *":1"* ]]; then
    echo "ok   [count-per-file]"; PASS=$((PASS+1))
else
    echo "FAIL [count-per-file] out=$out"; FAIL=$((FAIL+1))
fi

# 18. -l files-with-matches.
out=$("$BIN" --no-color -l needle "$SANDBOX" 2>&1)
if [[ "$out" == *"a.c"* && "$out" != *"L2"* ]]; then
    echo "ok   [files-with-matches]"; PASS=$((PASS+1))
else
    echo "FAIL [files-with-matches] out=$out"; FAIL=$((FAIL+1))
fi

# 19. Multi-pattern -e.
out=$("$BIN" --no-color -e alpha -e beta "$SANDBOX" 2>&1)
if [[ "$out" == *"alpha"* && "$out" == *"beta"* ]]; then
    echo "ok   [multi-pattern]"; PASS=$((PASS+1))
else
    echo "FAIL [multi-pattern] out=$out"; FAIL=$((FAIL+1))
fi

# 20. Line numbers correctness across multi-chunk file.
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

# 21. --hidden includes hidden files.
out=$("$BIN" --no-color --hidden needle "$SANDBOX" 2>&1)
if [[ "$out" == *".hiddenfile"* ]]; then
    echo "ok   [hidden-flag]"; PASS=$((PASS+1))
else
    echo "FAIL [hidden-flag] out=$out"; FAIL=$((FAIL+1))
fi

# ---------- Cat subcommand tests ----------
# Build a content file for head/tail/range tests.
mkdir -p "$TMPROOT/cat"
seq 1 10 > "$TMPROOT/cat/ten.txt"
printf 'hello world\n' > "$TMPROOT/cat/single.txt"
CAT_TEN="$(to_host_path "$TMPROOT/cat/ten.txt")"
CAT_ONE="$(to_host_path "$TMPROOT/cat/single.txt")"

# 22. cat default dumps file.
out=$("$BIN" --cmd cat "$CAT_ONE" 2>&1)
if [[ "$out" == *"hello world"* ]]; then
    echo "ok   [cat-default]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-default] out=$out"; FAIL=$((FAIL+1))
fi

# 23. cat --head 3.
out=$("$BIN" --cmd cat --head 3 "$CAT_TEN" 2>&1)
if [[ "$(echo "$out" | wc -l)" -eq 3 && "$out" == "1"* && "$out" == *"3"* ]]; then
    echo "ok   [cat-head]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-head] out=$out"; FAIL=$((FAIL+1))
fi

# 24. cat --tail 3.
out=$("$BIN" --cmd cat --tail 3 "$CAT_TEN" 2>&1)
if [[ "$(echo "$out" | wc -l)" -eq 3 && "$out" == *"8"* && "$out" == *"10"* ]]; then
    echo "ok   [cat-tail]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-tail] out=$out"; FAIL=$((FAIL+1))
fi

# 25. cat --lines=4:6.
out=$("$BIN" --cmd cat --lines=4:6 "$CAT_TEN" 2>&1)
if [[ "$(echo "$out" | wc -l)" -eq 3 && "$out" == *"4"* && "$out" == *"6"* ]]; then
    echo "ok   [cat-lines-range]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-lines-range] out=$out"; FAIL=$((FAIL+1))
fi

# 26. cat --bytes=1:10.
out=$("$BIN" --cmd cat --bytes=1:10 "$CAT_ONE" 2>&1)
if [[ "$out" == "hello worl" ]]; then
    echo "ok   [cat-bytes]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-bytes] out=$out"; FAIL=$((FAIL+1))
fi

# 27. cat --compact multi-file prints headings.
out=$("$BIN" --cmd cat --compact "$CAT_ONE" "$CAT_TEN" 2>&1)
if [[ "$out" == *"==>"*"single.txt"*"<=="* && "$out" == *"==>"*"ten.txt"*"<=="* ]]; then
    echo "ok   [cat-compact]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-compact] out=$out"; FAIL=$((FAIL+1))
fi

# 28. cat --json NDJSON validity.
out=$("$BIN" --cmd cat --json "$CAT_ONE" 2>&1)
if [[ "$out" == '{"type":"begin"'* && "$out" == *'"type":"line"'* && "$out" == *'"type":"end"'* ]]; then
    echo "ok   [cat-json]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-json] out=$out"; FAIL=$((FAIL+1))
fi

# 29. cat --json --head 3 produces correct total_lines in begin event.
out=$("$BIN" --cmd cat --json --head 3 "$CAT_TEN" 2>&1)
total=$(echo "$out" | grep '"type":"begin"' | grep -o '"total_lines":[0-9]*' | cut -d: -f2)
if [[ "$total" == "10" ]]; then
    echo "ok   [cat-json-head]"; PASS=$((PASS+1))
else
    echo "FAIL [cat-json-head] out=$out"; FAIL=$((FAIL+1))
fi

# 30. cat missing file → exit 2.
check "cat-missing" 2 "" "$BIN" --cmd cat "/nonexistent/file.txt"

# 31. cat no args → exit 2.
check "cat-no-args" 2 "" "$BIN" --cmd cat

# 32. cat --help.
check "cat-help" 0 "--head" "$BIN" --cmd cat --help

# ---------- Wc subcommand tests ----------

# 33. wc default (lines words chars).
out=$("$BIN" --cmd wc "$CAT_TEN" 2>&1); rc=$?
if [[ $rc -eq 0 && "$out" == *"ten.txt"* ]]; then
    echo "ok   [wc-default]"; PASS=$((PASS+1))
else
    echo "FAIL [wc-default] rc=$rc out=$out"; FAIL=$((FAIL+1))
fi

# 34. wc --lines.
out=$("$BIN" --cmd wc --lines "$CAT_TEN" 2>&1)
if [[ "$out" == *"10"* ]]; then
    echo "ok   [wc-lines]"; PASS=$((PASS+1))
else
    echo "FAIL [wc-lines] out=$out"; FAIL=$((FAIL+1))
fi

# 35. wc --words.
out=$("$BIN" --cmd wc --words "$CAT_ONE" 2>&1)
if [[ "$out" == *"2"* ]]; then
    echo "ok   [wc-words]"; PASS=$((PASS+1))
else
    echo "FAIL [wc-words] out=$out"; FAIL=$((FAIL+1))
fi

# 36. wc --chars.
out=$("$BIN" --cmd wc --chars "$CAT_ONE" 2>&1)
if [[ "$out" == *"12"* ]]; then
    echo "ok   [wc-chars]"; PASS=$((PASS+1))
else
    echo "FAIL [wc-chars] out=$out"; FAIL=$((FAIL+1))
fi

# 37. wc --bytes (same as -c for ASCII).
out=$("$BIN" --cmd wc --bytes "$CAT_ONE" 2>&1)
if [[ "$out" == *"12"* ]]; then
    echo "ok   [wc-bytes]"; PASS=$((PASS+1))
else
    echo "FAIL [wc-bytes] out=$out"; FAIL=$((FAIL+1))
fi

# 38. wc multi-file with total row.
out=$("$BIN" --cmd wc --lines "$CAT_ONE" "$CAT_TEN" 2>&1)
if [[ "$out" == *"total"* ]]; then
    echo "ok   [wc-multi-total]"; PASS=$((PASS+1))
else
    echo "FAIL [wc-multi-total] out=$out"; FAIL=$((FAIL+1))
fi

# 39. wc --json NDJSON structure.
out=$("$BIN" --cmd wc --json "$CAT_ONE" 2>&1)
if [[ "$out" == '{"type":"count"'* && "$out" == *'"lines"'* && "$out" == *'"words"'* && "$out" == *'"chars"'* ]]; then
    echo "ok   [wc-json]"; PASS=$((PASS+1))
else
    echo "FAIL [wc-json] out=$out"; FAIL=$((FAIL+1))
fi

# 40. wc missing file → exit 2.
check "wc-missing" 2 "" "$BIN" --cmd wc "/nonexistent/file.txt"

# 41. wc --help.
check "wc-help" 0 "--words" "$BIN" --cmd wc --help

# ---------- Ls subcommand tests ----------
# Use src/cmd/ — flat directory containing main.c, cat.c, etc., for non-recursive
# ls tests. After the src/ subdir restructure the top-level src/ holds only dirs.
SRCDIR="$(to_host_path "$DIR/../src/cmd")"
SRCROOT="$(to_host_path "$DIR/../src")"

# 42. ls default (paths) shows known files.
out=$("$BIN" --cmd ls "$SRCDIR" 2>&1)
if [[ "$out" == *"main.c"* && "$out" == *"cat.c"* ]]; then
    echo "ok   [ls-paths]"; PASS=$((PASS+1))
else
    echo "FAIL [ls-paths] out=$out"; FAIL=$((FAIL+1))
fi

# 43. ls --display long shows size column.
out=$("$BIN" --cmd ls --display long "$SRCDIR" 2>&1)
if [[ "$out" == *"main.c"* && "$out" == *"-rw"* ]]; then
    echo "ok   [ls-long]"; PASS=$((PASS+1))
else
    echo "FAIL [ls-long] out=$out"; FAIL=$((FAIL+1))
fi

# 44. ls --json NDJSON structure.
out=$("$BIN" --cmd ls --json "$SRCDIR" 2>&1)
if [[ "$out" == '{"type":"entry"'* && "$out" == *'"path"'* && "$out" == *'"is_dir"'* ]]; then
    echo "ok   [ls-json]"; PASS=$((PASS+1))
else
    echo "FAIL [ls-json] out=$out"; FAIL=$((FAIL+1))
fi

# 45. ls --compact outputs entries.
out=$("$BIN" --cmd ls --compact "$SRCDIR" 2>&1)
if [[ "$out" == *"main.c"* && "$out" == *"cat.c"* ]]; then
    echo "ok   [ls-compact]"; PASS=$((PASS+1))
else
    echo "FAIL [ls-compact] out=$out"; FAIL=$((FAIL+1))
fi

# 46. ls --display tree shows indented names.
out=$("$BIN" --cmd ls --display tree "$SRCDIR" 2>&1)
if [[ "$out" == *"main.c"* && "$out" == *"cat.c"* ]]; then
    echo "ok   [ls-tree]"; PASS=$((PASS+1))
else
    echo "FAIL [ls-tree] out=$out"; FAIL=$((FAIL+1))
fi

# 47. ls --recursive shows subdirectory contents.
out=$("$BIN" --cmd ls --recursive "$SRCROOT" 2>&1)
if [[ "$out" == *"src"* && "$out" == *"main.c"* && "$out" == *"cat.c"* ]]; then
    echo "ok   [ls-recursive]"; PASS=$((PASS+1))
else
    echo "FAIL [ls-recursive] out=$out"; FAIL=$((FAIL+1))
fi

# 48. ls missing path → exit 2.
check "ls-missing" 2 "" "$BIN" --cmd ls "/nonexistent/dir_xyz"

# 49. ls --help.
check "ls-help" 0 "--display" "$BIN" --cmd ls --help

# ---------- Find subcommand tests ----------

# 50. find default shows files.
out=$("$BIN" --cmd find "$SRCDIR" 2>&1)
if [[ "$out" == *".c"* && "$out" == *".h"* ]]; then
    echo "ok   [find-default]"; PASS=$((PASS+1))
else
    echo "FAIL [find-default] out=$out"; FAIL=$((FAIL+1))
fi

# 51. find --name "*.c" only matches .c files.
out=$("$BIN" --cmd find --name "*.c" "$SRCDIR" 2>&1)
if [[ "$out" == *".c"* && "$out" != *".h"* ]]; then
    echo "ok   [find-name]"; PASS=$((PASS+1))
else
    echo "FAIL [find-name] out=$out"; FAIL=$((FAIL+1))
fi

# 52. find --type d only directories.
out=$("$BIN" --cmd find --type d "$SRCDIR" 2>&1)
if [[ "$out" == *"src"* && "$out" != *".c"* && "$out" != *".h"* ]]; then
    echo "ok   [find-type-d]"; PASS=$((PASS+1))
else
    echo "FAIL [find-type-d] out=$out"; FAIL=$((FAIL+1))
fi

# 53. find --type f only files.
out=$("$BIN" --cmd find --type f "$SRCDIR" 2>&1)
if [[ "$out" == *".c"* ]]; then
    echo "ok   [find-type-f]"; PASS=$((PASS+1))
else
    echo "FAIL [find-type-f] out=$out"; FAIL=$((FAIL+1))
fi

# 54. find --size +0 finds files with content.
out=$("$BIN" --cmd find --size +0 "$SRCDIR" 2>&1)
if [[ "$out" == *"main.c"* ]]; then
    echo "ok   [find-size]"; PASS=$((PASS+1))
else
    echo "FAIL [find-size] out=$out"; FAIL=$((FAIL+1))
fi

# 55. find --json NDJSON structure.
out=$("$BIN" --cmd find --json "$SRCDIR" 2>&1)
if [[ "$out" == '{"type":"entry"'* && "$out" == *'"path"'* && "$out" == *'"is_dir"'* ]]; then
    echo "ok   [find-json]"; PASS=$((PASS+1))
else
    echo "FAIL [find-json] out=$out"; FAIL=$((FAIL+1))
fi

# 56. find missing path → exit 2.
check "find-missing" 2 "" "$BIN" --cmd find "/nonexistent/dir_xyz"

# 57. find --help.
check "find-help" 0 "--name" "$BIN" --cmd find --help

echo
echo "passed: $PASS, failed: $FAIL"
[[ "$FAIL" -eq 0 ]]
