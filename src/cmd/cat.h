/*
 * cat.h — file reading subcommand.
 *
 * Reads and displays file contents with optional line/byte ranges.
 * Supports three output formats: text (classic cat), compact (token-
 * efficient with file headings), and json (NDJSON with per-line events).
 */
#ifndef AMBIL_CAT_H
#define AMBIL_CAT_H

#include "ambil.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	common_opts_t common;
	uint64_t      head_n;      /* --head N, 0 = all */
	uint64_t      tail_n;      /* --tail N, 0 = off */
	uint64_t      line_start;  /* --lines N:M, 0 = off (1-indexed) */
	uint64_t      line_end;
	uint64_t      byte_start;  /* --bytes N:M, 0 = off (1-indexed) */
	uint64_t      byte_end;
	const char  **paths;
	size_t        n_paths;
	size_t        cap_paths;
} cat_opts_t;

void cat_opts_free(cat_opts_t *o);
int  run_cat(int argc, char **argv);

#endif /* AMBIL_CAT_H */
