/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Demonstration for the RTEMS heap AddressSanitizer.
 *
 * Compiled WITH instrumentation.  Each case provokes one class of bug and the
 * runtime should report it; the clean cases must produce no report at all.
 */

#include "rtems-kasan.h"

#include <stdlib.h>
#include <string.h>
#include <rtems.h>
#include <rtems/bspIo.h>

/* Keep the optimiser from proving these accesses away. */
static volatile int sink;

static void banner( const char *name )
{
  printk( "\n--- %s ---\n", name );
}

static void case_clean( void )
{
  char *p;
  int   i;

  banner( "clean: in-bounds access, no report expected" );

  p = malloc( 32 );

  for ( i = 0; i < 32; ++i ) {
    p[ i ] = (char) i;
  }

  for ( i = 0; i < 32; ++i ) {
    sink = p[ i ];
  }

  free( p );
}

static void case_overflow( void )
{
  char *p;

  banner( "heap-buffer-overflow: write one past a 16-byte object" );

  p = malloc( 16 );
  ( (volatile char *) p )[ 16 ] = 'x';   /* one byte past the end */
  free( p );
}

static void case_underflow( void )
{
  char *p;

  banner( "heap-buffer-underflow: read one before a 16-byte object" );

  p    = malloc( 16 );
  sink = p[ -1 ];
  free( p );
}

static void case_use_after_free( void )
{
  int *p;

  banner( "heap-use-after-free: read after free" );

  p = malloc( sizeof( *p ) * 4 );
  p[ 0 ] = 1;
  free( p );
  sink = p[ 0 ];          /* freed */
}

static void case_ragged_tail( void )
{
  char *p;

  banner( "ragged tail: 20-byte object, byte 20 is out of bounds" );

  p = malloc( 20 );
  ( (volatile char *) p )[ 19 ] = 'a';   /* fine */
  ( (volatile char *) p )[ 20 ] = 'b';   /* one past the end */
  free( p );
}

static void case_double_free( void )
{
  char *p;

  banner( "double-free" );

  p = malloc( 8 );
  free( p );
  free( p );
}

static void Init( rtems_task_argument arg )
{
  rtems_kasan_stats stats;

  (void) arg;

  printk( "\n*** BEGIN OF TEST RTEMS-KASAN ***\n" );

  rtems_kasan_init();

  case_clean();
  case_overflow();
  case_underflow();
  case_use_after_free();
  case_ragged_tail();
  case_double_free();

  rtems_kasan_get_stats( &stats );

  printk( "\n--- summary ---\n" );
  printk( "instrumented accesses checked : %u\n", (unsigned) stats.checks );
  printk( "violations reported           : %u\n", (unsigned) stats.errors );
  printk( "sanitized allocations         : %u\n", (unsigned) stats.allocations );
  printk( "frees                         : %u\n", (unsigned) stats.frees );
  printk( "passed through (too large)    : %u\n", (unsigned) stats.passthrough );
  printk( "high water                    : %u bytes\n", (unsigned) stats.high_water );

  /* overflow, underflow, use-after-free, ragged tail, double free.
     The clean case must contribute nothing. */
  if ( stats.errors == 5u ) {
    printk( "\n*** END OF TEST RTEMS-KASAN ***\n" );
  } else {
    printk( "\nFAILED: expected 5 reports, got %u\n", (unsigned) stats.errors );
  }

  rtems_shutdown_executive( 0 );
}

#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 4
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_STACK_SIZE ( 32u * 1024u )
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
