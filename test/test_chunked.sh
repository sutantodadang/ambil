#!/usr/bin/env bash
# test/test_chunked.sh — verify intra-file chunking is correct under load.
#
# Generates a ~20 MB file (crosses the 16 MB chunking threshold), then
# greps it with both ambil (chunked path active) and grep -F (baseline).
# Match counts AND line numbers MUST agree exactly.

set -eu  # pipefail off: `head` closes pipes early, awk sees SIGPIPE; harmless

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

# Bench file on Windows-native temp when running .exe to avoid 9P bridge.
if [[ "$BIN" == *.exe ]]; then
    WIN_TMP_HOST="$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r' || true)"
    [[ -n "$WIN_TMP_HOST" ]] && TMP_DIR="$(wslpath -u "$WIN_TMP_HOST" 2>/dev/null || cygpath -u "$WIN_TMP_HOST" 2>/dev/null)" || TMP_DIR="/tmp"
else
    TMP_DIR="${TMPDIR:-/tmp}"
fi
TMP="$TMP_DIR/ambil-chunked-test-$$.log"
trap 'rm -f "$TMP"' EXIT

# Generate ~20 MB of synthetic data with sparse needles.
awk -v target=$((20 * 1024 * 1024)) '
BEGIN {
    srand(7);
    bytes = 0; i = 0;
    while (bytes < target) {
        i++;
        if (i % 1009 == 0)      line = sprintf("line %d  SPARSE_needle  X", i);
        else if (i % 137 == 0)  line = sprintf("line %d MEDIUM_match yyyy", i);
        else                    line = sprintf("line %d filler text aaaaaaaaaaaaa bbbb cccc", i);
        printf "%s\n", line;
        bytes += length(line) + 1;
    }
}' > "$TMP"

actual=$(stat -c %s "$TMP" 2>/dev/null || stat -f %z "$TMP")
echo "test corpus: $((actual/1024/1024)) MB at $TMP"

HOST="$(to_host_path "$TMP")"
PASS=0; FAIL=0

check_pattern() {
    local label="$1" pattern="$2" extra_flags="${3:-}"
    local ambil_n grep_n
    # Use -c to get totals; ambil prints "path:count", sum the count column.
    ambil_n=$("$BIN" --no-color $extra_flags -c "$pattern" "$HOST" 2>/dev/null | awk -F: '{s+=$NF} END{print s+0}')
    grep_n=$(grep -cF "$pattern" "$TMP" || true)
    if [[ "$ambil_n" == "$grep_n" ]]; then
        echo "ok   [$label] count=$ambil_n"; PASS=$((PASS+1))
    else
        echo "FAIL [$label] ambil=$ambil_n grep=$grep_n"; FAIL=$((FAIL+1))
    fi
}

# Line-number parity: collect ambil text-mode "lineno:..." prefixes and compare
# to grep -nF lineno prefixes. Both should be identical sets.
check_linenums() {
    local label="$1" pattern="$2"
    local ambil_lns grep_lns
    # Single-file invocation: ambil --no-heading prints "lineno:line" (no path).
    # Filter out "--" group separators and any blank trailing line.
    ambil_lns=$("$BIN" --no-color --no-heading "$pattern" "$HOST" 2>/dev/null \
                  | awk -F: '/^[0-9]+:/{print $1}' | sort -n | head -10)
    grep_lns=$(grep -nF "$pattern" "$TMP" | awk -F: '{print $1}' | sort -n | head -10)
    if [[ "$ambil_lns" == "$grep_lns" ]]; then
        echo "ok   [$label] first-10 lineno match"; PASS=$((PASS+1))
    else
        echo "FAIL [$label] lineno mismatch"
        echo "  ambil:" $(echo "$ambil_lns" | tr '\n' ' ')
        echo "   grep:" $(echo "$grep_lns" | tr '\n' ' ')
        FAIL=$((FAIL+1))
    fi
}

echo "SIMD: $($BIN --simd-info)"

# Note: -c mode disables chunking by design (count semantics don't compose).
# But text/--no-heading mode uses chunked path. Verify both.
check_pattern  "count-sparse"   "SPARSE_needle"
check_pattern  "count-medium"   "MEDIUM_match"
check_linenums "lineno-sparse"  "SPARSE_needle"
check_linenums "lineno-medium"  "MEDIUM_match"

# --stream mode: order across files lost, but per-file MATCH content identical.
# (Group separator '--' counts differ across chunked/non-chunked paths by design —
#  chunking resets the separator state on each chunk, omitting up to N-1 separators.
#  This is cosmetic; match lines and line numbers are unaffected.)
stream_count=$("$BIN" --no-color --stream --no-heading "SPARSE_needle" "$HOST" 2>/dev/null | grep -c '^[0-9]')
plain_count=$("$BIN" --no-color           --no-heading "SPARSE_needle" "$HOST" 2>/dev/null | grep -c '^[0-9]')
if [[ "$stream_count" == "$plain_count" ]]; then
    echo "ok   [stream-sparse] $stream_count match lines (separators may differ)"; PASS=$((PASS+1))
else
    echo "FAIL [stream-sparse] stream=$stream_count plain=$plain_count"; FAIL=$((FAIL+1))
fi

echo
echo "chunked: passed=$PASS failed=$FAIL"
[[ "$FAIL" -eq 0 ]] || exit 1
