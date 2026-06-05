#ifndef BENCH_H
#define BENCH_H

/*
 * bench.h — timing primitives for the HAlloc benchmark harness.
 *
 * Why CLOCK_MONOTONIC instead of gettimeofday():
 *
 *   gettimeofday() reads the wall-clock, which NTP is allowed to step
 *   or slew at any point.  A forward step mid-benchmark inflates the
 *   measured interval; a backward step produces a negative duration.
 *   CLOCK_MONOTONIC is guaranteed by POSIX to be strictly non-decreasing
 *   regardless of NTP adjustments — once a benchmark interval starts, the
 *   clock only moves forward, giving stable, reproducible measurements
 *   across the full run.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <time.h>
#include <stddef.h>

/* Returns elapsed time in nanoseconds as a double (avoids long overflow on
 * long benchmark intervals and makes per-op arithmetic cleaner). */
static inline double bench_ns(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec  - start.tv_sec)  * 1e9
         + (double)(end.tv_nsec - start.tv_nsec);
}

#define BENCH_START(t)  clock_gettime(CLOCK_MONOTONIC, &(t))
#define BENCH_END(t)    clock_gettime(CLOCK_MONOTONIC, &(t))

#endif /* BENCH_H */
