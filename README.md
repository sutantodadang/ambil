# ambil

A **token-efficient grep replacement for AI coding agents**, written in C11.

`ambil` is a fast recursive-grep tool — like `grep -r` or `ripgrep` — with one
extra obsession: **the output is sized for an LLM context window**, not a human
terminal. Compact-grouped headings, NDJSON, and counting modes make it cheap to
pipe results straight into an agent loop.

The original log-aggregation tool (timestamp ranges, JSON field filters,
`--count-field`) is preserved as a bonus. See [Log analysis](#log-analysis-appendix).

---

## Why a different grep?

When an AI coding agent runs `grep -rn foo .` and pipes 12,000 lines back into a
prompt, you pay for every repeated path prefix. Default `grep` output looks
like:

```
src/foo/bar.c:142:    handle_foo_thing(x);
src/foo/bar.c:143:    handle_foo_thing(y);
src/foo/bar.c:201:    int foo = 1;
src/foo/baz.c:7:#include "foo.h"
...
```

The path is repeated on every line. For thousands of matches, that's thousands
of tokens spent on a string the model already saw. `ambil --compact` groups
matches under a single path heading:

```
src/foo/bar.c
142:    handle_foo_thing(x);
143:    handle_foo_thing(y);
201:    int foo = 1;
src/foo/baz.c
7:#include "foo.h"
```

Same information, ~30–50% fewer tokens on typical real-world greps. For
agentic loops where every search round-trip costs prompt tokens, this adds up
fast.

`ambil` also speaks first-class **NDJSON** (`--json`) for tool-calling agents,
**file-only** mode (`-l`) and **count-only** mode (`-c`) for cheap surveys, and
honors `.gitignore` / `node_modules` / hidden files by default just like
`ripgrep`.

---

## Quickstart

```bash
make                                  # builds build/ambil
build/ambil PATTERN [PATH...]       # recursive grep
build/ambil --compact foo src/      # token-efficient grouped output
build/ambil --json    foo src/      # NDJSON, one event per line
build/ambil -c        foo src/      # per-file match counts
build/ambil -l        foo src/      # files-with-matches only
build/ambil -t c -i   TODO          # only .c files, case-insensitive
build/ambil -e foo -e bar src/      # multiple patterns
build/ambil -A 2 -B 1 panic src/    # context lines
```

On Windows with MinGW64:

```bat
mingw32-make
build\ambil.exe --compact foo src\
```

---

## Installation

### Quick install (prebuilt binaries)

**Linux / macOS / WSL:**

```bash
curl -fsSL https://raw.githubusercontent.com/sutantodadang/ambil/main/install.sh | sh
# pin a version:
curl -fsSL https://raw.githubusercontent.com/sutantodadang/ambil/main/install.sh | sh -s -- --version v0.2.0
# install to a custom dir:
AMBIL_INSTALL_DIR=$HOME/.local/bin sh install.sh
```

**Windows (PowerShell 5.1+ or PowerShell 7):**

```powershell
iwr -useb https://raw.githubusercontent.com/sutantodadang/ambil/main/install.ps1 | iex
# pin a version:
$env:AMBIL_VERSION='v0.2.0'; iwr -useb https://raw.githubusercontent.com/sutantodadang/ambil/main/install.ps1 | iex
# explicit invocation:
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Version v0.2.0 -Force
```

Both installers detect the host OS/architecture, download the matching
release archive, verify its `SHA256SUMS` entry, and place `ambil` on disk
(default `/usr/local/bin` or `~/.local/bin` on POSIX,
`%LOCALAPPDATA%\Programs\ambil` on Windows). The Windows installer also
appends the install dir to the **user** `PATH`.

Useful overrides (env vars, identical on both scripts):

| Variable             | Default                       | Purpose                              |
|----------------------|-------------------------------|--------------------------------------|
| `AMBIL_REPO`         | `sutantodadang/ambil`         | GitHub `owner/repo` to pull from     |
| `AMBIL_VERSION`      | `latest`                      | Tag (e.g. `v0.2.0`) or `latest`      |
| `AMBIL_INSTALL_DIR`  | OS default (see above)        | Destination directory                |
| `AMBIL_BASE_URL`     | GitHub release URL            | Mirror / private release host        |
| `AMBIL_NO_VERIFY`    | unset                         | Set `1` to skip SHA-256 verification |

Asset naming the installers expect (produced by `make release`):

```
ambil-vX.Y.Z-x86_64-linux.tar.gz
ambil-vX.Y.Z-aarch64-linux.tar.gz
ambil-vX.Y.Z-x86_64-darwin.tar.gz
ambil-vX.Y.Z-aarch64-darwin.tar.gz
ambil-vX.Y.Z-x86_64-windows.zip
ambil-vX.Y.Z-aarch64-windows.zip
SHA256SUMS
```

### From source (Linux / macOS / WSL)

```bash
git clone <this-repo>
cd ambil
make
sudo install -m 0755 build/ambil /usr/local/bin/
```

### From source (Windows / MinGW64)

```bat
mingw32-make
copy build\ambil.exe %USERPROFILE%\bin\
```

Requirements: a C11 compiler (`gcc` ≥ 7, `clang` ≥ 6, or MSVC via mingw),
POSIX threads, and `mmap` (POSIX) or `CreateFileMapping` (Win32, automatic).

To build a portable binary that runs on older CPUs, set `NATIVE=0`:

```bash
make NATIVE=0
```

---

## Output modes

| Flag | Mode | Best for |
|------|------|----------|
| (default) | `text` — ripgrep-style: file heading + `LINE:content`, blank line between files | Humans in a terminal |
| `--compact` | `compact` — file heading once, `LINE:content` lines, **no separators** | **AI agents** (most token-efficient text mode) |
| `--json` | `json` — NDJSON: `begin`, `match`, `context`, `end` events | Structured tool calling |
| `-c` / `--count` | `count` — `path:N` per file with at least one match | Surveys, dashboards |
| `-l` / `--files-with-matches` | `files-with` — just paths | Quick file lists |
| `-o` / `--only-matching` | text mode but only the matched substring per line | Extracting tokens |
| `--no-heading` | per-line `path:line:content` (classic grep) | Unix pipelines |

Color is on for TTY by default; force with `--color`, disable with `--no-color`
or by piping to a non-TTY.

### NDJSON shape

```json
{"type":"begin","path":"src/main.c"}
{"type":"match","path":"src/main.c","line_number":42,"lines":"int main(void) {","submatches":[{"start":4,"end":8,"text":"main"}]}
{"type":"context","path":"src/main.c","line_number":43,"lines":"    return 0;"}
{"type":"end","path":"src/main.c","stats":{"matched_lines":1,"searches":1}}
```

Schema is a flat NDJSON record per event, optimised for token-cheap consumption
by LLM agents. It is **not** drop-in compatible with `ripgrep --json`'s nested
`{type, data:{path:{text}, lines:{text}, submatches:[{match:{text}}]}}` shape;
write a small adapter (jq one-liner) if you need ripgrep's exact format.

---

## Filtering & traversal

| Flag | Effect |
|------|--------|
| `-r` / `-R` (default on dirs) | Recurse into directories |
| `--no-recursive` | Single-level only |
| `--hidden` | Include dotfiles |
| `--no-ignore` | Don't read `.gitignore` / `.ignore` / built-in default ignores |
| `--follow` | Follow symlinks |
| `--max-depth N` | Cap recursion depth |
| `-g GLOB` / `-g '!GLOB'` | Include / exclude globs (`*`, `?`, `**`) |
| `-t TYPE` | File-type alias: `c`, `cpp`, `rust`, `py`, `ts`, `js`, `go`, `md`, `json`, `yaml`, `toml`, `sh` |
| `--text` / `-a` | Don't skip binary files |

Default ignores (always applied unless `--no-ignore`): `.git`, `.hg`, `.svn`,
`node_modules`, `.venv`, `venv`, `__pycache__`, `dist`, `build`, `target`,
`.next`, `.cache`, `.DS_Store`, plus common binary extensions
(`.png .jpg .pdf .zip .o .a .so .dll .exe .class .pyc` etc.).

Binary files are detected by NUL byte or UTF-16 BOM in the first 8 KB and
skipped silently (use `-a` to override).

---

## Search modifiers

| Flag | Effect |
|------|--------|
| `-i` / `--ignore-case` | Case-insensitive |
| `-F` / `--fixed-strings` | Literal substring (default — regex not supported) |
| `-w` / `--word-regexp` | Whole-word matches only |
| `-v` / `--invert-match` | Lines that **don't** match |
| `-e PAT` | Add a pattern; repeatable. With `-e`, all positionals become paths |
| `-A N` / `-B N` / `-C N` | After / before / both context lines |

> Regex: `ambil` uses Boyer-Moore-Horspool for sub-microsecond per-byte
> scanning. PCRE / RE2 is not supported by design — keeps the binary small,
> the dependencies zero, and the behavior obvious. For full regex use
> `ripgrep`.

---

## Performance

Benchmarks on a single laptop (Intel i7, 16 GB, mingw build, WSL2 host fs).
Smaller numbers are better.

### 1 GB synthetic JSON log file (`test/bench.sh`)

| Workload | ambil | grep | speedup |
|----------|--------:|-----:|--------:|
| Search common term `error` (warm) | **2.15 s** | 4.72 s | 2.2× |
| Pure scan, no matches `ZZZNOMATCH` | **0.67 s** | 4.05 s | 6.0× |
| `--count-field level` (group-by) | **0.20 s** | n/a (jq: 8 s+) | — |

### 50 MB recursive C source tree (500 files)

| Workload | ambil | `grep -rE` | speedup |
|----------|--------:|-----------:|--------:|
| Rare term (71 hits) | **0.045 s** | 0.663 s | 14.7× |
| Common term (4000 hits) | **0.032 s** | 0.647 s | 20.2× |
| `--compact` mode (same 4000 hits) | **0.031 s** | n/a | — |
| `-c` per-file count | **0.029 s** | n/a | — |
| `-l` files-with-matches | **0.031 s** | n/a | — |

Run yourself:

```bash
bash test/bench.sh                       # default 1024 MB
SIZE_MB=512 bash test/bench.sh           # smaller, faster
THREADS=8  bash test/bench.sh            # explicit thread count
```

If `ripgrep` is on your `PATH`, the bench script picks it up automatically and
adds an `rg` row to every comparison.

### Where the speed comes from

- **mmap + work-stealing thread pool**: each file is dispatched to a worker
  thread, so directory scans saturate every core.
- **Boyer-Moore-Horspool** sub-microsecond byte scanning per pattern.
- **Per-directory cheap-skip**: default-ignored directories are pruned at
  `readdir()` time — `node_modules` is never even opened.
- **Binary detection in 8 KB**: skip MP4s, PNGs, and `.o` files instantly.
- **Streaming output**: results are written in submission order via a
  per-job done channel; you see the first match before the last file is
  even read.

---

## AI-agent integration recipes

### MCP / function-calling tool

Wrap `ambil --json` in a tool that receives `{pattern, path, type?}` and
streams NDJSON events back to the model. The model sees `begin` / `match` /
`end` boundaries with no token-wasting path repetition.

```bash
ambil --json -t py "def main" src/
```

### Compact context for retrieval

For a "show me all callers of `foo`" agent step:

```bash
ambil --compact -w foo src/ | head -200
```

The grouped output keeps related matches adjacent — the model sees one path
per group and can cite line numbers without re-prompting for context.

### Cheap surveys

Before a large diff, ask the agent to budget:

```bash
ambil -c TODO src/                  # quick TODO census per file
ambil -l 'panic!' src/              # which files panic
```

Both modes return tiny outputs (kilobytes vs megabytes) so an agent can
plan before drilling in.

### Pipelines

```bash
ambil -l --json error src/ | jq -r '.path' | xargs my-tool
ambil --compact -e WARN -e ERROR logs/ > snippet.txt
```

---

## Log analysis appendix

The original `ambil` log-analysis features are preserved. They activate
automatically when you pass any of `--field`, `--count-field`, `--since`,
`--until`, or `--log-json`, and operate on a single log file.

| Flag | Effect |
|------|--------|
| `--field key=value` | Match JSON-log line where `key` equals `value` |
| `--count-field key` | Aggregate count of distinct `key` values, sorted desc |
| `--since TIMESTAMP` / `--from` | Lower bound on `ts` field (RFC3339 or epoch) |
| `--until TIMESTAMP` / `--to` | Upper bound on `ts` field |
| `--log-json` | Force-detect log lines as JSON |

Examples:

```bash
# All ERROR lines today
ambil --field level=error --since 2026-05-01T00:00:00Z app.log

# Distribution of HTTP statuses
ambil --count-field status app.log

# Combined: errors hitting /api/billing
ambil --field path=/api/billing error app.log
```

These modes use the original parallel chunked scanner from v0.1 — see
`src/aggregate.c`, `src/filter.c`, `src/parser.c`.

---

## Architecture

```
                ┌──────────────────────────┐
                │      main.c (CLI)        │
                └────┬─────────────────┬───┘
                     │                 │
          grep mode  │                 │  log-analysis mode
                     ▼                 ▼
            ┌────────────────┐   ┌───────────────┐
            │   walker.c     │   │   filter.c    │
            │  (DFS + ignore)│   │  + parser.c   │
            └────────┬───────┘   │  + aggregate  │
                     │           └───────┬───────┘
                     ▼                   │
            ┌────────────────┐           │
            │ thread_pool.c  │           │
            │  (file jobs)   │           │
            └────────┬───────┘           │
                     ▼                   ▼
            ┌────────────────┐   ┌───────────────┐
            │   grep.c       │   │ thread_pool   │
            │ (per-file scan │   │ (chunk pool)  │
            │  + emit)       │   └───────────────┘
            └────────┬───────┘
                     ▼
            ┌────────────────┐
            │   search.c     │
            │ (Boyer-Moore)  │
            └────────────────┘
```

Modules:

- **ambil.h** — shared `options_t` and core types.
- **util.{h,c}** — allocation, path helpers, options lifecycle.
- **search.{h,c}** — Boyer-Moore-Horspool substring scanner + match-span APIs.
- **binary.{h,c}** — binary-file detection.
- **ignore.{h,c}** — gitignore engine + default ignores + glob/type filters.
- **walker.{h,c}** — cross-platform recursive directory iterator.
- **grep.{h,c}** — per-file scan, context, multi-pattern union, output emission
  for all five modes.
- **thread_pool.{h,c}** — file-job dispatcher (new) + legacy chunk pool
  (log-analysis mode).
- **parser.{h,c}** — log-line tokenizer for the log-analysis appendix.
- **filter.{h,c}** — per-line predicate (timestamp range + JSON field).
- **aggregate.{h,c}** — `--count-field` group-by.
- **main.{h,c}** — argv parsing and mode dispatch.

---

## Tests

```bash
make test                # 27 tests: 12 legacy log-mode + 15 grep-mode
```

Coverage:

- Legacy: text/JSON parsing, time bounds, `--field`, `--count-field`,
  combined filters, thread parity, missing file, empty argv.
- Grep mode: recursive walk, default-ignore, hidden-skip, gitignore,
  binary-skip, `-g` include/exclude, `-t` types, `--json` NDJSON validity,
  `-A/-B/-C` context, `-c`, `-l`, multi-pattern `-e`, line-number
  correctness across multi-MB files, `--hidden` opt-in.

```bash
bash test/diag.sh        # quick smoke test against test/sample.json.log
bash test/bench.sh       # full benchmark vs grep (and rg if installed)
```

---

## Compatibility & limits

- **OS**: Linux, macOS, Windows (MinGW64). FreeBSD untested but should work.
- **Threading**: pthreads everywhere (winpthreads on MinGW).
- **Memory**: files are mmaped read-only — RSS is bounded by OS page cache.
- **No regex**: literal substring + `-i` + `-w` only. Use `ripgrep` for PCRE.
- **Encoding**: UTF-8 input assumed. UTF-16 files are detected as binary
  (use `-a` to scan anyway, but matches will be byte-level).
- **Symlink loops**: detected via `--max-depth`; explicit cycle-detection
  is not implemented (use `--max-depth N` if you have weird trees).

---

## Versioning

- `0.1.x` — log-analysis only (timestamp + JSON field aggregation).
- `0.2.0` — recursive grep, gitignore, multi-format output, parallel file
  dispatcher. Log-analysis preserved.

---

## License

MIT (or whatever the original repo specifies).
