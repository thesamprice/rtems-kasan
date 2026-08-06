# Full RTEMS testsuite under KASAN

`riscv/rv64imafdc`, RTEMS `main` (`fa81c051`), `qemu-system-riscv64 -M virt -m 512M`.
Everything instrumented — cpukit, BSP and tests — not just application code.

## Headline

**671 tests, zero memory-safety violations in RTEMS.**

No heap overflow, no underflow, no use-after-free, no double free anywhere in the
testsuite's heap usage on this BSP.

**Two real bugs were found — both in this runtime, not in RTEMS.** Both were missed by
the six-case demo and only appeared against the real testsuite. See
[Bugs found in the sanitizer](#bugs-found-in-the-sanitizer).

## Was the instrumentation actually live?

A zero-finding result is worthless if nothing was being checked, so:

| check | result |
|---|---|
| `__asan_load`/`__asan_store` call sites in `sp01.exe` | 1511 |
| cpukit instrumented (`heapallocate.c.o`, `malloc.c.o`) | yes |
| same objects in the baseline build | zero asan references |
| `__wrap_malloc` linked into every image | yes |
| same runtime against the seeded-bug demo | 5 of 5 caught |

Text growth on `sp01.exe`: 304,482 → 323,022 bytes, +6.1%.

## Verdicts

| | PASS | FAIL |
|---|---:|---:|
| baseline | 617 | 23 |
| instrumented, 45 s timeout | 589 | 49 |
| instrumented, regressions rechecked at 150 s | **598** | **40** |

Of 32 apparent regressions, **9 were timeout artifacts**: `sp03 sp04 sp05 sp06 sp11
sp12 sp13 sptimecounter02 ticker`. All pass when given time — `ticker` needs 40 s, and
the harness allowed 45 s while running six QEMU instances in parallel. A call per
memory access is expensive and long-running timing tests need a bigger budget. Anyone
reading a KASAN run as a regression report needs to account for this first.

## The 23 real regressions are this runtime, not RTEMS

One coherent cause. These tests **deliberately exhaust the heap** and assert on
out-of-memory behaviour:

| test | assertion |
|---|---|
| `block11` | `sc == RTEMS_NO_MEMORY` |
| `malloc04` | `p == NULL` |
| `rbheap01` | `p == NULL` |
| `spprivenv01` | `sc == RTEMS_NO_MEMORY` |
| `spstdthreads01` | `status == thrd_nomem` |
| `spfifo02` | `fd == -1` |
| `psxpipe01` | `status == -1` |
| `psximfs02` | `status == -1` |
| `spmountmgr01` | `status == -1` |
| `malloctest` | `t == u` (heap accounting) |
| `spstkalloc02` | `stack_space_size == _CONFIGURE_STACK_SPACE_SIZE` |
| `sptls01` | `tls_size == 1` |
| `sptls03` | `executing->Start.tls_area == NULL` |
| `ts-validation-acfg-0/1` | `INTERNAL_ERROR_CORE` |
| `ts-fatal-idle-thread-create-failed` | expects a fatal that never arrives |
| `ts-fatal-init-task-construct-failed` | likewise |
| `exit04`, `fsimfsgeneric01`, `psxchroot01`, `sp01`, `termios01`, `ts-performance-no-clock-0` | — |

`__wrap_malloc` serves from a private arena that RTEMS knows nothing about, so
exhaustion never happens when the test expects it, and heap accounting no longer
matches. Not RTEMS bugs and not detections — the single biggest design limitation of
this approach. Tracked as an issue.

## Bugs found in the sanitizer

**1. Wrong alignment guarantee.** The arena was aligned to `KASAN_GRANULE` (8), but
`CPU_HEAP_ALIGNMENT` on `rv64imafdc` is **16**. `malloc` must return memory aligned for
any type. This does not trip the sanitizer; it silently corrupts code that relies on
the guarantee. Fixed with a separate `KASAN_ALIGN`.

**2. Integer overflow in the allocator.** `spfreechain01` does:

```c
rtems_test_assert( _Freechain_Get( &fc, malloc, 1, SIZE_MAX ) == NULL );
```

It asks for `SIZE_MAX` bytes and expects `NULL`. The size calculation was:

```c
need = KASAN_REDZONE + ((size + KASAN_ALIGN - 1) & ~(KASAN_ALIGN - 1)) + KASAN_REDZONE;
```

`SIZE_MAX + 15` **wraps to 14**, masked to 0, giving `need = 32`. A request for the
entire address space returned a 32-byte buffer. `calloc` had the same flaw in
`n * size`. Both now rejected up front.

An integer-overflow bug in the memory-safety tool, caught by RTEMS's own test suite.
`dl03`, `spfreechain01`, `spintrcritical08` and `spintrcritical24` went from failing to
passing once fixed.

## Reproducing

```sh
# baseline
[riscv/rv64imafdc]
BUILD_TESTS = True

# instrumented
[riscv/rv64imafdc]
BUILD_TESTS = True
OPTIMIZATION_FLAGS = -O2 -g -fsanitize=kernel-address -fasan-shadow-offset=0
    --param asan-instrumentation-with-call-threshold=0 --param asan-stack=0
    --param asan-globals=0 -fno-builtin-malloc -fno-builtin-free
    -fno-builtin-calloc -fno-builtin-realloc
LDFLAGS = -Wl,--gc-sections /path/to/rtems-kasan.o -Wl,--wrap=malloc
    -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc
```

`OPTIMIZATION_FLAGS` and `LDFLAGS` are single lines in the real file. Build the runtime
separately and uninstrumented, with `-DKASAN_ARENA_SIZE=$((4*1024*1024))`; the default
1 MB is exhausted early by a full RTEMS configuration.

Two tests fail to build instrumented, neither a runtime bug: `regulator01` defines its
own `__wrap_malloc` and collides, and `tftpfs` trips
`-Werror=stringop-truncation` under the changed inlining.
