/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Heap AddressSanitizer for RTEMS.
 *
 * Compile application code with
 *
 *   -fsanitize=kernel-address
 *   -fasan-shadow-offset=0
 *   --param asan-instrumentation-with-call-threshold=0
 *   --param asan-stack=0
 *   --param asan-globals=0
 *
 * and link with
 *
 *   -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc
 *
 * GCC then emits an out-of-line call to __asan_loadN_noabort /
 * __asan_storeN_noabort before every memory access, and this runtime decides
 * what to do about it.  Nothing about the shadow layout is baked into the
 * generated code, which is what makes a small target-specific runtime possible
 * at all -- see README.
 */

#ifndef RTEMS_KASAN_H
#define RTEMS_KASAN_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the sanitized heap.  Allocations larger than this obviously cannot
   be served; the wrapper falls back to the ordinary allocator for those and
   they are simply not checked. */
#ifndef KASAN_ARENA_SIZE
#define KASAN_ARENA_SIZE ( 1024u * 1024u )
#endif

/* Bytes of real memory described by one shadow byte.  Fixed at 8 by the
   shadow encoding; do not change without revisiting kasan_poison(). */
#define KASAN_GRANULE 8u

/* Red zone placed on each side of an allocation.  Must be a multiple of
   KASAN_ALIGN so that the payload keeps the alignment malloc promises. */
#ifndef KASAN_REDZONE
#define KASAN_REDZONE 16u
#endif

/*
 * Alignment guaranteed to callers.  malloc() must return memory aligned for
 * any type, which RTEMS states as CPU_HEAP_ALIGNMENT -- 16 on rv64imafdc,
 * and larger than KASAN_GRANULE on most 64-bit targets.  Getting this wrong
 * does not trip the sanitizer; it corrupts unrelated code that assumes the
 * guarantee, which is exactly how it was found here.
 */
#ifndef KASAN_ALIGN
#define KASAN_ALIGN 16u
#endif

/* Shadow byte values.  0..7 mean "the first N bytes of this granule are
   addressable"; the rest are poison and follow the upstream ASan encoding so
   that reports look familiar. */
#define KASAN_SHADOW_ADDRESSABLE 0x00
#define KASAN_SHADOW_LEFT_RZ     0xfa
#define KASAN_SHADOW_RIGHT_RZ    0xfb
#define KASAN_SHADOW_FREED       0xfd

/* Call once before any instrumented code runs.  Safe to call twice. */
void rtems_kasan_init( void );

/* Statistics, mostly so a test can assert something happened. */
typedef struct {
  uint64_t checks;          /* instrumented accesses seen           */
  uint64_t errors;          /* violations reported                  */
  uint64_t allocations;     /* served from the sanitized arena      */
  uint64_t frees;
  uint64_t passthrough;     /* too large for the arena, unchecked   */
  size_t   bytes_in_use;
  size_t   high_water;
} rtems_kasan_stats;

void rtems_kasan_get_stats( rtems_kasan_stats *stats );

/*
 * Called when a violation is detected.  The default implementation prints a
 * report with printk and returns, so that a test can continue and count
 * errors.  Override it to call rtems_fatal() instead.
 *
 * Returning from this function means execution continues into the bad access,
 * which is exactly what upstream ASan's "noabort" mode does.
 */
void rtems_kasan_report(
  uintptr_t   address,
  size_t      size,
  bool        is_write,
  const char *reason
);

#ifdef __cplusplus
}
#endif

#endif /* RTEMS_KASAN_H */
