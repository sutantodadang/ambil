/*
 * ls.h — directory listing subcommand.
 *
 * Lists files and directories with three display modes (paths, long, tree),
 * optional recursion, and three output formats (text, compact, json).
 * Reuses the walker for traversal and platform_stat for metadata.
 */
#ifndef AMBIL_LS_H
#define AMBIL_LS_H

#include "ambil.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
	DISP_PATHS = 0,
	DISP_LONG,
	DISP_TREE
} ls_display_t;

typedef struct {
	common_opts_t common;
	ls_display_t  display;
	int           recursive;
	int           max_depth;    /* -1 = unlimited */
	const char  **paths;
	size_t        n_paths;
	size_t        cap_paths;
} ls_opts_t;

void ls_opts_free(ls_opts_t *o);
int  run_ls(int argc, char **argv);

#endif /* AMBIL_LS_H */
