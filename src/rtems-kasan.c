/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Heap AddressSanitizer for RTEMS -- runtime.
 *
 * MUST NOT be compiled with -fsanitize=kernel-address.  Instrumenting this
 * file would make every shadow access recurse into the checker.
 */

#include "rtems-kasan.h"

#include <string.h>
#include <rtems/bspIo.h>

/* ------------------------------------------------------------------ state */

static uint8_t kasan_arena[ KASAN_ARENA_SIZE ]
  __attribute__( ( aligned( KASAN_GRANULE ) ) );

static uint8_t kasan_shadow[ KASAN_ARENA_SIZE / KASAN_GRANULE ];

static size_t            kasan_brk;
static bool              kasan_ready;
static rtems_kasan_stats kasan_stats;

/*
 * Every allocation carries a header sitting inside its left red zone, so a
 * report can say how big the object was and whether it had been freed.
 */
typedef struct {
  uint32_t magic;
  uint32_t freed;
  size_t   size;      /* as requested by the caller */
} kasan_header;

#define KASAN_MAGIC 0x4b41534eu /* "KASN" */

/* ----------------------------------------------------------------- shadow */

static inline bool kasan_in_arena( uintptr_t addr )
{
  return addr >= (uintptr_t) kasan_arena
         && addr < (uintptr_t) kasan_arena + KASAN_ARENA_SIZE;
}

static inline uint8_t *kasan_shadow_of( uintptr_t addr )
{
  return &kasan_shadow[ ( addr - (uintptr_t) kasan_arena ) / KASAN_GRANULE ];
}

/* Mark [addr, addr+size) with VALUE.  addr must be granule aligned. */
static void kasan_poison( uintptr_t addr, size_t size, uint8_t value )
{
  size_t granules = ( size + KASAN_GRANULE - 1u ) / KASAN_GRANULE;

  memset( kasan_shadow_of( addr ), value, granules );
}

/* Mark [addr, addr+size) addressable, encoding the ragged tail. */
static void kasan_unpoison( uintptr_t addr, size_t size )
{
  size_t   whole = size / KASAN_GRANULE;
  uint8_t  tail  = (uint8_t) ( size % KASAN_GRANULE );
  uint8_t *shadow = kasan_shadow_of( addr );

  memset( shadow, KASAN_SHADOW_ADDRESSABLE, whole );

  if ( tail != 0u ) {
    shadow[ whole ] = tail;
  }
}

/* True if the single byte at ADDR may be accessed. */
static bool kasan_byte_ok( uintptr_t addr, uint8_t *bad )
{
  uint8_t shadow = *kasan_shadow_of( addr );
  uint8_t offset;

  if ( shadow == KASAN_SHADOW_ADDRESSABLE ) {
    return true;
  }

  offset = (uint8_t) ( addr % KASAN_GRANULE );

  /* 1..7: the first `shadow` bytes of this granule are addressable. */
  if ( shadow < KASAN_GRANULE && offset < shadow ) {
    return true;
  }

  *bad = shadow;
  return false;
}

static const char *kasan_reason( uint8_t shadow )
{
  switch ( shadow ) {
    case KASAN_SHADOW_LEFT_RZ:
      return "heap-buffer-underflow";
    case KASAN_SHADOW_RIGHT_RZ:
      return "heap-buffer-overflow";
    case KASAN_SHADOW_FREED:
      return "heap-use-after-free";
    default:
      return "heap-buffer-overflow";
  }
}

/* ------------------------------------------------------------------ check */

static void kasan_check( uintptr_t addr, size_t size, bool is_write )
{
  uint8_t bad = 0;
  size_t  i;

  if ( !kasan_ready || !kasan_in_arena( addr ) ) {
    return;
  }

  kasan_stats.checks++;

  /* Check first and last byte, then the interior granule starts.  This is
     what upstream ASan does and it is exact for any access that does not
     straddle a poisoned granule entirely, which cannot happen for accesses
     of 16 bytes or fewer. */
  for ( i = 0; i < size; ++i ) {
    if ( !kasan_byte_ok( addr + i, &bad ) ) {
      kasan_stats.errors++;
      rtems_kasan_report( addr + i, size, is_write, kasan_reason( bad ) );
      return;
    }
  }
}

