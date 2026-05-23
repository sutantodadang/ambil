/*
 * ignore.c — gitignore engine + defaults + globs + types.
 *
 * Implementation notes:
 *   - Patterns from all sources (defaults, .gitignore stack, user -g, types)
 *     are stored in one flat dynamic array. Each entry remembers its scope
 *     prefix (so .gitignore inside foo/ matches foo/bar but not bar/).
 *   - Matching walks the array LAST-TO-FIRST and short-circuits on the first
 *     pattern that decides; this gives gitignore "later overrides earlier"
 *     semantics naturally because we push later patterns later.
 *   - Globbing is a small recursive descent supporting *, ?, **.
 */
#include "ignore.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- pattern table ---------- */

typedef struct {
    char *pattern;       /* glob (without leading '!' or trailing '/') */
    char *prefix;        /* dir prefix this rule is anchored under (rel to walk root) */
    int   negate;        /* !pat */
    int   anchored;      /* leading / : match only at prefix boundary */
    int   dir_only;      /* trailing / : matches directories only */
    int   basename_only; /* no '/' inside pattern : match against basename only */
    int   user_glob;     /* from -g; user globs override others */
    int   type_filter;   /* from -t; behaves like include filter */
    int   include;       /* user_glob without leading '!' = include */
} pat_t;

struct lf_ignore_s {
    pat_t *pats;
    size_t n, cap;

    int hidden;          /* allow hidden files (.foo) */
    int no_ignore;       /* disable defaults + gitignore + hidden filter */

    /* Type filter active flag: if any -t was supplied, files must match
     * one of those type extensions to be searched. */
    int type_active;

    /* Track dir-stack markers (push/pop). */
};

static void push_pat(lf_ignore_t *ig, pat_t p) {
    if (ig->n == ig->cap) {
        ig->cap = ig->cap ? ig->cap * 2 : 64;
        ig->pats = (pat_t *)lf_xrealloc(ig->pats, ig->cap * sizeof(pat_t));
    }
    ig->pats[ig->n++] = p;
}

/* Strip surrounding whitespace and comments. Returns NULL if nothing left. */
static char *clean_line(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || *s == '#') return NULL;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    if (*s == '\0') return NULL;
    return s;
}

/* Parse one gitignore-style line into the table. */
static void add_pattern(lf_ignore_t *ig, const char *raw,
                        const char *prefix, int user_glob, int type_filter) {
    char *buf = lf_xstrdup(raw);
    char *s = clean_line(buf);
    if (!s) { free(buf); return; }

    pat_t p;
    memset(&p, 0, sizeof(p));
    p.user_glob   = user_glob;
    p.type_filter = type_filter;
    p.include     = user_glob ? 1 : 0;

    if (*s == '!') { p.negate = 1; s++; if (user_glob) p.include = 0; }
    if (*s == '/') { p.anchored = 1; s++; }
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '/') { p.dir_only = 1; s[len - 1] = '\0'; len--; }

    /* "basename only" if pattern has no '/' (and not anchored) — gitignore
     * spec: matches at any depth. */
    int has_slash = 0;
    for (size_t i = 0; i < len; i++) if (s[i] == '/') { has_slash = 1; break; }
    p.basename_only = (!p.anchored && !has_slash);

    p.pattern = lf_xstrdup(s);
    p.prefix  = prefix ? lf_xstrdup(prefix) : lf_xstrdup("");
    free(buf);

    push_pat(ig, p);
}

/* ---------- glob matcher ---------- */

/* Glob match: name vs pattern. Supports *, ?, **. Returns 1 on match.
 * '**' matches across '/' boundaries; '*' and '?' do not. */
static int glob_match(const char *pat, const char *name) {
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            /* '**' — skip following '/' if present */
            pat += 2;
            if (*pat == '/') pat++;
            if (*pat == '\0') return 1;
            for (const char *n = name; ; n++) {
                if (glob_match(pat, n)) return 1;
                if (*n == '\0') return 0;
            }
        } else if (*pat == '*') {
            pat++;
            if (*pat == '\0') {
                /* '*' at end: match anything except '/' */
                for (const char *n = name; *n; n++) if (*n == '/') return 0;
                return 1;
            }
            for (const char *n = name; ; n++) {
                if (glob_match(pat, n)) return 1;
                if (*n == '\0' || *n == '/') return 0;
            }
        } else if (*pat == '?') {
            if (*name == '\0' || *name == '/') return 0;
            pat++; name++;
        } else {
            if (*pat != *name) return 0;
            pat++; name++;
        }
    }
    return *name == '\0';
}

