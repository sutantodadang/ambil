/*
 * filetype.c — content sniffer.
 *
 * Priority order:
 *   1. Binary magic bytes (PNG, JPEG, ELF, PE, ZIP, ...) — cheap, unambiguous.
 *   2. Shebang lines (#!/bin/sh, #!/usr/bin/env python, ...).
 *   3. Content signatures (#include, package main, import, def, fn, ...).
 *   4. Extension tiebreaker (when path_hint is provided).
 *   5. Text/Binary heuristic via NUL-byte check.
 */
#include "filetype.h"
#include "util.h"

#include <stddef.h>
#include <string.h>

#define HEAD_BYTES 4096

static int starts_with(const char *data, size_t size, const char *needle) {
    size_t n = strlen(needle);
    if (size < n) return 0;
    return memcmp(data, needle, n) == 0;
}

/* Search for `needle` in the first `limit` bytes. */
static int contains(const char *data, size_t size, const char *needle, size_t limit) {
    size_t scan = size < limit ? size : limit;
    size_t n = strlen(needle);
    if (scan < n) return 0;
    for (size_t i = 0; i + n <= scan; i++) {
        if (data[i] == needle[0] && memcmp(data + i, needle, n) == 0) return 1;
    }
    return 0;
}

static lf_filetype_t check_magic(const char *d, size_t n) {
    /* PNG */
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return LF_FT_PNG;
    /* JPEG */
    if (n >= 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF) return LF_FT_JPEG;
    /* GIF */
    if (starts_with(d, n, "GIF87a") || starts_with(d, n, "GIF89a")) return LF_FT_GIF;
    /* WEBP (RIFF....WEBP) */
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) return LF_FT_WEBP;
    /* PDF */
    if (starts_with(d, n, "%PDF-")) return LF_FT_PDF;
    /* ZIP / JAR / DOCX */
    if (n >= 4 && d[0] == 'P' && d[1] == 'K' &&
        (d[2] == 3 || d[2] == 5 || d[2] == 7) &&
        (d[3] == 4 || d[3] == 6 || d[3] == 8)) return LF_FT_ZIP;
    /* GZIP */
    if (n >= 2 && (unsigned char)d[0] == 0x1F && (unsigned char)d[1] == 0x8B) return LF_FT_GZIP;
    /* ELF */
    if (n >= 4 && memcmp(d, "\x7f""ELF", 4) == 0) return LF_FT_ELF;
    /* PE / DOS header MZ */
    if (n >= 2 && d[0] == 'M' && d[1] == 'Z') return LF_FT_PE_EXE;
    /* Mach-O (32/64, BE/LE) */
    if (n >= 4) {
        unsigned long mag = ((unsigned char)d[0] << 24) | ((unsigned char)d[1] << 16) |
                            ((unsigned char)d[2] <<  8) |  (unsigned char)d[3];
        if (mag == 0xFEEDFACEUL || mag == 0xFEEDFACFUL ||
            mag == 0xCEFAEDFEUL || mag == 0xCFFAEDFEUL) return LF_FT_MACH_O;
    }
    /* WASM */
    if (n >= 4 && memcmp(d, "\0asm", 4) == 0) return LF_FT_WASM;
    /* TAR (ustar header at offset 257) */
    if (n >= 263 && memcmp(d + 257, "ustar", 5) == 0) return LF_FT_TAR;
    return LF_FT_UNKNOWN;
}

static lf_filetype_t check_shebang(const char *d, size_t n) {
    if (n < 3 || d[0] != '#' || d[1] != '!') return LF_FT_UNKNOWN;
    size_t line_end = 0;
    while (line_end < n && line_end < 256 && d[line_end] != '\n') line_end++;
    /* Look at the shebang substring [2, line_end). */
    const char *line = d + 2;
    size_t llen = line_end - 2;
    if (contains(line, llen, "python", llen)) return LF_FT_PY;
    if (contains(line, llen, "node",   llen)) return LF_FT_JS;
    if (contains(line, llen, "ruby",   llen)) return LF_FT_RUBY;
    if (contains(line, llen, "php",    llen)) return LF_FT_PHP;
    if (contains(line, llen, "bash",   llen) ||
        contains(line, llen, "/sh",    llen) ||
        contains(line, llen, "zsh",    llen) ||
        contains(line, llen, "fish",   llen) ||
        contains(line, llen, "/dash",  llen) ||
        contains(line, llen, "/ksh",   llen)) return LF_FT_SHELL;
    if (contains(line, llen, "make",   llen)) return LF_FT_MAKEFILE;
    return LF_FT_UNKNOWN;
}

