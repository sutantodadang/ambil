/*
 * help.h — shared --help formatting primitives.
 */
#ifndef HELP_H
#define HELP_H

#include <stdio.h>

void lf_help_usage(FILE *out, const char *progname, const char *subcmd,
                   const char *synopsis);
void lf_help_flag(FILE *out, const char *flag_text, const char *desc);
void lf_help_section(FILE *out, const char *title);
void lf_help_spacer(FILE *out);
void lf_help_desc(FILE *out, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#endif /* HELP_H */
