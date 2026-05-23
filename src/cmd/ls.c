#include "ls.h"
#include "help.h"
#include "ignore.h"
#include "json_emit.h"
#include "platform_stat.h"
#include "util.h"
#include "walker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void add_path(ls_opts_t *o, const char *p) {
	if (o->n_paths >= o->cap_paths) {
		size_t nc = o->cap_paths ? o->cap_paths * 2 : 8;
		o->paths = (const char **)lf_xrealloc(o->paths, nc * sizeof(char *));
		o->cap_paths = nc;
	}
	o->paths[o->n_paths++] = p;
}

static int parse_int(const char *s, long *out) {
	char *endp;
	long v = strtol(s, &endp, 10);
	if (*endp != '\0') return -1;
	*out = v;
	return 0;
}

static void ls_help(FILE *out) {
	lf_help_usage(out, "ambil", "ls", "[options] [PATH...]");
	lf_help_desc(out, "List files and directories. Defaults to current directory, paths-only display.");
	lf_help_spacer(out);
	lf_help_section(out, "Display modes");
	lf_help_flag(out, "--display paths",  "one path per line (default)");
	lf_help_flag(out, "--display long",   "permissions, size, date, name");
	lf_help_flag(out, "--display tree",   "indented tree view");
	lf_help_spacer(out);
	lf_help_section(out, "Walk options");
	lf_help_flag(out, "-R, --recursive",  "descend into subdirectories");
	lf_help_flag(out, "--depth N",        "max depth (0 = root only)");
	lf_help_spacer(out);
	lf_help_section(out, "Output format");
	lf_help_flag(out, "--json",           "NDJSON, one object per entry");
	lf_help_flag(out, "--compact",        "compact token-efficient output");
	lf_help_flag(out, "--no-color",       "disable color");
	fprintf(out, "\nDefault format is text (classic listing).\n");
}

static int parse_ls_args(int argc, char **argv, ls_opts_t *o) {
	int i;
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			ls_help(stdout);
			return 1;
		}
		if (strcmp(arg, "--json") == 0) {
			o->common.format = OUT_JSON;
			continue;
		}
		if (strcmp(arg, "--compact") == 0) {
			o->common.format = OUT_COMPACT;
			continue;
		}
		if (strcmp(arg, "--no-color") == 0) {
			o->common.color = COLOR_NEVER;
			continue;
		}
		if (strcmp(arg, "--recursive") == 0 || strcmp(arg, "-R") == 0) {
			o->recursive = 1;
			continue;
		}
		if (strcmp(arg, "--display") == 0) {
			if (++i >= argc) { fprintf(stderr, "ambil ls: --display requires a value\n"); return -1; }
			const char *v = argv[i];
			if (strcmp(v, "paths") == 0) o->display = DISP_PATHS;
			else if (strcmp(v, "long") == 0) o->display = DISP_LONG;
			else if (strcmp(v, "tree") == 0) o->display = DISP_TREE;
			else { fprintf(stderr, "ambil ls: unknown display mode '%s' (paths|long|tree)\n", v); return -1; }
			continue;
		}
		if (strcmp(arg, "--depth") == 0) {
			if (++i >= argc) { fprintf(stderr, "ambil ls: --depth requires a value\n"); return -1; }
			long v;
			if (parse_int(argv[i], &v) != 0 || v < 0) {
				fprintf(stderr, "ambil ls: invalid depth '%s'\n", argv[i]);
				return -1;
			}
			o->max_depth = (int)v;
			continue;
		}
		if (arg[0] == '-' && arg[1] != '\0') {
			fprintf(stderr, "ambil ls: unknown flag '%s'\n", arg);
			return -1;
		}
		add_path(o, arg);
	}
	return 0;
}

