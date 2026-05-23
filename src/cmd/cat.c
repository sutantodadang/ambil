#include "cat.h"
#include "file_reader.h"
#include "json_emit.h"
#include "util.h"
#include "help.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static void cat_help(FILE *out) {
	lf_help_usage(out, "ambil", "--cmd cat", "[options] FILE...");
	lf_help_spacer(out);
	lf_help_desc(out, "Read and display file contents.");
	lf_help_spacer(out);
	lf_help_section(out, "Range options (mutually exclusive)");
	lf_help_flag(out, "--head N",     "first N lines (1-indexed)");
	lf_help_flag(out, "--tail N",     "last N lines (ring buffer)");
	lf_help_flag(out, "--lines N:M",  "lines N through M inclusive (1-indexed)");
	lf_help_flag(out, "--bytes N:M",  "byte range N through M (1-indexed)");
	lf_help_spacer(out);
	lf_help_section(out, "Output format");
	lf_help_flag(out, "--json",       "NDJSON structured output");
	lf_help_flag(out, "--compact",    "token-efficient output with file headings");
	lf_help_spacer(out);
	lf_help_flag(out, "-h, --help",   "show this help");
}

void cat_opts_free(cat_opts_t *o) {
	if (!o) return;
	free(o->paths);
	memset(o, 0, sizeof(*o));
}

static void add_path(cat_opts_t *o, const char *p) {
	if (o->n_paths >= o->cap_paths) {
		size_t nc = o->cap_paths ? o->cap_paths * 2 : 8;
		o->paths = (const char **)lf_xrealloc(o->paths, nc * sizeof(char *));
		o->cap_paths = nc;
	}
	o->paths[o->n_paths++] = p;
}

static uint64_t parse_u64(const char *s) {
	char *end;
	unsigned long long v = strtoull(s, &end, 10);
	if (*end != '\0' || s == end) return UINT64_MAX;
	return (uint64_t)v;
}

static int parse_range(const char *s, uint64_t *a, uint64_t *b) {
	const char *colon = strchr(s, ':');
	if (!colon) return -1;

	size_t alen = (size_t)(colon - s);
	char abuf[32];
	if (alen >= sizeof(abuf)) return -1;
	memcpy(abuf, s, alen);
	abuf[alen] = '\0';

	*a = parse_u64(abuf);
	if (*a == UINT64_MAX || *a == 0) return -1;
	*b = parse_u64(colon + 1);
	if (*b == UINT64_MAX || *b == 0) return -1;
	if (*b < *a) return -1;
	return 0;
}

static int parse_cat_args(int argc, char **argv, cat_opts_t *o) {
	memset(o, 0, sizeof(*o));
	o->common.format = OUT_TEXT;
	o->common.threads = 1;

	int range_count = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val = NULL;

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			cat_help(stdout); exit(0);
		}
		if (strcmp(a, "--json") == 0)    { o->common.format = OUT_JSON; continue; }
		if (strcmp(a, "--compact") == 0) { o->common.format = OUT_COMPACT; continue; }

		if (strncmp(a, "--head=", 7) == 0) {
			val = a + 7;
		} else if (strcmp(a, "--head") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "ambil cat: --head requires a value\n"); return -1;
			}
			val = argv[i];
		}

		if (val) {
			o->head_n = parse_u64(val);
			if (o->head_n == UINT64_MAX || o->head_n == 0) {
				fprintf(stderr, "ambil cat: --head expects positive integer, got '%s'\n", val);
				return -1;
			}
			range_count++;
			continue;
		}

		if (strncmp(a, "--tail=", 7) == 0) {
			val = a + 7;
		} else if (strcmp(a, "--tail") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "ambil cat: --tail requires a value\n"); return -1;
			}
			val = argv[i];
		}

		if (val) {
			o->tail_n = parse_u64(val);
			if (o->tail_n == UINT64_MAX || o->tail_n == 0) {
				fprintf(stderr, "ambil cat: --tail expects positive integer, got '%s'\n", val);
				return -1;
			}
			range_count++;
			continue;
		}

		if (strncmp(a, "--lines=", 8) == 0) {
			val = a + 8;
		} else if (strcmp(a, "--lines") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "ambil cat: --lines requires a value\n"); return -1;
			}
			val = argv[i];
		}

		if (val) {
			if (parse_range(val, &o->line_start, &o->line_end) != 0) {
				fprintf(stderr, "ambil cat: --lines expects N:M (e.g. 10:20), got '%s'\n", val);
				return -1;
			}
			range_count++;
			continue;
		}

		if (strncmp(a, "--bytes=", 8) == 0) {
			val = a + 8;
		} else if (strcmp(a, "--bytes") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "ambil cat: --bytes requires a value\n"); return -1;
			}
			val = argv[i];
		}

		if (val) {
			if (parse_range(val, &o->byte_start, &o->byte_end) != 0) {
				fprintf(stderr, "ambil cat: --bytes expects N:M (e.g. 1:100), got '%s'\n", val);
				return -1;
			}
			range_count++;
			continue;
		}

		if (a[0] == '-' && a[1] != '\0') {
			fprintf(stderr, "ambil cat: unknown option '%s' (try --help)\n", a);
			return -1;
		}

		add_path(o, a);
	}

	if (range_count > 1) {
		fprintf(stderr, "ambil cat: --head, --tail, --lines, --bytes are mutually exclusive\n");
		return -1;
	}

	if (o->n_paths == 0) {
		fprintf(stderr, "ambil cat: missing file path (try --help)\n");
		return -1;
	}

	return 0;
}

