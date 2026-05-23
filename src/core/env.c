#include "env.h"
#include "ambil.h"
#include "util.h"
#include "json_emit.h"
#include <stdlib.h>
#include <string.h>

static const char *sc_names[] = {
    "grep", "cat", "ls", "find", "wc", "file", "env", "help"
};
static const int sc_count = (int)(sizeof(sc_names) / sizeof(sc_names[0]));

static void emit_env_json(lf_buf_t *b)
{
    lf_json_begin_object(b);
    lf_json_key(b, "type");
    lf_json_string(b, "env", 3);
    lf_json_key(b, "version");
    lf_json_string(b, AMBIL_VERSION, strlen(AMBIL_VERSION));
    lf_json_key(b, "platform");
#ifdef _WIN32
    lf_json_string(b, "windows", 7);
#elif defined(__APPLE__)
    lf_json_string(b, "macos", 5);
#else
    lf_json_string(b, "linux", 5);
#endif
    lf_json_key(b, "arch");
#if defined(__x86_64__) || defined(_M_X64)
    lf_json_string(b, "x86_64", 6);
#elif defined(__aarch64__) || defined(_M_ARM64)
    lf_json_string(b, "aarch64", 7);
#else
    lf_json_string(b, "unknown", 7);
#endif
    lf_json_key(b, "num_cpus");
    lf_json_uint(b, (unsigned long long)lf_detect_cpus());
    lf_json_key(b, "commands");
    lf_json_begin_array(b);
    for (int i = 0; i < sc_count; i++) {
        lf_json_string(b, sc_names[i], strlen(sc_names[i]));
    }
    lf_json_end_array(b);
    lf_json_end_object(b);
}

void lf_env_print_json(FILE *out)
{
    lf_buf_t buf;
    lf_buf_init(&buf);
    emit_env_json(&buf);
    fwrite(buf.data, 1, buf.len, out);
    fputc('\n', out);
    lf_buf_free(&buf);
}

void lf_env_print_text(FILE *out)
{
    fprintf(out, "ambil " AMBIL_VERSION "\n");
#ifdef _WIN32
    fprintf(out, "Platform:  windows");
#elif defined(__APPLE__)
    fprintf(out, "Platform:  macos");
#else
    fprintf(out, "Platform:  linux");
#endif
#if defined(__x86_64__) || defined(_M_X64)
    fprintf(out, " x86_64\n");
#elif defined(__aarch64__) || defined(_M_ARM64)
    fprintf(out, " aarch64\n");
#else
    fputc('\n', out);
#endif
    fprintf(out, "Threads:   %d\n", lf_detect_cpus());
    fputs("Commands:  ", out);
    for (int i = 0; i < sc_count; i++) {
        if (i > 0) fputs(", ", out);
        fputs(sc_names[i], out);
    }
    fputc('\n', out);
}