static const char *mode_string(const lf_stat_t *st) {
	static char buf[11];
	uint32_t mode = st->mode;
	buf[0] = st->is_dir ? 'd' : (st->is_symlink ? 'l' : '-');
	buf[1] = (mode & 0400) ? 'r' : '-';
	buf[2] = (mode & 0200) ? 'w' : '-';
	buf[3] = (mode & 0100) ? 'x' : '-';
	buf[4] = (mode & 0040) ? 'r' : '-';
	buf[5] = (mode & 0020) ? 'w' : '-';
	buf[6] = (mode & 0010) ? 'x' : '-';
	buf[7] = (mode & 0004) ? 'r' : '-';
	buf[8] = (mode & 0002) ? 'w' : '-';
	buf[9] = (mode & 0001) ? 'x' : '-';
	buf[10] = '\0';
	return buf;
}

static void format_iso_time(int64_t epoch, char *out, size_t n) {
	time_t t = (time_t)epoch;
	struct tm *tm = gmtime(&t);
	if (!tm) { snprintf(out, n, "1970-01-01T00:00:00Z"); return; }
	strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", tm);
}

static const char *display_path(const lf_walk_entry_t *ent) {
	if (ent->rel_path && ent->rel_path[0]) return ent->rel_path;
	if (ent->path && ent->path[0]) return ent->path;
	return ".";
}

static int ls_print_text_paths(lf_walker_t *w, const ls_opts_t *o) {
	lf_walk_entry_t ent;
	int count = 0;
	while (lf_walk_next(w, &ent)) {
		printf("%s\n", display_path(&ent));
		count++;
	}
	(void)o;
	return count;
}

static int ls_print_text_long(lf_walker_t *w, const ls_opts_t *o) {
	lf_walk_entry_t ent;
	int count = 0;
	char timebuf[32];
	while (lf_walk_next(w, &ent)) {
		const char *dp = display_path(&ent);
		format_iso_time(ent.st.mtime, timebuf, sizeof(timebuf));
		printf("%s %12llu %s %s\n", mode_string(&ent.st),
		       (unsigned long long)ent.st.size, timebuf, dp);
		count++;
	}
	(void)o;
	return count;
}

static int ls_print_text_tree(lf_walker_t *w, const ls_opts_t *o) {
	lf_walk_entry_t ent;
	int count = 0;
	while (lf_walk_next(w, &ent)) {
		int depth = 0;
		for (const char *p = ent.rel_path; *p; p++) {
			if (lf_is_sep(*p)) depth++;
		}
		for (int d = 0; d < depth; d++) printf("  ");
		const char *bn = ent.rel_path;
		const char *last_sep = NULL;
		for (const char *p = ent.rel_path; *p; p++) {
			if (lf_is_sep(*p)) last_sep = p;
		}
		if (last_sep) bn = last_sep + 1;
		printf("%s%s\n", bn, ent.st.is_dir ? "/" : "");
		count++;
	}
	(void)o;
	return count;
}

typedef struct {
	lf_buf_t buf;
	int      first;
} ls_json_ctx_t;

static void ls_emit_json_entry(ls_json_ctx_t *ctx, const lf_walk_entry_t *ent) {
	lf_buf_t *b = &ctx->buf;
	char timebuf[32];

	if (ctx->first) ctx->first = 0;
	else lf_buf_append(b, "\n", 1);

	const char *dp = ent->rel_path && ent->rel_path[0] ? ent->rel_path : ent->path;
	if (!dp || !dp[0]) dp = ".";

	lf_json_begin_object(b);
	lf_json_key(b, "type"); lf_json_string(b, "entry", 5); lf_buf_append(b, ",", 1);
	lf_json_key(b, "path"); lf_json_string(b, dp, strlen(dp)); lf_buf_append(b, ",", 1);
	lf_json_key(b, "size"); lf_json_uint(b, ent->st.size); lf_buf_append(b, ",", 1);
	lf_json_key(b, "mode"); lf_json_string(b, mode_string(&ent->st), 10); lf_buf_append(b, ",", 1);
	format_iso_time(ent->st.mtime, timebuf, sizeof(timebuf));
	lf_json_key(b, "mtime"); lf_json_string(b, timebuf, strlen(timebuf)); lf_buf_append(b, ",", 1);
	lf_json_key(b, "is_dir"); lf_json_bool(b, ent->st.is_dir); lf_buf_append(b, ",", 1);
	lf_json_key(b, "is_file"); lf_json_bool(b, ent->st.is_file);
	lf_json_end_object(b);
}