/* Test rel against pattern entry. Returns 1 on match. */
static int pat_match(const pat_t *p, const char *rel, const char *basename, int is_dir) {
    if (p->dir_only && !is_dir) return 0;

    /* Restrict to prefix scope. */
    size_t plen = strlen(p->prefix);
    if (plen > 0) {
        if (strncmp(rel, p->prefix, plen) != 0) return 0;
        if (rel[plen] != '\0' && rel[plen] != '/') return 0;
        rel = (rel[plen] == '/') ? rel + plen + 1 : rel + plen;
    }

    if (p->basename_only) {
        if (glob_match(p->pattern, basename)) return 1;
        /* Also: match any path component? gitignore: pattern with no slash
         * matches any file or directory along the way. We already test by
         * basename which covers the common case; for parent dir matches,
         * the walker tests dirs separately as it descends. */
        return 0;
    }
    /* Full path match against rel-from-prefix. */
    return glob_match(p->pattern, rel);
}

/* ---------- defaults + types ---------- */

static const char *DEFAULT_DIRS[] = {
    ".git", ".hg", ".svn", "node_modules", ".venv", "venv",
    "__pycache__", "dist", "build", "target", ".next", ".cache", NULL
};
static const char *DEFAULT_FILES[] = {
    ".DS_Store", NULL
};
static const char *BIN_EXTS[] = {
    ".exe",".dll",".so",".dylib",".a",".o",".obj",".pyc",".class",
    ".jar",".zip",".tar",".gz",".bz2",".xz",".7z",".rar",
    ".png",".jpg",".jpeg",".gif",".webp",".ico",".pdf",
    ".mp3",".mp4",".mov",".avi",".ttf",".otf",".woff",".woff2",
    ".bin",".iso",".dat",".db",".sqlite", NULL
};

int lf_ignore_is_binary_ext(const char *path) {
    for (int i = 0; BIN_EXTS[i]; i++) {
        if (lf_path_has_ext_ci(path, BIN_EXTS[i])) return 1;
    }
    return 0;
}

typedef struct {
    const char *name;
    const char *exts[8];   /* NULL-terminated */
} type_alias_t;

static const type_alias_t TYPE_ALIASES[] = {
    {"rust", {".rs", NULL}},
    {"py",   {".py", ".pyi", NULL}},
    {"ts",   {".ts", ".tsx", NULL}},
    {"js",   {".js", ".mjs", ".cjs", ".jsx", NULL}},
    {"c",    {".c", ".h", NULL}},
    {"cpp",  {".cpp", ".cc", ".cxx", ".hpp", ".hh", NULL}},
    {"go",   {".go", NULL}},
    {"md",   {".md", NULL}},
    {"json", {".json", NULL}},
    {"yaml", {".yaml", ".yml", NULL}},
    {"toml", {".toml", NULL}},
    {"sh",   {".sh", ".bash", NULL}},
    {NULL,   {NULL}}
};

int lf_ignore_typename_known(const char *name) {
    for (int i = 0; TYPE_ALIASES[i].name; i++) {
        if (strcmp(TYPE_ALIASES[i].name, name) == 0) return 1;
    }
    return 0;
}

static const type_alias_t *find_alias(const char *name) {
    for (int i = 0; TYPE_ALIASES[i].name; i++) {
        if (strcmp(TYPE_ALIASES[i].name, name) == 0) return &TYPE_ALIASES[i];
    }
    return NULL;
}

/* ---------- engine ---------- */

