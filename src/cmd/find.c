#include "find.h"
#include "help.h"
#include "ignore.h"
#include "json_emit.h"
#include "platform_stat.h"
#include "util.h"
#include "walker.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void add_path(find_opts_t *o, const char *p) {
	if (o->n_paths >= o->cap_paths) {
		size_t nc = o->cap_paths ? o->cap_paths * 2 : 8;
		o->paths = (const char **)lf_xrealloc(o->paths, nc * sizeof(char *));
		o->cap_paths = nc;
	}
	o->paths[o->n_paths++] = p;
}

static int find_help(FILE *out) {
	lf_help_usage(out, "ambil", "find", "[filters] [PATH...]");
	lf_help_desc(out, "Recursively list files matching filters. Defaults to current directory.");
	lf_help_spacer(out);
	lf_help_section(out, "Filters");
	lf_help_flag(out, "--name GLOB",    "basename glob match (*, ? supported)");
	lf_help_flag(out, "--type f|d",     "file or directory only");
	lf_help_flag(out, "--size +N|-N",   "size greater/less than N (suffix K/M/G)");
	lf_help_flag(out, "--mtime +N|-N",  "modified older/newer than N days");
	lf_help_spacer(out);
	lf_help_section(out, "Output format");
	lf_help_flag(out, "--json",         "NDJSON, one object per result");
	lf_help_flag(out, "--compact",      "compact token-efficient output");
	lf_help_flag(out, "--no-color",     "disable color");
	fprintf(out, "\nDefault format is text (paths only).\n");
	return 0;
}

static int parse_size(const char *s, uint64_t *out) {
	char *end;
	unsigned long long v = strtoull(s, &end, 10);
	if (end == s) return -1;
	uint64_t mult = 1;
	if (*end == 'K' || *end == 'k') { mult = 1024ULL; end++; }
	else if (*end == 'M' || *end == 'm') { mult = 1024ULL * 1024ULL; end++; }
	else if (*end == 'G' || *end == 'g') { mult = 1024ULL * 1024ULL * 1024ULL; end++; }
	if (*end != '\0') return -1;
	*out = v * mult;
	return 0;
}

static int parse_find_args(int argc, char **argv, find_opts_t *o) {
	int i;
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			find_help(stdout);
			return 1;
		}
		if (strcmp(arg, "--json") == 0) {
			o->common.format = OUT_JSON; continue;
		}
		if (strcmp(arg, "--compact") == 0) {
			o->common.format = OUT_COMPACT; continue;
		}
		if (strcmp(arg, "--no-color") == 0) {
			o->common.color = COLOR_NEVER; continue;
		}
		if (strcmp(arg, "--name") == 0) {
			if (++i >= argc) { fprintf(stderr, "ambil find: --name requires a value\n"); return -1; }
			o->name_glob = argv[i]; continue;
		}
		if (strcmp(arg, "--type") == 0) {
			if (++i >= argc) { fprintf(stderr, "ambil find: --type requires a value\n"); return -1; }
			const char *v = argv[i];
			if (strcmp(v, "f") == 0) o->type_filter = 'f';
			else if (strcmp(v, "d") == 0) o->type_filter = 'd';
			else { fprintf(stderr, "ambil find: unknown --type '%s' (use f or d)\n", v); return -1; }
			continue;
		}
		if (strcmp(arg, "--size") == 0) {
			if (++i >= argc) { fprintf(stderr, "ambil find: --size requires a value\n"); return -1; }
			const char *v = argv[i];
			if (*v == '+') { o->size_op = FIND_OP_GT; v++; }
			else if (*v == '-') { o->size_op = FIND_OP_LT; v++; }
			else o->size_op = FIND_OP_EQ;
			if (parse_size(v, &o->size_val) != 0) {
				fprintf(stderr, "ambil find: invalid --size '%s'\n", argv[i]); return -1;
			}
			continue;
		}
		if (strcmp(arg, "--mtime") == 0) {
			if (++i >= argc) { fprintf(stderr, "ambil find: --mtime requires a value\n"); return -1; }
			const char *v = argv[i];
			if (*v == '+') { o->mtime_op = FIND_OP_GT; v++; }
			else if (*v == '-') { o->mtime_op = FIND_OP_LT; v++; }
			else o->mtime_op = FIND_OP_EQ;
			char *end;
			long days = strtol(v, &end, 10);
			if (*end != '\0' || days < 0) {
				fprintf(stderr, "ambil find: invalid --mtime '%s'\n", argv[i]); return -1;
			}
			o->mtime_days = (int64_t)days;
			continue;
		}
		if (arg[0] == '-' && arg[1] != '\0') {
			fprintf(stderr, "ambil find: unknown flag '%s'\n", arg); return -1;
		}
		add_path(o, arg);
	}
	return 0;
}

static int glob_match(const char *pat, const char *name) {
	while (*pat && *name) {
		if (*pat == '*') {
			pat++;
			if (!*pat) return 1;
			for (const char *n = name; *n; n++) {
				if (glob_match(pat, n)) return 1;
			}
			return 0;
		}
		if (*pat == '?') { pat++; name++; continue; }
		if (*pat != *name) return 0;
		pat++; name++;
	}
	while (*pat == '*') pat++;
	return *pat == '\0' && *name == '\0';
}

