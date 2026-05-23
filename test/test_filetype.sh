#!/usr/bin/env bash
# test/test_filetype.sh — sanity-check `ambil --cmd file` content sniffer.

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

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Synthesise minimal samples for each format we want to detect.
printf '#include <stdio.h>\nint main(void) { return 0; }\n' > "$TMP/hello.c"
printf '#include <iostream>\nusing namespace std;\nint main() { cout << 1; }\n' > "$TMP/hello.cpp"
printf '#!/usr/bin/env python3\nimport sys\ndef main(): pass\n' > "$TMP/hello.py"
printf 'package main\nfunc main() {}\nimport "fmt"\n' > "$TMP/hello.go"
printf 'fn main() { println!("hi"); }\nuse std::io;\n' > "$TMP/hello.rs"
printf '{"name": "ambil", "version": "1.0"}\n' > "$TMP/config.json"
printf -- '---\nname: ambil\nversion: 1.0\n' > "$TMP/config.yaml"
printf '# README\n\nSome **markdown** text.\n' > "$TMP/README.md"
printf '<!DOCTYPE html>\n<html><body>hi</body></html>\n' > "$TMP/page.html"
printf '\x89PNG\r\n\x1a\n....' > "$TMP/img.png"
printf '\x7fELF\x02\x01\x01....' > "$TMP/binary.elf"
printf 'MZ\x90\x00....' > "$TMP/program.exe"
printf 'CC = gcc\nall:\n\t$(CC) -o foo foo.c\n' > "$TMP/Makefile"

PASS=0; FAIL=0
check() {
    local file="$1" expect="$2"
    local host got
    host="$(to_host_path "$TMP/$file")"
    got=$("$BIN" --cmd file "$host" 2>/dev/null | awk -F': ' '{print $NF}' | tr -d '\r')
    if [[ "$got" == "$expect" ]]; then
        echo "ok   [$file] -> $got"; PASS=$((PASS+1))
    else
        echo "FAIL [$file] expected=$expect got=$got"; FAIL=$((FAIL+1))
    fi
}

check "hello.c"    "c"
check "hello.cpp"  "cpp"
check "hello.py"   "py"
check "hello.go"   "go"
check "hello.rs"   "rust"
check "config.json" "json"
check "config.yaml" "yaml"
check "README.md"  "md"
check "page.html"  "html"
check "img.png"    "png"
check "binary.elf" "elf"
check "program.exe" "pe"
check "Makefile"   "makefile"

echo
echo "filetype: passed=$PASS failed=$FAIL"
[[ "$FAIL" -eq 0 ]] || exit 1
