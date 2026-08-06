# RTEMS-KASAN

A working heap AddressSanitizer for RTEMS, running on `riscv/rv64imafdc` under
QEMU. Catches heap overflow, underflow, use-after-free and double-free, with
ASan-style reports naming the offending allocation.

```
=================================================================
ERROR: RTEMS-KASAN: heap-buffer-overflow on address 0x80070e00
WRITE of size 1 at 0x80070e00
0x80070e00 is located 0 bytes after a 16-byte region [0x80070df0,0x80070e00)
the region was allocated and is live
=================================================================
```

RTEMS ships no sanitizer support of any kind — `grep -r __asan_ cpukit bsps`
returns nothing — and there is no `libasan` for any RTEMS target. This is a
first cut at closing that gap.

## Status

| | |
|---|---|
| heap overflow / underflow | **working** |
| use-after-free | **working** |
| double free, invalid free | **working** |
| ragged (non-granule-multiple) allocations | **working** |
| stack instrumentation | not implemented |
| global instrumentation | not implemented |
| SMP safety | not implemented |
| BSPs other than `riscv/rv64imafdc` | untested, see [Porting](#porting-to-another-bsp) |

Demo result on `qemu-system-riscv64 -M virt`: 5 seeded bugs, 5 reports, and
the deliberately clean case reports nothing.

**Full RTEMS testsuite run: 671 tests, zero violations in RTEMS** — and two real
bugs found in this runtime. See [`RESULTS.md`](RESULTS.md).

## Why this approach

The obvious route — port `libsanitizer` — is a dead end. GCC's
`libsanitizer/configure.tgt` supports only `*-linux*`, `*-freebsd*`,
`*-solaris2.11*` and `*-darwin*`; the runtime wants `mmap` for shadow memory,
libc interceptors, a thread registry and a symbolizer, all of which assume a
hosted OS with virtual memory.

The route that works is the one Linux uses for KASAN: let the compiler emit
*calls*, and write a small runtime behind them.

```sh
-fsanitize=kernel-address
-fasan-shadow-offset=0
--param asan-instrumentation-with-call-threshold=0
--param asan-stack=0
--param asan-globals=0
```

The threshold of 0 forces every access out of line. That is the key: with
inline instrumentation GCC bakes `(addr >> 3) + offset` into the generated code
and you must supply a shadow region covering every address the program might
touch — 512 MB of it on a 32-bit target. Out of line, GCC emits only

```
0000000000000000 <load>:
   8:	auipc	ra,0x0
   c:	jalr	ra          # __asan_load4_noabort
  10:	lw	a0,0(s0)
```

and the runtime owns the mapping completely. So the shadow can cover just a
1 MB arena instead of the address space, and `-fasan-shadow-offset=0` is
inert — present only because GCC insists on a value.

The whole compiler-facing surface is 13 symbols: `__asan_load{1,2,4,8,16}_noabort`,
the matching stores, `__asan_loadN_noabort`, `__asan_storeN_noabort`, and
`__asan_handle_no_return`.

## How it works

- A static 1 MB arena with a 128 KB shadow, one shadow byte per 8-byte granule.
- `malloc` is intercepted with `-Wl,--wrap=malloc` and serves from the arena
  with a 16-byte red zone on each side. No RTEMS source changes.
- Shadow encoding follows upstream ASan so reports read familiarly: `0x00`
  fully addressable, `0x01`–`0x07` partially addressable (the ragged tail of a
  non-multiple-of-8 allocation), `0xfa` left red zone, `0xfb` right red zone,
  `0xfd` freed.
- Freed memory is poisoned and **never reused**, which is what makes
  use-after-free detection exact rather than probabilistic.
- Allocations too large for the arena fall through to the real `malloc` and are
  simply not checked; the count is reported so you can see it happening.

## Build and run

You need an installed RTEMS BSP — the build reads its `.pc` file, so the ABI
flags (`-mcmodel=medany` and friends) come from RTEMS rather than being
guessed.

```sh
# build and install the BSP
cd rtems
echo '[riscv/rv64imafdc]'      > config.ini
echo 'BUILD_TESTS = False'    >> config.ini
./waf configure --prefix=$HOME/rtems-bsp
./waf install

# build the demo
cd rtems-kasan
BSP_PREFIX=$HOME/rtems-bsp ./build.sh

# run it
qemu-system-riscv64 -M virt -m 512M -nographic -no-reboot -bios none \
    -kernel kasan-demo.exe
```

## Using it in your own application

Three things:

1. Compile **application** code with the flags above. Do not instrument the
   runtime — `rtems-kasan.c` touches shadow memory directly and instrumenting
   it would recurse.
2. Link with `-Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc`.
3. Call `rtems_kasan_init()` early, or just let the first `malloc` do it.

`rtems_kasan_report()` is weak. Override it to call `rtems_fatal()` if you want
the first violation to stop the system; the default prints and returns, which
matches upstream ASan's `noabort` behaviour and lets a test collect every bug in
one run.

### A trap for the unwary

At `-O2`, GCC removes `malloc`/`free` pairs whose memory is never meaningfully
used, and dead stores — *including the deliberately buggy ones in a test*. The
first version of the demo silently lost three of its six cases that way. The
build uses `-fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc
-fno-builtin-realloc`, and the test writes through `volatile` pointers. If you
write your own sanitizer tests, check the disassembly before believing a
negative result.

## Porting to another BSP

The requirement is that the target's GCC backend supports
`-fsanitize=kernel-address`. Verify before anything else:

```sh
echo 'int f(int*p){return *p;}' > t.c
<target>-gcc -fsanitize=kernel-address -fasan-shadow-offset=0 \
    --param asan-instrumentation-with-call-threshold=0 -c t.c -o t.o
<target>-nm t.o | grep __asan_
```

If `__asan_load4_noabort` appears, the target works and `build.sh` should need
nothing but a different `RTEMS_PC` and `CC`. If nothing appears, the backend
does not define `TARGET_ASAN_SHADOW_OFFSET` and instrumentation is silently
skipped — note that GCC **accepts the flag and warns, then does nothing**, so a
build can look successful and check nothing at all. `build.sh` fails loudly on
this rather than producing a uselessly "clean" run.

Checked in GCC 15.2.0:

| backend | `TARGET_ASAN_SHADOW_OFFSET` | so |
|---|---|---|
| aarch64, arm, i386, riscv, sparc | defined | should work |
| **microblaze** | **absent** | needs a GCC patch first |

MicroBlaze needs roughly what RISC-V has — a handful of lines:

```c
static unsigned HOST_WIDE_INT
riscv_asan_shadow_offset (void)
{
  return 0;
}
```

plus the `#undef`/`#define` pair. That is a small, upstreamable GCC change and
it would unblock MicroBlaze for this runtime unmodified. Not attempted here.

Note also the RISC-V comment: *"RV64 using dynamic shadow offset, and RV32 isn't
support yet"* — 32-bit is the hard case even for well-supported targets, which
is part of why `riscv/rv64imafdc` was chosen to start.

## Limitations

Read these before trusting it.

- **The allocator never reuses memory.** A long-running application will
  exhaust the arena and then quietly fall through to the real `malloc`,
  unchecked. This is a debugging allocator.
- **Heap only.** Stack and global overflows are invisible. Stack support needs
  `__asan_stack_malloc_*` and fake stacks and is materially harder; globals need
  `__asan_register_globals` plus linker-script sections. Both are plausible next
  steps.
- **Not SMP safe.** The bump pointer and shadow writes are unsynchronised.
- **Everything is out of line**, so it is slow — a call per memory access.
  Fine for tests, not for anything timing-sensitive.
- **Only the wrapped allocator is tracked.** Memory from `rtems_heap_allocate`,
  partitions, regions or the workspace is not covered.
- Tested on exactly one BSP, on QEMU, with one compiler.

## Files

| | |
|---|---|
| `src/rtems-kasan.h` | public interface and shadow encoding |
| `src/rtems-kasan.c` | the runtime: shadow, allocator, compiler entry points |
| `src/init.c` | demo with five seeded bugs and one clean case |
| `build.sh` | builds against an installed BSP via its `.pc` file |
| `RESULTS.md` | full testsuite comparison, baseline vs instrumented |
| `results/` | per-test verdicts for both runs |

## Contributing

```sh
git config core.hooksPath .githooks    # once, after cloning
```

`tools/check-local-paths.sh` refuses developer home directory paths, agent scratch
directories and the invoking username in tracked files. It runs as a pre-commit hook
if you enable the hooks path above, and in CI regardless. Generated artifacts -- build
logs, dejagnu `.sum` files, objdump output -- embed paths nobody typed, which is how
three of them reached public history in the sibling repository before there was a
check.

## Background

This came out of a MicroBlaze linker bug where an ASan-instrumented *linker*
found a relocation defect that had been silently miscompiling RTEMS at `-O2`:
<https://github.com/thesamprice/rtems-microblaze-linker-relaxation-bug>. That
work sanitized a host program. This is the other half — sanitizing target code
— and the two are unrelated in mechanism despite sharing a name.
