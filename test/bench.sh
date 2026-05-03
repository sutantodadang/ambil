#!/usr/bin/env bash
# test/bench.sh — generate a multi-GB log and benchmark ambil vs grep/jq.
#
# Knobs:
#   SIZE_MB=2048   target file size in MB (default 1024)
#   KEEP=1         keep the generated file (default deletes)
#   THREADS=N      pass -j N to ambil (default auto)
set -euo pipefail

BIN="${BIN:-build/ambil}"
if [[ ! -x "$BIN" && -x "${BIN}.exe" ]]; then
    BIN="${BIN}.exe"
fi
SIZE_MB="${SIZE_MB:-1024}"
THREADS="${THREADS:-}"
KEEP="${KEEP:-0}"

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

host_to_posix() {
    if command -v wslpath >/dev/null 2>&1; then wslpath -u "$1"; return; fi
    if command -v cygpath  >/dev/null 2>&1; then cygpath  -u "$1"; return; fi
    printf '%s\n' "$1"
}

# When benchmarking the Windows .exe under WSL, /tmp lives on WSL ext4 and
# every read from the .exe traverses the 9P bridge (~200 MB/s ceiling).
# Place the bench file on Windows-native temp so we measure the tool, not the bridge.
if [[ "$BIN" == *.exe ]]; then
    WIN_TMP_HOST="$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r' || true)"
    if [[ -n "$WIN_TMP_HOST" ]]; then
        TMP_DIR="$(host_to_posix "$WIN_TMP_HOST")"
    else
        TMP_DIR="${TMPDIR:-/tmp}"
    fi
else
    TMP_DIR="${TMPDIR:-/tmp}"
fi
TMP="${TMP_DIR}/ambil-bench-$$.log"

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN — run 'make' first." >&2
    exit 2
fi

cleanup() {
    if [[ "$KEEP" -ne 1 ]]; then rm -f "$TMP"; fi
}
trap cleanup EXIT

echo "Generating ~${SIZE_MB} MB of synthetic JSON log → $TMP"
# Each line ~110 bytes; loop until we cross the target size.
target=$(( SIZE_MB * 1024 * 1024 ))
awk -v target="$target" '
BEGIN {
    srand(42);
    levels[0]="info"; levels[1]="warn"; levels[2]="error"; levels[3]="debug";
    paths[0]="/health"; paths[1]="/login"; paths[2]="/api/users";
    paths[3]="/api/orders"; paths[4]="/api/billing";
    bytes=0; i=0;
    while (bytes < target) {
        i++;
        sec = i % 3600;
        ts = sprintf("2026-05-01T%02d:%02d:%02dZ", int(sec/3600), int((sec%3600)/60), sec%60);
        lvl = levels[int(rand()*4)];
        path = paths[int(rand()*5)];
        status = (lvl=="error") ? 500 : 200;
        line = sprintf("{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"req\",\"path\":\"%s\",\"status\":%d,\"id\":%d}\n",
                       ts, lvl, path, status, i);
        printf "%s", line;
        bytes += length(line);
    }
}' > "$TMP"

actual=$(stat -c %s "$TMP" 2>/dev/null || stat -f %z "$TMP")
echo "  actual size: $((actual/1024/1024)) MB"
HOST_TMP="$(to_host_path "$TMP")"

J_FLAG=""
[[ -n "$THREADS" ]] && J_FLAG="-j $THREADS"

run() {
    local label="$1"; shift
    local out_file rc start end elapsed bytes lines
    out_file="$(mktemp)"
    start=$(date +%s.%N)
    if "$@" > "$out_file" 2>/dev/null; then rc=0; else rc=$?; fi
    end=$(date +%s.%N)
    elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')
    bytes=$(stat -c %s "$out_file" 2>/dev/null || stat -f %z "$out_file")
    lines=$(wc -l < "$out_file" | tr -d ' ')
    rm -f "$out_file"
    printf "  %-40s %8ss  rc=%d  out=%dB/%dL\n" "$label" "$elapsed" "$rc" "$bytes" "$lines"
}

drop_caches() {
    # Best-effort. Requires root + Linux. Silently skipped otherwise.
    if [[ "$(id -u)" -eq 0 && -w /proc/sys/vm/drop_caches ]]; then
        sync; echo 3 > /proc/sys/vm/drop_caches
    fi
}

