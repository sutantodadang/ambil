#!/usr/bin/env bash
# test/test_simd_parity.sh — verify SIMD search returns identical results to
# a scalar-equivalent grep run, across pattern lengths and case modes.
#
# Strategy: run ambil (SIMD when CPU supports it) against grep -F (scalar
# baseline) on the same input. Match counts MUST agree exactly.

set -euo pipefail

BIN="${BIN:-build/ambil}"
if [[ ! -x "$BIN" && -x "${BIN}.exe" ]]; then BIN="${BIN}.exe"; fi
if [[ ! -x "$BIN" ]]; then
    echo "missing $BIN — run 'make' first" >&2
    exit 2
fi

to_host_path() {
    local p="$1"
    if [[ "$BIN" == *.exe ]]; then
        command -v cygpath >/dev/null 2>&1 && { cygpath -w "$p"; return; }
        command -v wslpath >/dev/null 2>&1 && { wslpath -w "$p"; return; }
    fi
    printf '%s\n' "$p"
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Synthetic corpus: 64 KB of mixed-case ASCII with embedded needles.
awk 'BEGIN {
    srand(1);
    chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789  ";
    for (l = 1; l <= 800; l++) {
        line = "";
        n = 60 + int(rand() * 60);
        for (k = 0; k < n; k++) line = line substr(chars, 1 + int(rand()*length(chars)), 1);
        if (l % 13 == 0) line = line " needle_HERE_xyz";
        if (l % 17 == 0) line = line " ALPHA_beta_gamma";
        if (l %  7 == 0) line = line " abc";   /* short pattern target */
        if (l % 23 == 0) line = line " The Quick Brown Fox Jumps";  /* long ci */
        print line;
    }
}' > "$TMP/corpus.txt"

HOST="$(to_host_path "$TMP/corpus.txt")"
PASS=0; FAIL=0

check() {
    local label="$1" pattern="$2" flags="$3"
    local ambil_n grep_n
    # ambil match count via -c (sums per-file counts)
    ambil_n=$("$BIN" $flags -c "$pattern" "$HOST" 2>/dev/null | awk -F: '{s+=$NF} END{print s+0}')
    # grep -F as scalar baseline; -i if case-insensitive
    if [[ "$flags" == *"-i"* ]]; then
        grep_n=$(grep -cFi "$pattern" "$TMP/corpus.txt" || true)
    else
        grep_n=$(grep -cF  "$pattern" "$TMP/corpus.txt" || true)
    fi
    if [[ "$ambil_n" == "$grep_n" ]]; then
        echo "ok   [$label] $ambil_n matches"; PASS=$((PASS+1))
    else
        echo "FAIL [$label] ambil=$ambil_n grep=$grep_n"; FAIL=$((FAIL+1))
    fi
}

echo "SIMD path: $($BIN --simd-info)"

# Case-sensitive paths, varied pattern lengths
check "cs-short-3"    "abc"                  "--no-color"
check "cs-mid-8"      "needle_H"             "--no-color"
check "cs-mid-16"     "needle_HERE_xyz"      "--no-color"
check "cs-long-25"    "ALPHA_beta_gamma"     "--no-color"

# Case-insensitive paths — same patterns via -i
check "ci-short-3"    "ABC"                  "--no-color -i"
check "ci-mid-7"      "NEEDLE_"              "--no-color -i"
check "ci-mid-15"     "needle_here_xyz"      "--no-color -i"
check "ci-long-25"    "alpha_BETA_gamma"     "--no-color -i"
check "ci-mixed"      "the quick brown fox"  "--no-color -i"

echo
echo "parity: passed=$PASS failed=$FAIL"
[[ "$FAIL" -eq 0 ]] || exit 1
