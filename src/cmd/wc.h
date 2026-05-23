#ifndef AMBIL_WC_H
#define AMBIL_WC_H

#include "ambil.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	common_opts_t common;
	int           show_lines;
	int           show_words;
	int           show_chars;
	int           show_bytes;
	const char  **paths;
	size_t        n_paths;
	size_t        cap_paths;
} wc_opts_t;

void wc_opts_free(wc_opts_t *o);
int  run_wc(int argc, char **argv);

#endif