/* Content-signature heuristic on first 4 KB of source code. */
static lf_filetype_t check_content(const char *d, size_t n) {
    size_t scan = n < HEAD_BYTES ? n : HEAD_BYTES;

    /* JSON: must start with { or [ after whitespace, contain ':'. */
    {
        size_t i = 0;
        while (i < scan && (d[i] == ' ' || d[i] == '\t' || d[i] == '\n' || d[i] == '\r')) i++;
        if (i < scan && (d[i] == '{' || d[i] == '[')) {
            if (contains(d + i, scan - i, "\":", scan - i)) return LF_FT_JSON;
        }
    }
    /* YAML: leading `---\n` or `key: value` pattern. */
    if (starts_with(d, n, "---\n") || starts_with(d, n, "---\r\n")) return LF_FT_YAML;
    /* HTML / XML. */
    if (starts_with(d, n, "<!DOCTYPE html") || starts_with(d, n, "<!doctype html") ||
        starts_with(d, n, "<html") || starts_with(d, n, "<HTML")) return LF_FT_HTML;
    if (starts_with(d, n, "<?xml")) return LF_FT_XML;

    /* Code languages — order matters: more specific patterns first. */
    if (contains(d, scan, "fn main()", scan) || contains(d, scan, "use std::", scan)) return LF_FT_RUST;
    if (contains(d, scan, "package main", scan) ||
        contains(d, scan, "func main()", scan) ||
        contains(d, scan, "import \"", scan)) return LF_FT_GO;
    if (contains(d, scan, "#include <", scan) || contains(d, scan, "#include \"", scan)) {
        /* Distinguish C vs C++ heuristically. */
        if (contains(d, scan, "std::", scan) ||
            contains(d, scan, "template<", scan) ||
            contains(d, scan, "namespace ", scan)) return LF_FT_CPP;
        return LF_FT_C;
    }
    if (contains(d, scan, "public class ",  scan) ||
        contains(d, scan, "package java",   scan)) return LF_FT_JAVA;
    if (contains(d, scan, "interface ",     scan) && contains(d, scan, ": ", scan)) return LF_FT_TS;
    if (contains(d, scan, "import {",       scan) ||
        contains(d, scan, "export const ",  scan) ||
        contains(d, scan, "export default", scan) ||
        contains(d, scan, "const ",         scan)) return LF_FT_JS;
    /* Python signatures: require leading newline/start to avoid `ifdef` /
     * `_imports` false matches. */
    if (contains(d, scan, "\ndef ",         scan) ||
        contains(d, scan, "\nimport ",      scan) ||
        contains(d, scan, "\nfrom ",        scan) ||
        starts_with(d, n, "def ")           ||
        starts_with(d, n, "import ")        ||
        starts_with(d, n, "from ")) return LF_FT_PY;
    if (contains(d, scan, "require ",       scan) || contains(d, scan, "module ", scan)) return LF_FT_RUBY;
    if (contains(d, scan, "<?php",          scan)) return LF_FT_PHP;
    return LF_FT_UNKNOWN;
}

static int looks_like_binary(const char *d, size_t n) {
    size_t scan = n < HEAD_BYTES ? n : HEAD_BYTES;
    for (size_t i = 0; i < scan; i++) {
        if (d[i] == 0) return 1;
    }
    return 0;
}

