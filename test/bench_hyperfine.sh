#!/usr/bin/env bash
# test/bench_hyperfine.sh — statistically-valid benchmark via `hyperfine`.
#
# Compares ambil vs grep (and ripgrep if present) on three workloads:
#   1. 1 GB synthetic JSON log, fixed-string scan.
#   2. Same log, case-insensitive scan.
#   3. 50 MB recursive source tree, fixed-string scan.
#
# Output:
#   stdout — human-readable hyperfine summary.
#   bench-results.json — machine-readable for trend tracking.
#
# Knobs:
#   SIZE_MB=2048   target log size (default 1024)
#   RUNS=5         hyperfine --runs (default 5)
#   JSON=path      output file (default bench-results.json)
#   KEEP=1         preserve generated corpora
#
# Requires: hyperfine (https://github.com/sharkdp/hyperfine). Will exit 0
# with a hint if hyperfine is not installed.

set -euo pipefail

BIN="${BIN:-build/ambil}"
if [[ ! -x "$BIN" && -x "${BIN}.exe" ]]; then BIN="${BIN}.exe"; fi
if [[ ! -x "$BIN" ]]; then echo "missing $BIN — run 'make' first" >&2; exit 2; fi

if ! command -v hyperfine >/dev/null 2>&1; then
    echo "hyperfine not found — install from https://github.com/sharkdp/hyperfine"
    echo "  (cargo install hyperfine)  OR  (brew install hyperfine)"
    echo "Falling back to test/bench.sh for a non-statistical run."
    exec bash "$(dirname "$0")/bench.sh"
fi

SIZE_MB="${SIZE_MB:-1024}"
RUNS="${RUNS:-5}"
JSON="${JSON:-bench-results.json}"
KEEP="${KEEP:-0}"

to_host() {
    if [[ "$BIN" == *.exe ]]; then
        command -v cygpath >/dev/null 2>&1 && { cygpath -w "$1"; return; }
        command -v wslpath >/dev/null 2>&1 && { wslpath -w "$1"; return; }
    fi
    printf '%s\n' "$1"
}

# Pick a fast tmp filesystem; on Windows .exe under WSL use Windows-native temp.
if [[ "$BIN" == *.exe ]]; then
    WIN_TMP="$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r' || true)"
    TMP_DIR="$(wslpath -u "$WIN_TMP" 2>/dev/null || cygpath -u "$WIN_TMP" 2>/dev/null || echo /tmp)"
else
    TMP_DIR="${TMPDIR:-/tmp}"
fi

LOG="$TMP_DIR/ambil-bench-$$.log"
TREE="$TMP_DIR/ambil-bench-tree-$$"
trap 'if [[ "$KEEP" -ne 1 ]]; then rm -f "$LOG"; rm -rf "$TREE"; fi' EXIT

echo "[gen] $SIZE_MB MB synthetic JSON log -> $LOG"
target=$((SIZE_MB * 1024 * 1024))
awk -v target="$target" 'BEGIN {
    srand(42); levels[0]="info"; levels[1]="warn"; levels[2]="error"; levels[3]="debug";
    paths[0]="/health"; paths[1]="/login"; paths[2]="/api/users"; paths[3]="/api/orders";
    bytes=0; i=0;
    while (bytes < target) { i++;
        sec = i % 3600;
        ts = sprintf("2026-05-01T%02d:%02d:%02dZ", int(sec/3600), int((sec%3600)/60), sec%60);
        lvl = levels[int(rand()*4)]; path = paths[int(rand()*4)];
        status = (lvl=="error") ? 500 : 200;
        line = sprintf("{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"req\",\"path\":\"%s\",\"status\":%d,\"id\":%d}\n",
                       ts, lvl, path, status, i);
        printf "%s", line; bytes += length(line);
    }
}' > "$LOG"

echo "[gen] 50 MB source tree -> $TREE"
mkdir -p "$TREE"
awk -v dir="$TREE" -v files=500 -v lines_per=2000 'BEGIN {
    for (f = 1; f <= files; f++) {
        path = dir "/file_" f ".c";
        for (l = 1; l <= lines_per; l++) {
            if (l == 17 && f % 7 == 0) print "void rare_needle(void) { return; }" > path;
            else if (l % 250 == 0) printf "    /* foo_bar_baz handler %d */\n", l > path;
            else printf "    int x_%d = %d * 2 + 1;\n", l, l > path;
        }
        close(path);
    }
}'

HOST_LOG="$(to_host "$LOG")"
HOST_TREE="$(to_host "$TREE")"

EXTRA_TOOLS=()
command -v rg >/dev/null 2>&1 && EXTRA_TOOLS+=("rg --no-heading --color=never")

run_bench() {
    local label="$1"; shift
    local cmds=("$@")
    echo
    echo "=== $label ==="
    hyperfine --warmup 1 --runs "$RUNS" \
              --export-json "$JSON.tmp" \
              "${cmds[@]}"
    # Append this scenario's results to the rolling JSON.
    if [[ -f "$JSON" ]]; then
        # naive append: combine arrays
        python3 - "$JSON" "$JSON.tmp" <<'PY' 2>/dev/null || cat "$JSON.tmp" > "$JSON"
import json, sys
with open(sys.argv[1]) as f: a = json.load(f)
with open(sys.argv[2]) as f: b = json.load(f)
a.setdefault("results", []).extend(b.get("results", []))
with open(sys.argv[1], "w") as f: json.dump(a, f, indent=2)
PY
    else
        cp "$JSON.tmp" "$JSON"
    fi
    rm -f "$JSON.tmp"
}

ambil_cs="$BIN --no-color error $HOST_LOG"
grep_cs="grep error $LOG"
run_bench "fixed-string scan (1 GB log)" "$ambil_cs" "$grep_cs" \
    $(for t in "${EXTRA_TOOLS[@]}"; do printf '%s\n' "$t error $LOG"; done)

ambil_ci="$BIN --no-color -i ERROR $HOST_LOG"
grep_ci="grep -i ERROR $LOG"
run_bench "case-insensitive scan (1 GB log)" "$ambil_ci" "$grep_ci" \
    $(for t in "${EXTRA_TOOLS[@]}"; do printf '%s\n' "$t -i ERROR $LOG"; done)

ambil_dir="$BIN --no-color rare_needle $HOST_TREE"
grep_dir="grep -rE rare_needle $TREE"
run_bench "recursive source tree (50 MB)" "$ambil_dir" "$grep_dir" \
    $(for t in "${EXTRA_TOOLS[@]}"; do printf '%s\n' "$t rare_needle $TREE"; done)

echo
echo "[done] results -> $JSON"
