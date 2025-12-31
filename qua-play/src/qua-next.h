#ifndef QUA_NEXT_H
#define QUA_NEXT_H

#include <limits.h>
#include <stddef.h>

// Navigate to next/previous file in directory based on offset
// current_path: current file path
// offset: navigation offset (positive = forward, negative = backward)
// result: buffer to write result path to
// result_size: size of result buffer
// Returns 0 on success, 1 on failure
int qua_next(const char *current_path, int offset, char *result, size_t result_size);

#endif
