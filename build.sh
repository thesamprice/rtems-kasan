#!/bin/sh
#
# Build the RTEMS-KASAN demo.
#
#   BSP_PREFIX=/path/to/installed/bsp ./build.sh
#
# BSP_PREFIX is the --prefix an RTEMS BSP was installed to; it must contain
# lib/pkgconfig/<arch>-rtems7-<bsp>.pc.  Defaults target riscv/rv64imafdc.
set -e

: "${RTEMS_PREFIX:=$HOME/rtems/7}"
: "${BSP_PREFIX:?set BSP_PREFIX to an installed RTEMS BSP prefix}"
: "${RTEMS_PC:=riscv-rtems7-rv64imafdc}"
: "${CC:=$RTEMS_PREFIX/bin/riscv-rtems7-gcc}"

export PKG_CONFIG_PATH="$BSP_PREFIX/lib/pkgconfig"
PC="pkg-config --define-variable=prefix=$BSP_PREFIX"
CFLAGS_BSP=$($PC --cflags "$RTEMS_PC")
LDFLAGS_BSP=$($PC --libs "$RTEMS_PC")

# GCC still prints "not supported for this target" for -fsanitize=kernel-address
# on some targets even when it instruments correctly; check the object instead.
KASAN_FLAGS="-fsanitize=kernel-address \
             -fasan-shadow-offset=0 \
             --param asan-instrumentation-with-call-threshold=0 \
             --param asan-stack=0 \
             --param asan-globals=0"

# Without these, -O2 removes malloc/free pairs whose memory is "unused" --
# including the deliberately buggy accesses this demo is made of.
NOBUILTIN="-fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc -fno-builtin-realloc"

WRAP="-Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc"

echo "CC       : $CC"
echo "BSP      : $RTEMS_PC"
echo "cflags   : $CFLAGS_BSP"

# The runtime must NOT be instrumented -- it touches the shadow directly.
$CC $CFLAGS_BSP -Isrc -O2 -g -Wall -Wextra -c src/rtems-kasan.c -o rtems-kasan.o

# Application code is instrumented.
$CC $CFLAGS_BSP -Isrc -O2 -g -Wall $KASAN_FLAGS $NOBUILTIN -c src/init.c -o init.o

if ! $RTEMS_PREFIX/bin/riscv-rtems7-nm init.o | grep -q "__asan_"; then
  echo "ERROR: no instrumentation was emitted -- this target does not support" >&2
  echo "       -fsanitize=kernel-address.  See README, 'Porting to another BSP'." >&2
  exit 1
fi

$CC $LDFLAGS_BSP init.o rtems-kasan.o $WRAP -o kasan-demo.exe

echo "built kasan-demo.exe"