typedef struct {
	const char *start;
	size_t      len;
} tail_line_t;

typedef struct {
	const cat_opts_t *o;
	FILE             *out;
	uint64_t          emitted;
	tail_line_t      *ring;
	uint64_t          ring_line;
} text_ctx_t;

static void write_line(FILE *out, const char *line, size_t len) {
	fwrite(line, 1, len, out);
	fputc('\n', out);
}

static int text_line_cb(void *userdata, uint64_t lineno,
                        const char *line, size_t len) {
	text_ctx_t *ctx = (text_ctx_t *)userdata;

	if (ctx->o->tail_n > 0) {
		uint64_t idx = ctx->ring_line % ctx->o->tail_n;
		ctx->ring[idx].start = line;
		ctx->ring[idx].len   = len;
		ctx->ring_line++;
		return 0;
	}

	if (ctx->o->head_n > 0) {
		if (ctx->emitted >= ctx->o->head_n) return 1;
		write_line(ctx->out, line, len);
		ctx->emitted++;
		return 0;
	}

	if (ctx->o->line_start > 0) {
		if (lineno < ctx->o->line_start) return 0;
		if (lineno > ctx->o->line_end) return 1;
		write_line(ctx->out, line, len);
		ctx->emitted++;
		return 0;
	}

	write_line(ctx->out, line, len);
	ctx->emitted++;
	return 0;
}

static void emit_tail_lines(text_ctx_t *ctx, int64_t total) {
	if (ctx->o->tail_n == 0 || ctx->ring_line == 0) return;
	uint64_t ntail = (uint64_t)total;
	if (ntail > ctx->o->tail_n) ntail = ctx->o->tail_n;
	uint64_t start_line = (uint64_t)total - ntail;
	for (uint64_t i = 0; i < ntail; i++) {
		uint64_t idx = (start_line + i) % ctx->o->tail_n;
		write_line(ctx->out, ctx->ring[idx].start, ctx->ring[idx].len);
	}
}

static int cat_file_text(const char *path, const cat_opts_t *o,
                         int multi_file) {
	(void)multi_file;
	lf_mmap_t m;
	if (lf_mmap_open(path, &m) != 0) return -1;

	const char *data = lf_mmap_data(&m);
	size_t size      = lf_mmap_size(&m);

	if (o->byte_start > 0) {
		uint64_t start = o->byte_start - 1;
		uint64_t end   = o->byte_end;
		if (start >= size) { lf_mmap_close(&m); return 0; }
		if (end > size || end == 0) end = size;
		if (start >= end) { lf_mmap_close(&m); return 0; }
		fwrite(data + start, 1, (size_t)(end - start), stdout);
		fflush(stdout);
		lf_mmap_close(&m);
		return 0;
	}

	text_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.o   = o;
	ctx.out = stdout;

	if (o->tail_n > 0) {
		ctx.ring = (tail_line_t *)lf_xcalloc((size_t)o->tail_n, sizeof(tail_line_t));
	}

	int64_t total = lf_for_each_line(data, size, text_line_cb, &ctx);

	if (o->tail_n > 0) {
		emit_tail_lines(&ctx, total);
	}

	free(ctx.ring);
	lf_mmap_close(&m);
	return 0;
}

