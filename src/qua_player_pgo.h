/**
 * @file QUAPLAYER_PGO_H
 * @brief Conditional Macros for GCC Profile-Guided Optimization (PGO) Control.
 *
 * Defines PGO_PROFILING_RESET() and PGO_PROFILING_FLUSH().
 *
 * If the 'PROFILING' macro is defined during compilation, these macros map
 * to the GCOV runtime functions (__gcov_reset and __gcov_flush) and link to libgcov.
 *
 * If 'PROFILING' is NOT defined, the macros compile to no-ops ((void)0),
 * removing all profiling overhead and dependencies for the final, optimized binary.
 */

#ifndef QUAPLAYER_PGO_H
#define QUAPLAYER_PGO_H
#ifdef PROFILING
extern void __gcov_reset(void);
extern void __gcov_dump(void);
#define PGO_PROFILING_RESET() \
    do { \
        __gcov_reset(); \
    } while (0)
#define PGO_PROFILING_FLUSH() \
    do { \
        __gcov_dump(); \
    } while (0)
#else
#define PGO_PROFILING_RESET() ((void)0)
#define PGO_PROFILING_FLUSH() ((void)0)
#endif
#endif // QUAPLAYER_PGO_H