lf_ignore_t *lf_ignore_new(const grep_opts_t *o) {
    lf_ignore_t *ig = (lf_ignore_t *)lf_xcalloc(1, sizeof(*ig));
    ig->hidden    = o->hidden;
    ig->no_ignore = o->no_ignore;

    if (!o->no_ignore) {
        for (int i = 0; DEFAULT_DIRS[i]; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s/", DEFAULT_DIRS[i]);
            add_pattern(ig, buf, "", 0, 0);
        }
        for (int i = 0; DEFAULT_FILES[i]; i++) {
            add_pattern(ig, DEFAULT_FILES[i], "", 0, 0);
        }
    }

    /* User globs (always applied, win over gitignore). */
    for (size_t i = 0; i < o->n_globs; i++) {
        add_pattern(ig, o->globs[i], "", 1, 0);
    }

    /* Type filters. Each becomes an include glob 'star-star slash star.ext'. */
    if (o->n_types > 0) {
        ig->type_active = 1;
        for (size_t i = 0; i < o->n_types; i++) {
            const type_alias_t *a = find_alias(o->types[i]);
            if (!a) lf_die("unknown --type '%s'", o->types[i]);
            for (int e = 0; a->exts[e]; e++) {
                char buf[64];
                snprintf(buf, sizeof(buf), "**/*%s", a->exts[e]);
                add_pattern(ig, buf, "", 0, 1);
            }
        }
    }

    return ig;
}

void lf_ignore_free(lf_ignore_t *ig) {
    if (!ig) return;
    for (size_t i = 0; i < ig->n; i++) {
        free(ig->pats[i].pattern);
        free(ig->pats[i].prefix);
    }
    free(ig->pats);
    free(ig);
}

/* Read and parse a gitignore-style file. */
static void load_ignore_file(lf_ignore_t *ig, const char *abs_path, const char *prefix) {
    FILE *f = fopen(abs_path, "rb");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        add_pattern(ig, line, prefix, 0, 0);
    }
    fclose(f);
}

int lf_ignore_push_dir(lf_ignore_t *ig, const char *dir_abs, const char *rel_prefix) {
    int marker = (int)ig->n;
    if (ig->no_ignore) return marker;

    static const char *NAMES[] = {".gitignore", ".ignore", ".rgignore", NULL};
    for (int i = 0; NAMES[i]; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_abs, NAMES[i]);
        load_ignore_file(ig, path, rel_prefix);
    }
    return marker;
}

void lf_ignore_pop_dir(lf_ignore_t *ig, int marker) {
    while ((int)ig->n > marker) {
        ig->n--;
        free(ig->pats[ig->n].pattern);
        free(ig->pats[ig->n].prefix);
    }
}

/* Decide ignore status for a path. Walks patterns in reverse. The "first
 * deciding rule" is the first match; user globs always decide. */
static int decide(const lf_ignore_t *ig, const char *rel, const char *basename, int is_dir) {
    if (!ig->no_ignore) {
        if (!ig->hidden && basename[0] == '.' &&
            !(basename[1] == '\0' || (basename[1] == '.' && basename[2] == '\0'))) {
            return 1;
        }
    }

    /* User globs: positive globs (no '!') are include-only filters when ANY
     * positive glob is present. Negative '!' globs always exclude. */
    int has_pos_user = 0;
    int user_pos_match = 0;
    int user_neg_match = 0;
    for (size_t i = 0; i < ig->n; i++) {
        const pat_t *p = &ig->pats[i];
        if (!p->user_glob) continue;
        if (p->include) has_pos_user = 1;
        if (pat_match(p, rel, basename, is_dir)) {
            if (p->include) user_pos_match = 1;
            else            user_neg_match = 1;
        }
    }
    if (user_neg_match) return 1;
    if (has_pos_user && !user_pos_match && !is_dir) return 1;
    /* For dirs, positive include globs do not prune (we still need to descend). */

    /* Type filter: if active, files (not dirs) must match. */
    if (ig->type_active && !is_dir) {
        int matched = 0;
        for (size_t i = 0; i < ig->n; i++) {
            const pat_t *p = &ig->pats[i];
            if (!p->type_filter) continue;
            if (pat_match(p, rel, basename, is_dir)) { matched = 1; break; }
        }
        if (!matched) return 1;
    }

    /* gitignore + defaults: walk in reverse. */
    for (size_t k = ig->n; k > 0; k--) {
        const pat_t *p = &ig->pats[k - 1];
        if (p->user_glob || p->type_filter) continue;
        if (pat_match(p, rel, basename, is_dir)) {
            return p->negate ? 0 : 1;
        }
    }
    return 0;
}

int lf_ignore_match_file(const lf_ignore_t *ig, const char *rel, const char *basename) {
    return decide(ig, rel, basename, 0);
}
int lf_ignore_match_dir(const lf_ignore_t *ig, const char *rel, const char *basename) {
    return decide(ig, rel, basename, 1);
}
