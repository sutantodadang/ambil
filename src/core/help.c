/*
 * help.c — shared --help formatting primitives.
 *
 * Every subcommand's print_help() function builds its output from these
 * primitives to ensure consistent layout and spacing.
 */
#include "help.h"
#include <stdarg.h>
#include <string.h>

void lf_help_usage(FILE *out, const char *progname, const char *subcmd,
                   const char *synopsis)
{
    fprintf(out, "Usage: %s", progname);
    if (subcmd)
        fprintf(out, " --cmd %s", subcmd);
    fprintf(out, " %s\n", synopsis);
}

void lf_help_flag(FILE *out, const char *flag_text, const char *desc)
{
    fputs("  ", out);
    fputs(flag_text, out);
    if (desc && *desc) {
        int len = (int)strlen(flag_text);
        int pad = len < 28 ? 28 - len : 2;
        fprintf(out, "%*s%s", pad, "", desc);
    }
    fputc('\n', out);
}

void lf_help_section(FILE *out, const char *title)
{
    fprintf(out, "%s:\n", title);
}

void lf_help_spacer(FILE *out)
{
    fputc('\n', out);
}

void lf_help_desc(FILE *out, const char *fmt, ...)
{
    va_list ap;
    fprintf(out, "  ");
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}