/* -------------------------------------------------------------- allocator */

/*
 * Bump allocator.  Freed memory is poisoned and never reused, which is how
 * use-after-free is caught with certainty.  That makes this a debugging
 * allocator, not a production one -- see README.
 */

static void *kasan_alloc( size_t size )
{
  size_t        need;
  uintptr_t     base;
  uintptr_t     user;
  kasan_header *header;

  if ( size == 0u ) {
    size = 1u;
  }

  /* left red zone (holding the header) + payload rounded up + right red zone */
  need = KASAN_REDZONE
         + ( ( size + KASAN_GRANULE - 1u ) & ~( (size_t) KASAN_GRANULE - 1u ) )
         + KASAN_REDZONE;

  if ( kasan_brk + need > KASAN_ARENA_SIZE ) {
    return NULL; /* caller falls back to the ordinary allocator */
  }

  base = (uintptr_t) kasan_arena + kasan_brk;
  user = base + KASAN_REDZONE;
  kasan_brk += need;

  kasan_poison( base, need, KASAN_SHADOW_RIGHT_RZ );
  kasan_poison( base, KASAN_REDZONE, KASAN_SHADOW_LEFT_RZ );
  kasan_unpoison( user, size );

  header         = (kasan_header *) base;
  header->magic  = KASAN_MAGIC;
  header->freed  = 0u;
  header->size   = size;

  kasan_stats.allocations++;
  kasan_stats.bytes_in_use += size;

  if ( kasan_stats.bytes_in_use > kasan_stats.high_water ) {
    kasan_stats.high_water = kasan_stats.bytes_in_use;
  }

  return (void *) user;
}

static bool kasan_owns( const void *p )
{
  return p != NULL && kasan_in_arena( (uintptr_t) p );
}

static kasan_header *kasan_header_of( void *p )
{
  kasan_header *header = (kasan_header *) ( (uintptr_t) p - KASAN_REDZONE );

  return header->magic == KASAN_MAGIC ? header : NULL;
}

/* ------------------------------------------------------------- public API */

void rtems_kasan_init( void )
{
  if ( kasan_ready ) {
    return;
  }

  /* Everything starts poisoned; only live allocations get unpoisoned. */
  memset( kasan_shadow, KASAN_SHADOW_LEFT_RZ, sizeof( kasan_shadow ) );
  kasan_brk   = 0;
  kasan_ready = true;
}

void rtems_kasan_get_stats( rtems_kasan_stats *stats )
{
  *stats = kasan_stats;
}

__attribute__( ( weak ) ) void rtems_kasan_report(
  uintptr_t   address,
  size_t      size,
  bool        is_write,
  const char *reason
)
{
  kasan_header *header = NULL;
  uintptr_t     probe;

  printk( "\n=================================================================\n" );
  printk( "ERROR: RTEMS-KASAN: %s on address %p\n", reason, (void *) address );
  if ( size != 0u ) {
    printk(
      "%s of size %u at %p\n",
      is_write ? "WRITE" : "READ",
      (unsigned) size,
      (void *) address
    );
  }

  /* Walk back to the owning allocation header, if there is one. */
  for ( probe = address & ~( (uintptr_t) KASAN_GRANULE - 1u );
        probe >= (uintptr_t) kasan_arena;
        probe -= KASAN_GRANULE ) {
    kasan_header *candidate = (kasan_header *) probe;

    if ( candidate->magic == KASAN_MAGIC ) {
      header = candidate;
      break;
    }
  }

  if ( header != NULL ) {
    uintptr_t user = (uintptr_t) header + KASAN_REDZONE;

    printk(
      "%p is located %d bytes %s a %u-byte region [%p,%p)\n",
      (void *) address,
      address >= user + header->size
        ? (int) ( address - ( user + header->size ) )
        : (int) ( user - address ),
      address >= user + header->size ? "after" : "before",
      (unsigned) header->size,
      (void *) user,
      (void *) ( user + header->size )
    );
    printk( "the region was %s\n", header->freed ? "FREED" : "allocated and is live" );
  }

  printk( "=================================================================\n" );
}