static int cat_file_compact(const char *path, const cat_opts_t *o,
                            int multi_file) {
	if (multi_file) {
		printf("==> %s <==\n", path);
	}
	return cat_file_text(path, o, 0);
}

typedef struct {
	lf_buf_t        buf;
	uint64_t        seq;
	const char     *path;
	const cat_opts_t *o;
	uint64_t        emitted;
} json_line_ctx_t;

static int json_line_cb(void *userdata, uint64_t lineno,
                        const char *line, size_t len) {
	json_line_ctx_t *jctx = (json_line_ctx_t *)userdata;

	if (jctx->o->head_n > 0) {
		if (jctx->emitted >= jctx->o->head_n) return 0;
	}
	if (jctx->o->line_start > 0) {
		if (lineno < jctx->o->line_start) return 0;
		if (lineno > jctx->o->line_end) return 1;
	}

	lf_json_begin_object(&jctx->buf);
	lf_json_key(&jctx->buf, "type");
	lf_json_string(&jctx->buf, "line", 4);
	lf_json_key(&jctx->buf, "seq");
	lf_json_uint(&jctx->buf, jctx->seq++);
	lf_json_key(&jctx->buf, "path");
	lf_json_string(&jctx->buf, jctx->path, strlen(jctx->path));
	lf_json_key(&jctx->buf, "line_number");
	lf_json_uint(&jctx->buf, lineno);
	lf_json_key(&jctx->buf, "text");
	lf_json_string(&jctx->buf, line, len);
	lf_json_end_object(&jctx->buf);
	lf_buf_append(&jctx->buf, "\n", 1);
	jctx->emitted++;
	return 0;
}

typedef struct {
	tail_line_t *ring;
	uint64_t     ring_line;
	uint64_t     tail_n;
} tail_cap_t;

static int tail_cap_cb(void *userdata, uint64_t lineno,
                       const char *line, size_t len) {
	(void)lineno;
	tail_cap_t *tc = (tail_cap_t *)userdata;
	uint64_t idx = tc->ring_line % tc->tail_n;
	tc->ring[idx].start = line;
	tc->ring[idx].len   = len;
	tc->ring_line++;
	return 0;
}

static void emit_json_begin(lf_buf_t *b, uint64_t seq,
                            const char *path, uint64_t total_lines) {
	lf_json_begin_object(b);
	lf_json_key(b, "type");
	lf_json_string(b, "begin", 5);
	lf_json_key(b, "seq");
	lf_json_uint(b, seq);
	lf_json_key(b, "path");
	lf_json_string(b, path, strlen(path));
	lf_json_key(b, "total_lines");
	lf_json_uint(b, total_lines);
	lf_json_end_object(b);
	lf_buf_append(b, "\n", 1);
}

static void emit_json_end(lf_buf_t *b, uint64_t seq, const char *path) {
	lf_json_begin_object(b);
	lf_json_key(b, "type");
	lf_json_string(b, "end", 3);
	lf_json_key(b, "seq");
	lf_json_uint(b, seq);
	lf_json_key(b, "path");
	lf_json_string(b, path, strlen(path));
	lf_json_end_object(b);
	lf_buf_append(b, "\n", 1);
}

