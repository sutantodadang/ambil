/*
 * filetype.h — content-based file type detection.
 *
 * Inspects the first 4 KB of a file (magic bytes + shebang + content
 * signatures) to classify it into one of a small set of known types.
 * Falls back to LF_FT_TEXT for unrecognised text or LF_FT_BINARY for
 * unrecognised binary data.
 *
 * Cheap: pure byte scans, no allocation. Intended for use by tooling
 * (`--cmd filetype`) and as a hint for future syntax-aware features.
 */
#ifndef AMBIL_FILETYPE_H
#define AMBIL_FILETYPE_H

#include <stddef.h>

typedef enum {
    LF_FT_UNKNOWN = 0,
    /* Source code. */
    LF_FT_C, LF_FT_CPP, LF_FT_RUST, LF_FT_GO,
    LF_FT_PY, LF_FT_JS, LF_FT_TS,
    LF_FT_JAVA, LF_FT_RUBY, LF_FT_PHP,
    LF_FT_SHELL, LF_FT_MAKEFILE,
    /* Data / config. */
    LF_FT_JSON, LF_FT_YAML, LF_FT_TOML, LF_FT_XML, LF_FT_HTML,
    LF_FT_MD, LF_FT_CSV,
    /* Binary signatures. */
    LF_FT_PNG, LF_FT_JPEG, LF_FT_GIF, LF_FT_WEBP, LF_FT_PDF,
    LF_FT_ZIP, LF_FT_GZIP, LF_FT_TAR,
    LF_FT_ELF, LF_FT_PE_EXE, LF_FT_MACH_O, LF_FT_WASM,
    /* Generic buckets. */
    LF_FT_TEXT,
    LF_FT_BINARY,
} lf_filetype_t;

/* Detect type from the first `size` bytes of `data`. path_hint may be NULL;
 * when present, the file extension is consulted as a tiebreaker for ambiguous
 * cases (e.g., a .md file with no clear content signature). */
lf_filetype_t lf_filetype_detect(const char *data, size_t size, const char *path_hint);

/* Stable identifier ("c", "py", "png", ...) — same vocabulary as --type. */
const char *lf_filetype_name(lf_filetype_t ft);

#endif
