/*
 * binary.h — heuristic binary file detection.
 *
 * Strategy: scan the first 8 KB for a NUL byte. NUL strongly indicates a
 * binary file (compiled object, image, archive, etc.). Also detect UTF-16
 * BOMs, which we choose not to decode.
 */
#ifndef AMBIL_BINARY_H
#define AMBIL_BINARY_H

#include <stddef.h>

/*
 * Return 1 if `data` (size `len`) looks binary, 0 otherwise.
 * Empty input returns 0.
 */
int lf_is_binary(const char *data, size_t len);

#endif /* AMBIL_BINARY_H */
