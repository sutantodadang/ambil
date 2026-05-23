#ifndef AMBIL_FIND_H
#define AMBIL_FIND_H

#include "ambil.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
	FIND_OP_NONE = 0,
	FIND_OP_LT,    /* -N : less than */
	FIND_OP_GT,    /* +N : greater than */
	FIND_OP_EQ     /* =N or bare N : equal */
} find_op_t;

typedef struct {
	common_opts_t common;
	const char *name_glob;
	char        type_filter;  /* 0=none, 'f'=file, 'd'=dir */
	find_op_t   size_op;
	uint64_t    size_val;
	find_op_t   mtime_op;
	int64_t     mtime_days;
	const char **paths;
	size_t      n_paths;
	size_t      cap_paths;
} find_opts_t;

void find_opts_free(find_opts_t *o);
int  run_find(int argc, char **argv);

#endif /* AMBIL_FIND_H */