static int cat_file_json(const char *path, const cat_opts_t *o,
                         uint64_t *global_seq) {
	lf_mmap_t m;
	if (lf_mmap_open(path, &m) != 0) {
		lf_buf_t eb;
		lf_buf_init(&eb);
		uint64_t es = (*global_seq)++;
		lf_json_begin_object(&eb);
		lf_json_key(&eb, "type");   lf_json_string(&eb, "error", 5);
		lf_json_key(&eb, "seq");    lf_json_uint(&eb, es);
		lf_json_key(&eb, "code");   lf_json_string(&eb, "EIO", 3);
		lf_json_key(&eb, "path");   lf_json_string(&eb, path, strlen(path));
		lf_json_key(&eb, "subcommand"); lf_json_string(&eb, "cat", 3);
		lf_json_end_object(&eb);
		lf_buf_append(&eb, "\n", 1);
		fwrite(eb.data, 1, eb.len, stderr);
		fflush(stderr);
		lf_buf_free(&eb);
		return -1;
	}

	const char *data = lf_mmap_data(&m);
	size_t size      = lf_mmap_size(&m);

	lf_buf_t out;
	lf_buf_init(&out);
	uint64_t file_seq = (*global_seq)++;

	if (o->byte_start > 0) {
		uint64_t start = o->byte_start - 1;
		uint64_t end   = o->byte_end;
		if (end == 0 || end > size) end = size;

		emit_json_begin(&out, file_seq, path, 0);

		if (start < end) {
			lf_json_begin_object(&out);
			lf_json_key(&out, "type");
			lf_json_string(&out, "bytes", 5);
			lf_json_key(&out, "seq");
			lf_json_uint(&out, file_seq);
			lf_json_key(&out, "path");
			lf_json_string(&out, path, strlen(path));
			lf_json_key(&out, "start");
			lf_json_uint(&out, o->byte_start);
			lf_json_key(&out, "end");
			lf_json_uint(&out, end);
			lf_json_end_object(&out);
			lf_buf_append(&out, "\n", 1);
		}
	} else if (o->tail_n > 0) {
		tail_cap_t tc;
		memset(&tc, 0, sizeof(tc));
		tc.tail_n = o->tail_n;
		tc.ring = (tail_line_t *)lf_xcalloc((size_t)o->tail_n, sizeof(tail_line_t));

		int64_t total = lf_for_each_line(data, size, tail_cap_cb, &tc);
		uint64_t count = (uint64_t)total;
		uint64_t ntail = (count > o->tail_n) ? o->tail_n : count;
		uint64_t start_line = (count >= ntail) ? (count - ntail) : 0;

		emit_json_begin(&out, file_seq, path, count);

		for (uint64_t k = 0; k < ntail; k++) {
			uint64_t idx = (start_line + k) % o->tail_n;
			lf_json_begin_object(&out);
			lf_json_key(&out, "type");
			lf_json_string(&out, "line", 4);
			lf_json_key(&out, "seq");
			lf_json_uint(&out, (unsigned long long)k);
			lf_json_key(&out, "path");
			lf_json_string(&out, path, strlen(path));
			lf_json_key(&out, "line_number");
			lf_json_uint(&out, start_line + k + 1);
			lf_json_key(&out, "text");
			lf_json_string(&out, tc.ring[idx].start, tc.ring[idx].len);
			lf_json_end_object(&out);
			lf_buf_append(&out, "\n", 1);
		}

		free(tc.ring);
		*global_seq = *global_seq; /* seq tracking: tail uses inline counters */
	} else {
		json_line_ctx_t jctx;
		memset(&jctx, 0, sizeof(jctx));
		jctx.path = path;
		jctx.o = o;
		lf_buf_init(&jctx.buf);
		jctx.seq = *global_seq;

		int64_t total = lf_for_each_line(data, size, json_line_cb, &jctx);

		emit_json_begin(&out, file_seq, path, (uint64_t)total);
		lf_buf_append(&out, jctx.buf.data, jctx.buf.len);
		*global_seq = jctx.seq;
		lf_buf_free(&jctx.buf);
	}

	emit_json_end(&out, file_seq, path);

	fwrite(out.data, 1, out.len, stdout);
	fflush(stdout);
	lf_buf_free(&out);
	lf_mmap_close(&m);
	return 0;
}

int run_cat(int argc, char **argv) {
	cat_opts_t o;
	if (parse_cat_args(argc, argv, &o) != 0) {
		cat_opts_free(&o);
		return 2;
	}

	int multi_file = (o.n_paths > 1);
	uint64_t global_seq = 0;
	int errors = 0;

	for (size_t pi = 0; pi < o.n_paths; pi++) {
		const char *path = o.paths[pi];

		switch (o.common.format) {
		case OUT_JSON:
			if (cat_file_json(path, &o, &global_seq) != 0) errors++;
			break;
		case OUT_COMPACT:
			if (cat_file_compact(path, &o, multi_file) != 0) errors++;
			break;
		default:
			if (cat_file_text(path, &o, multi_file) != 0) errors++;
			break;
		}
	}

	cat_opts_free(&o);
	return errors > 0 ? 2 : 0;
}
