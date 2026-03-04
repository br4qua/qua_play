// debug.h
#ifndef DEBUG_H
#define DEBUG_H
#include <stdio.h>

#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define DEBUG_CHECK(condition, fmt, ...)         \
    do                                           \
    {                                            \
        if (condition)                           \
        {                                        \
            fprintf(stderr, fmt, ##__VA_ARGS__); \
        }                                        \
    } while (0)
#define CHECK_READ(val, expected, msg)          \
    if (unlikely((val) != (ssize_t)(expected))) \
    {                                           \
        fprintf(stderr, msg "\n");              \
        return -1;                              \
    }
#else
#define DEBUG_PRINT(fmt, ...) ((void)0)
#define DEBUG_CHECK(condition, fmt, ...) ((void)0)
// CHECK_READ should always check, but only print in debug mode
#define CHECK_READ(val, expected, msg)          \
    if (unlikely((val) != (ssize_t)(expected))) \
    {                                           \
        return -1;                              \
    }
#endif

#endif