/* Extension/basename tiebreaker. Maps file extension or known basename to type. */
static lf_filetype_t ext_hint(const char *path) {
    if (!path) return LF_FT_UNKNOWN;
    /* Known whole-name files. */
    const char *base = lf_path_basename(path);
    if (base) {
        if (strcmp(base, "Makefile")    == 0 ||
            strcmp(base, "makefile")    == 0 ||
            strcmp(base, "GNUmakefile") == 0) return LF_FT_MAKEFILE;
        if (strcmp(base, "Dockerfile")  == 0) return LF_FT_SHELL; /* close enough */
    }
    if (lf_path_has_ext_ci(path, ".md"))   return LF_FT_MD;
    if (lf_path_has_ext_ci(path, ".csv"))  return LF_FT_CSV;
    if (lf_path_has_ext_ci(path, ".toml")) return LF_FT_TOML;
    if (lf_path_has_ext_ci(path, ".yaml") || lf_path_has_ext_ci(path, ".yml")) return LF_FT_YAML;
    if (lf_path_has_ext_ci(path, ".json")) return LF_FT_JSON;
    if (lf_path_has_ext_ci(path, ".xml"))  return LF_FT_XML;
    if (lf_path_has_ext_ci(path, ".html") || lf_path_has_ext_ci(path, ".htm")) return LF_FT_HTML;
    if (lf_path_has_ext_ci(path, ".c") || lf_path_has_ext_ci(path, ".h")) return LF_FT_C;
    if (lf_path_has_ext_ci(path, ".cpp") || lf_path_has_ext_ci(path, ".cc") ||
        lf_path_has_ext_ci(path, ".cxx") || lf_path_has_ext_ci(path, ".hpp")) return LF_FT_CPP;
    if (lf_path_has_ext_ci(path, ".rs"))   return LF_FT_RUST;
    if (lf_path_has_ext_ci(path, ".go"))   return LF_FT_GO;
    if (lf_path_has_ext_ci(path, ".py") || lf_path_has_ext_ci(path, ".pyi")) return LF_FT_PY;
    if (lf_path_has_ext_ci(path, ".ts") || lf_path_has_ext_ci(path, ".tsx")) return LF_FT_TS;
    if (lf_path_has_ext_ci(path, ".js") || lf_path_has_ext_ci(path, ".mjs") ||
        lf_path_has_ext_ci(path, ".jsx") || lf_path_has_ext_ci(path, ".cjs")) return LF_FT_JS;
    if (lf_path_has_ext_ci(path, ".java")) return LF_FT_JAVA;
    if (lf_path_has_ext_ci(path, ".rb"))   return LF_FT_RUBY;
    if (lf_path_has_ext_ci(path, ".php"))  return LF_FT_PHP;
    if (lf_path_has_ext_ci(path, ".sh") || lf_path_has_ext_ci(path, ".bash") ||
        lf_path_has_ext_ci(path, ".zsh")) return LF_FT_SHELL;
    return LF_FT_UNKNOWN;
}

lf_filetype_t lf_filetype_detect(const char *data, size_t size, const char *path_hint) {
    if (size == 0) {
        lf_filetype_t e = ext_hint(path_hint);
        return e != LF_FT_UNKNOWN ? e : LF_FT_TEXT;
    }

    lf_filetype_t ft = check_magic(data, size);
    if (ft != LF_FT_UNKNOWN) return ft;

    ft = check_shebang(data, size);
    if (ft != LF_FT_UNKNOWN) return ft;

    /* Strong-signal extensions (markdown, structured data, known filenames)
     * win over content sniffing — a README.md contains C-looking code blocks
     * but is still markdown. */
    ft = ext_hint(path_hint);
    if (ft != LF_FT_UNKNOWN) return ft;

    ft = check_content(data, size);
    if (ft != LF_FT_UNKNOWN) return ft;

    return looks_like_binary(data, size) ? LF_FT_BINARY : LF_FT_TEXT;
}

const char *lf_filetype_name(lf_filetype_t ft) {
    switch (ft) {
        case LF_FT_C:        return "c";
        case LF_FT_CPP:      return "cpp";
        case LF_FT_RUST:     return "rust";
        case LF_FT_GO:       return "go";
        case LF_FT_PY:       return "py";
        case LF_FT_JS:       return "js";
        case LF_FT_TS:       return "ts";
        case LF_FT_JAVA:     return "java";
        case LF_FT_RUBY:     return "ruby";
        case LF_FT_PHP:      return "php";
        case LF_FT_SHELL:    return "sh";
        case LF_FT_MAKEFILE: return "makefile";
        case LF_FT_JSON:     return "json";
        case LF_FT_YAML:     return "yaml";
        case LF_FT_TOML:     return "toml";
        case LF_FT_XML:      return "xml";
        case LF_FT_HTML:     return "html";
        case LF_FT_MD:       return "md";
        case LF_FT_CSV:      return "csv";
        case LF_FT_PNG:      return "png";
        case LF_FT_JPEG:     return "jpeg";
        case LF_FT_GIF:      return "gif";
        case LF_FT_WEBP:     return "webp";
        case LF_FT_PDF:      return "pdf";
        case LF_FT_ZIP:      return "zip";
        case LF_FT_GZIP:     return "gzip";
        case LF_FT_TAR:      return "tar";
        case LF_FT_ELF:      return "elf";
        case LF_FT_PE_EXE:   return "pe";
        case LF_FT_MACH_O:   return "mach-o";
        case LF_FT_WASM:     return "wasm";
        case LF_FT_TEXT:     return "text";
        case LF_FT_BINARY:   return "binary";
        case LF_FT_UNKNOWN:
        default:             return "unknown";
    }
}
