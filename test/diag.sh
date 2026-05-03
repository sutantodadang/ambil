#!/usr/bin/env bash
# Sanity-check: does grep actually scan the bench file, or short-circuit?
# Writes to a Windows-native temp dir so ambil.exe doesn't pay 9P bridge cost.
set -u

host_to_posix() {
    if command -v wslpath >/dev/null 2>&1; then wslpath -u "$1"; return; fi
    if command -v cygpath  >/dev/null 2>&1; then cygpath  -u "$1"; return; fi
    printf '%s\n' "$1"
}
posix_to_host() {
    if command -v wslpath >/dev/null 2>&1; then wslpath -w "$1"; return; fi
    if command -v cygpath  >/dev/null 2>&1; then cygpath  -w "$1"; return; fi
    printf '%s\n' "$1"
}

WIN_TMP_HOST="$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')"
if [[ -n "$WIN_TMP_HOST" ]]; then
    WIN_TMP_POSIX="$(host_to_posix "$WIN_TMP_HOST")"
else
    WIN_TMP_POSIX="${TMPDIR:-/tmp}"
fi
TMP="${WIN_TMP_POSIX}/ambil-diag-$$.log"
trap 'rm -f "$TMP"' EXIT

echo "Generating ~512 MB synthetic log -> $TMP"
awk 'BEGIN {
    levels[0]="info"; levels[1]="warn"; levels[2]="error"; levels[3]="debug";
    target=512*1024*1024; bytes=0; i=0;
    srand(42);
    while (bytes<target) {
        i++;
        lvl=levels[int(rand()*4)];
        line=sprintf("{\"ts\":\"2026-05-01T00:00:00Z\",\"level\":\"%s\",\"msg\":\"req\",\"id\":%d}\n",lvl,i);
        printf "%s",line;
        bytes+=length(line);
    }
}' > "$TMP"

ls -lh "$TMP"
echo
echo "--- head ---"
head -c 200 "$TMP"
echo
echo
echo "--- grep with match ---"
time grep -c "error" "$TMP"
echo
echo "--- grep with no match ---"
time grep -c "ZZZNOMATCH_ZZZ" "$TMP"
echo
echo "--- wc -l (full scan baseline) ---"
time wc -l "$TMP"

# Test ambil in same shape
HOST_TMP="$(posix_to_host "$TMP")"
echo
echo "ambil input path (Windows view): $HOST_TMP"
echo
echo "--- ambil 'error' (no output) ---"
time build/ambil.exe --no-color "error" "$HOST_TMP" > /dev/null
echo
echo "--- ambil 'error' -j 1 ---"
time build/ambil.exe --no-color -j 1 "error" "$HOST_TMP" > /dev/null
echo
echo "--- ambil 'ZZZNOMATCH_ZZZ' (no-match, pure scan) ---"
time build/ambil.exe --no-color "ZZZNOMATCH_ZZZ" "$HOST_TMP" > /dev/null
echo
echo "--- ambil --count level ---"
time build/ambil.exe --count level "$HOST_TMP" > /dev/null