static int ls_print_json(lf_walker_t *w, const ls_opts_t *o) {
	ls_json_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.first = 1;
	lf_buf_init(&ctx.buf);

	lf_walk_entry_t ent;
	int count = 0;
	while (lf_walk_next(w, &ent)) {
		ls_emit_json_entry(&ctx, &ent);
		count++;
	}
	if (ctx.buf.len > 0) printf("%s\n", ctx.buf.data);
	lf_buf_free(&ctx.buf);
	(void)o;
	return count;
}

static int ls_print_compact(lf_walker_t *w, const ls_opts_t *o) {
	lf_walk_entry_t ent;
	int count = 0;
	char timebuf[32];
	while (lf_walk_next(w, &ent)) {
		const char *dp = display_path(&ent);
		if (o->display == DISP_PATHS) {
			printf("%s\n", dp);
		} else if (o->display == DISP_LONG) {
			format_iso_time(ent.st.mtime, timebuf, sizeof(timebuf));
			printf("%s\t%llu\t%s\n", dp,
			       (unsigned long long)ent.st.size, timebuf);
		} else {
			int depth = 0;
			for (const char *p = dp; *p; p++) {
				if (lf_is_sep(*p)) depth++;
			}
			for (int d = 0; d < depth; d++) printf("  ");
			const char *bn = dp;
			const char *last_sep = NULL;
			for (const char *p = dp; *p; p++) {
				if (lf_is_sep(*p)) last_sep = p;
			}
			if (last_sep) bn = last_sep + 1;
			printf("%s%s\n", bn, ent.st.is_dir ? "/" : "");
		}
		count++;
	}
	(void)o;
	return count;
}

static int run_ls_impl(ls_opts_t *o) {
	grep_opts_t go;
	memset(&go, 0, sizeof(go));
	lf_ignore_t *ig = lf_ignore_new(&go);
	lf_walker_t *w = lf_walk_open(o->paths, o->n_paths, ig,
	                              o->recursive, o->max_depth, 0, 1);

	int count;
	switch (o->common.format) {
	case OUT_JSON:    count = ls_print_json(w, o);    break;
	case OUT_COMPACT: count = ls_print_compact(w, o); break;
	default: {
		switch (o->display) {
		case DISP_LONG:  count = ls_print_text_long(w, o);  break;
		case DISP_TREE:  count = ls_print_text_tree(w, o);  break;
		default:         count = ls_print_text_paths(w, o); break;
		}
		break;
	}
	}

	unsigned werrs = lf_walk_error_count(w);
	lf_walk_close(w);
	lf_ignore_free(ig);
	fflush(stdout);

	if (werrs > 0) return 2;
	return count > 0 ? 0 : 0;
}

void ls_opts_free(ls_opts_t *o) {
	free((void *)o->paths);
}

int run_ls(int argc, char **argv) {
	ls_opts_t o;
	memset(&o, 0, sizeof(o));
	o.common.format = OUT_TEXT;
	o.display = DISP_PATHS;
	o.recursive = 0;
	o.max_depth = -1;

	int prc = parse_ls_args(argc, argv, &o);
	if (prc == 1) { ls_opts_free(&o); return 0; }
	if (prc != 0) { ls_opts_free(&o); return 2; }
	if (o.n_paths == 0) {
		o.paths = (const char **)lf_xmalloc(2 * sizeof(char *));
		o.paths[0] = ".";
		o.paths[1] = NULL;
		o.cap_paths = 2;
		o.n_paths = 1;
	}
	int rc = run_ls_impl(&o);
	ls_opts_free(&o);
	return rc;
}
