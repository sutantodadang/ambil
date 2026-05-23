#include "wc.h"
#include "file_reader.h"
#include "json_emit.h"
#include "util.h"
#include "help.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static void wc_help(FILE *out) {
	lf_help_usage(out, "ambil", "--cmd wc", "[options] FILE...");
	lf_help_spacer(out);
	lf_help_desc(out, "Count lines, words, characters, and bytes.");
	lf_help_spacer(out);
	lf_help_section(out, "Count options (default: lines + words + chars)");
	lf_help_flag(out, "--lines",       "line count");
	lf_help_flag(out, "--words",       "word count");
	lf_help_flag(out, "--chars",       "character count");
	lf_help_flag(out, "--bytes",       "byte count (file size)");
	lf_help_spacer(out);
	lf_help_section(out, "Output format");
	lf_help_flag(out, "--json",        "NDJSON structured output");
	lf_help_spacer(out);
	lf_help_flag(out, "-h, --help",    "show this help");
}

void wc_opts_free(wc_opts_t *o) {
	if (!o) return;
	free(o->paths);
	memset(o, 0, sizeof(*o));
}

static void add_path(wc_opts_t *o, const char *p) {
	if (o->n_paths >= o->cap_paths) {
		size_t nc = o->cap_paths ? o->cap_paths * 2 : 8;
		o->paths = (const char **)lf_xrealloc(o->paths, nc * sizeof(char *));
		o->cap_paths = nc;
	}
	o->paths[o->n_paths++] = p;
}

static int parse_wc_args(int argc, char **argv, wc_opts_t *o) {
	memset(o, 0, sizeof(*o));
	o->common.format = OUT_TEXT;
	o->common.threads = 1;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			wc_help(stdout); exit(0);
		}
		if (strcmp(a, "--json") == 0)   { o->common.format = OUT_JSON; continue; }
		if (strcmp(a, "--lines") == 0)  { o->show_lines = 1; continue; }
		if (strcmp(a, "--words") == 0)  { o->show_words = 1; continue; }
		if (strcmp(a, "--chars") == 0)  { o->show_chars = 1; continue; }
		if (strcmp(a, "--bytes") == 0)  { o->show_bytes = 1; continue; }

		if (a[0] == '-' && a[1] != '\0') {
			fprintf(stderr, "ambil wc: unknown option '%s' (try --help)\n", a);
			return -1;
		}

		add_path(o, a);
	}

	if (o->n_paths == 0) {
		fprintf(stderr, "ambil wc: missing file path (try --help)\n");
		return -1;
	}

	return 0;
}

typedef struct {
	uint64_t lines;
	uint64_t words;
	uint64_t chars;
	uint64_t bytes;
} wc_counts_t;

static int wc_count_cb(void *userdata, uint64_t lineno,
                       const char *line, size_t len) {
	(void)lineno;
	wc_counts_t *c = (wc_counts_t *)userdata;
	c->lines++;
	c->chars += (uint64_t)len + 1; /* +1 for stripped \n */
	c->bytes += (uint64_t)len + 1;

	int in_word = 0;
	for (size_t i = 0; i < len; i++) {
		int space = isspace((unsigned char)line[i]);
		if (!space && !in_word) {
			c->words++;
			in_word = 1;
		} else if (space) {
			in_word = 0;
		}
	}
	return 0;
}

static int wc_count_file(const char *path, wc_counts_t *c) {
	memset(c, 0, sizeof(*c));

	lf_mmap_t m;
	if (lf_mmap_open(path, &m) != 0) return -1;

	const char *data = lf_mmap_data(&m);
	size_t size      = lf_mmap_size(&m);

	lf_for_each_line(data, size, wc_count_cb, c);

	c->bytes = size;

	lf_mmap_close(&m);
	return 0;
}

static void wc_print_text(const wc_opts_t *o, const wc_counts_t *c,
                          const char *path) {
	int explicit_flags = o->show_lines || o->show_words ||
	                     o->show_chars || o->show_bytes;

	if (o->show_lines || (!explicit_flags)) printf("%7" PRIu64 " ", c->lines);
	if (o->show_words || (!explicit_flags)) printf("%7" PRIu64 " ", c->words);
	if (o->show_chars || (!explicit_flags)) printf("%7" PRIu64 " ", c->chars);
	if (o->show_bytes) printf("%7" PRIu64 " ", c->bytes);

	printf("%s\n", path);
}

static void wc_print_json(const wc_opts_t *o, const wc_counts_t *c,
                          const char *path, uint64_t seq) {
	(void)o;
	lf_buf_t b;
	lf_buf_init(&b);

	lf_json_begin_object(&b);
	lf_json_key(&b, "type");
	lf_json_string(&b, "count", 5);
	lf_json_key(&b, "seq");
	lf_json_uint(&b, seq);
	lf_json_key(&b, "path");
	lf_json_string(&b, path, strlen(path));
	lf_json_key(&b, "lines");
	lf_json_uint(&b, c->lines);
	lf_json_key(&b, "words");
	lf_json_uint(&b, c->words);
	lf_json_key(&b, "chars");
	lf_json_uint(&b, c->chars);
	lf_json_key(&b, "bytes");
	lf_json_uint(&b, c->bytes);
	lf_json_end_object(&b);
	lf_buf_append(&b, "\n", 1);

	fwrite(b.data, 1, b.len, stdout);
	fflush(stdout);
	lf_buf_free(&b);
}

int run_wc(int argc, char **argv) {
	wc_opts_t o;
	if (parse_wc_args(argc, argv, &o) != 0) {
		wc_opts_free(&o);
		return 2;
	}

	wc_counts_t total;
	memset(&total, 0, sizeof(total));
	int errors = 0;

	for (size_t pi = 0; pi < o.n_paths; pi++) {
		wc_counts_t c;
		if (wc_count_file(o.paths[pi], &c) != 0) {
			errors++;
			continue;
		}

		switch (o.common.format) {
		case OUT_JSON:
			wc_print_json(&o, &c, o.paths[pi], (uint64_t)pi);
			break;
		default:
			wc_print_text(&o, &c, o.paths[pi]);
			break;
		}

		total.lines += c.lines;
		total.words += c.words;
		total.chars += c.chars;
		total.bytes += c.bytes;
	}

	if (o.n_paths > 1 && o.common.format != OUT_JSON) {
		wc_print_text(&o, &total, "total");
	}

	wc_opts_free(&o);
	return errors > 0 ? 2 : 0;
}