static const char *find_display_path(const lf_walk_entry_t *ent) {
	if (ent->rel_path && ent->rel_path[0]) return ent->rel_path;
	if (ent->path && ent->path[0]) return ent->path;
	return ".";
}

static int match_filters(const lf_walk_entry_t *ent, const find_opts_t *o) {
	if (o->name_glob) {
		const char *dp = find_display_path(ent);
		const char *bn = dp;
		const char *last = NULL;
		for (const char *p = dp; *p; p++) {
			if (lf_is_sep(*p)) last = p;
		}
		if (last) bn = last + 1;
		if (!glob_match(o->name_glob, bn)) return 0;
	}
	if (o->type_filter) {
		if (o->type_filter == 'f' && !ent->st.is_file) return 0;
		if (o->type_filter == 'd' && !ent->st.is_dir)  return 0;
	}
	if (o->size_op != FIND_OP_NONE) {
		int pass;
		switch (o->size_op) {
		case FIND_OP_GT: pass = ent->st.size > o->size_val; break;
		case FIND_OP_LT: pass = ent->st.size < o->size_val; break;
		default:         pass = ent->st.size == o->size_val; break;
		}
		if (!pass) return 0;
	}
	if (o->mtime_op != FIND_OP_NONE) {
		int64_t now = (int64_t)time(NULL);
		int64_t age_secs = now - ent->st.mtime;
		int64_t age_days = age_secs / 86400;
		if (age_secs < 0) age_days = 0;
		int pass;
		switch (o->mtime_op) {
		case FIND_OP_GT: pass = age_days > o->mtime_days; break;
		case FIND_OP_LT: pass = age_days < o->mtime_days; break;
		default:         pass = age_days == o->mtime_days; break;
		}
		if (!pass) return 0;
	}
	return 1;
}

static void emit_json_entry(lf_buf_t *b,
                            const lf_walk_entry_t *ent, int *first) {
	char timebuf[32];
	time_t t = (time_t)ent->st.mtime;
	struct tm *tm = gmtime(&t);
	if (tm) strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", tm);
	else snprintf(timebuf, sizeof(timebuf), "1970-01-01T00:00:00Z");

	if (*first) *first = 0;
	else lf_buf_append(b, "\n", 1);

	lf_json_begin_object(b);
	lf_json_key(b, "type"); lf_json_string(b, "entry", 5); lf_buf_append(b, ",", 1);
	const char *dp = find_display_path(ent);
	lf_json_key(b, "path"); lf_json_string(b, dp, strlen(dp)); lf_buf_append(b, ",", 1);
	lf_json_key(b, "size"); lf_json_uint(b, ent->st.size); lf_buf_append(b, ",", 1);
	lf_json_key(b, "mtime"); lf_json_string(b, timebuf, strlen(timebuf)); lf_buf_append(b, ",", 1);
	lf_json_key(b, "is_dir"); lf_json_bool(b, ent->st.is_dir); lf_buf_append(b, ",", 1);
	lf_json_key(b, "is_file"); lf_json_bool(b, ent->st.is_file);
	lf_json_end_object(b);
}

static int run_find_impl(find_opts_t *o) {
	grep_opts_t go;
	memset(&go, 0, sizeof(go));
	lf_ignore_t *ig = lf_ignore_new(&go);
	lf_walker_t *w = lf_walk_open(o->paths, o->n_paths, ig, 1, -1, 0, 1);

	int count = 0;
	lf_walk_entry_t ent;

	if (o->common.format == OUT_JSON) {
		lf_buf_t buf;
		lf_buf_init(&buf);
		int first = 1;
		while (lf_walk_next(w, &ent)) {
			if (!match_filters(&ent, o)) continue;
			emit_json_entry(&buf, &ent, &first);
			count++;
		}
		if (buf.len > 0) printf("%s\n", buf.data);
		lf_buf_free(&buf);
	} else {
		while (lf_walk_next(w, &ent)) {
			if (!match_filters(&ent, o)) continue;
			printf("%s\n", find_display_path(&ent));
			count++;
		}
	}

	unsigned werrs = lf_walk_error_count(w);
	lf_walk_close(w);
	lf_ignore_free(ig);
	fflush(stdout);

	if (werrs > 0) return 2;
	return count > 0 ? 0 : 1;
}

void find_opts_free(find_opts_t *o) {
	free((void *)o->paths);
}

int run_find(int argc, char **argv) {
	find_opts_t o;
	memset(&o, 0, sizeof(o));
	o.common.format = OUT_TEXT;

	int prc = parse_find_args(argc, argv, &o);
	if (prc == 1) { find_opts_free(&o); return 0; }
	if (prc != 0) { find_opts_free(&o); return 2; }
	if (o.n_paths == 0) {
		o.paths = (const char **)lf_xmalloc(2 * sizeof(char *));
		o.paths[0] = ".";
		o.paths[1] = NULL;
		o.cap_paths = 2;
		o.n_paths = 1;
	}
	int rc = run_find_impl(&o);
	find_opts_free(&o);
	return rc;
}