/* ------------------------------------------------------- malloc interposn */

extern void *__real_malloc( size_t size );
extern void  __real_free( void *p );
extern void *__real_calloc( size_t n, size_t size );
extern void *__real_realloc( void *p, size_t size );

void *__wrap_malloc( size_t size )
{
  void *p;

  rtems_kasan_init();

  p = kasan_alloc( size );

  if ( p != NULL ) {
    return p;
  }

  kasan_stats.passthrough++;
  return __real_malloc( size );
}

void __wrap_free( void *p )
{
  kasan_header *header;

  if ( !kasan_owns( p ) ) {
    __real_free( p );
    return;
  }

  header = kasan_header_of( p );

  if ( header == NULL ) {
    kasan_stats.errors++;
    rtems_kasan_report( (uintptr_t) p, 0, false, "invalid-free" );
    return;
  }

  if ( header->freed ) {
    kasan_stats.errors++;
    rtems_kasan_report( (uintptr_t) p, 0, false, "double-free" );
    return;
  }

  header->freed = 1u;
  kasan_stats.frees++;
  kasan_stats.bytes_in_use -= header->size;

  /* Poison the payload; the memory is never handed out again. */
  kasan_poison( (uintptr_t) p, header->size, KASAN_SHADOW_FREED );
}

void *__wrap_calloc( size_t n, size_t size )
{
  size_t total = n * size;
  void  *p     = __wrap_malloc( total );

  if ( p != NULL ) {
    memset( p, 0, total );
  }

  return p;
}

void *__wrap_realloc( void *p, size_t size )
{
  void         *fresh;
  kasan_header *header;

  if ( p == NULL ) {
    return __wrap_malloc( size );
  }

  if ( !kasan_owns( p ) ) {
    return __real_realloc( p, size );
  }

  header = kasan_header_of( p );
  fresh  = __wrap_malloc( size );

  if ( fresh != NULL && header != NULL ) {
    memcpy( fresh, p, header->size < size ? header->size : size );
    __wrap_free( p );
  }

  return fresh;
}

/* ------------------------------------------------ compiler-called entries */

/*
 * GCC emits calls to these before each access when
 * --param asan-instrumentation-with-call-threshold=0 is in effect.
 */

#define KASAN_ENTRY( n )                                            \
  void __asan_load##n##_noabort( uintptr_t addr );                  \
  void __asan_load##n##_noabort( uintptr_t addr )                   \
  {                                                                 \
    kasan_check( addr, n, false );                                  \
  }                                                                 \
  void __asan_store##n##_noabort( uintptr_t addr );                 \
  void __asan_store##n##_noabort( uintptr_t addr )                  \
  {                                                                 \
    kasan_check( addr, n, true );                                   \
  }

KASAN_ENTRY( 1 )
KASAN_ENTRY( 2 )
KASAN_ENTRY( 4 )
KASAN_ENTRY( 8 )
KASAN_ENTRY( 16 )

void __asan_loadN_noabort( uintptr_t addr, size_t size );
void __asan_loadN_noabort( uintptr_t addr, size_t size )
{
  kasan_check( addr, size, false );
}

void __asan_storeN_noabort( uintptr_t addr, size_t size );
void __asan_storeN_noabort( uintptr_t addr, size_t size )
{
  kasan_check( addr, size, true );
}

/* Emitted at points the compiler knows control will not come back, e.g.
   before longjmp.  With stack instrumentation disabled there is nothing to
   unpoison, but the symbol must exist. */
void __asan_handle_no_return( void );
void __asan_handle_no_return( void )
{
}