echo
echo "=== Cold cache (drop_caches if root) ==="
drop_caches; run "ambil 'error'"        "$BIN" --no-color $J_FLAG "error" "$HOST_TMP"
drop_caches; run "grep 'error'"           grep "error" "$TMP"
if command -v rg >/dev/null 2>&1; then
    drop_caches; run "ripgrep 'error'"    rg --no-heading --color=never "error" "$TMP"
fi

echo
echo "=== Warm cache ==="
run "ambil 'error'"                     "$BIN" --no-color $J_FLAG "error" "$HOST_TMP"
run "grep   'error'"                      grep "error" "$TMP"
if command -v rg >/dev/null 2>&1; then
    run "ripgrep 'error'"                 rg --no-heading --color=never "error" "$TMP"
fi
run "ambil 'ZZZNOMATCH' (pure scan)"    "$BIN" --no-color $J_FLAG "ZZZNOMATCH" "$HOST_TMP"
run "grep   'ZZZNOMATCH' (pure scan)"     grep "ZZZNOMATCH" "$TMP"
if command -v rg >/dev/null 2>&1; then
    run "ripgrep 'ZZZNOMATCH'"            rg --no-heading --color=never "ZZZNOMATCH" "$TMP"
fi
run "ambil --count-field level"         "$BIN" --count-field level $J_FLAG "$HOST_TMP"
if command -v jq >/dev/null 2>&1; then
    run "jq -r .level | sort | uniq -c"  bash -c "jq -r .level '$TMP' | sort | uniq -c"
fi
run "ambil --json --field level=error"  "$BIN" --no-color --log-json --field level=error $J_FLAG "$HOST_TMP"

# ===========================================================================
# Recursive directory bench: synthesize a fake source tree and grep through it.
# ===========================================================================
DIR_TMP="${TMP_DIR}/ambil-bench-dir-$$"
mkdir -p "$DIR_TMP"
cleanup_dir() {
    rm -rf "$DIR_TMP"
}
trap 'cleanup; cleanup_dir' EXIT

echo
echo "Generating synthetic source tree at $DIR_TMP (~50 MB across many .c files)"
# 500 files * ~100 KB each = ~50 MB
awk -v dir="$DIR_TMP" -v files=500 -v lines_per=2000 '
BEGIN {
    for (f = 1; f <= files; f++) {
        path = dir "/file_" f ".c";
        for (l = 1; l <= lines_per; l++) {
            if (l == 17 && f % 7 == 0) {
                print "void rare_needle_function(void) { return; }" > path;
            } else if (l % 250 == 0) {
                printf "    /* common pattern: foo_bar_baz handler %d */\n", l > path;
            } else {
                printf "    int x_%d = %d * 2 + 1; /* filler */\n", l, l > path;
            }
        }
        close(path);
    }
}'
HOST_DIR_TMP="$(to_host_path "$DIR_TMP")"

echo
echo "=== Recursive directory scan (warm) ==="
run "ambil rare_needle"               "$BIN" --no-color $J_FLAG "rare_needle" "$HOST_DIR_TMP"
run "grep -rE rare_needle"              grep -rE "rare_needle" "$DIR_TMP"
if command -v rg >/dev/null 2>&1; then
    run "ripgrep rare_needle"           rg --no-heading --color=never "rare_needle" "$DIR_TMP"
fi
run "ambil foo_bar_baz"               "$BIN" --no-color $J_FLAG "foo_bar_baz" "$HOST_DIR_TMP"
run "grep -rE foo_bar_baz"              grep -rE "foo_bar_baz" "$DIR_TMP"
if command -v rg >/dev/null 2>&1; then
    run "ripgrep foo_bar_baz"           rg --no-heading --color=never "foo_bar_baz" "$DIR_TMP"
fi
run "ambil --compact (token-eff)"     "$BIN" --no-color --compact $J_FLAG "foo_bar_baz" "$HOST_DIR_TMP"
run "ambil -c rare_needle"            "$BIN" --no-color -c $J_FLAG "rare_needle" "$HOST_DIR_TMP"
run "ambil -l rare_needle"            "$BIN" --no-color -l $J_FLAG "rare_needle" "$HOST_DIR_TMP"

echo
echo "Done."
