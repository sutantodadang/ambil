/*
 * binary.c — implementation of lf_is_binary.
 */
#include "binary.h"

#include <string.h>

#define BIN_PROBE 8192u

int lf_is_binary(const char *data, size_t len) {
    if (!data || len == 0) return 0;
    /* UTF-16 BOMs */
    if (len >= 2) {
        unsigned char a = (unsigned char)data[0], b = (unsigned char)data[1];
        if ((a == 0xFF && b == 0xFE) || (a == 0xFE && b == 0xFF)) return 1;
    }
    size_t n = len < BIN_PROBE ? len : BIN_PROBE;
    return memchr(data, 0, n) != NULL;
}
