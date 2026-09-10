---
title: Vectorization and SIMD
part: Part III - The Machine
summary: Getting one instruction to do eight comparisons: what the auto-vectoriser needs, how to check it worked, hand-written AVX2 and SWAR, and when wider registers buy you nothing.
time: 35 min
level: expert
tags: simd, avx2, autovectorization, swar, restrict
---

Every core you own contains a second, wider set of registers that most C++ programs never
touch. A 256-bit AVX2 register holds eight 32-bit integers, and one `vpcmpgtd` compares all
eight against a target in a single instruction with the same latency as comparing one. When
your work is a loop over an array of prices, that is a factor of eight sitting on the table.

The catch is that the compiler will only generate those instructions when it can prove a
list of things about your loop, and it fails silently when it cannot.

## What SIMD is, and how wide

SIMD is Single Instruction, Multiple Data: one instruction applied lane-wise to a vector
register. The x86 lineage, in order:

| Extension | Register | Width | Lanes of `int32_t` | Typical availability |
|---|---|---|---|---|
| SSE2 | `xmm0`-`xmm15` | 128 bit | 4 | every x86-64 CPU |
| AVX / AVX2 | `ymm0`-`ymm15` | 256 bit | 8 | Haswell (2013) onward |
| AVX-512 | `zmm0`-`zmm31` | 512 bit | 16 | Skylake-SP and later server parts, patchy on desktop |

AVX-512 also brings 32 registers instead of 16, and **mask registers** `k0`-`k7` that let
every instruction be predicated per lane. Masking is the genuinely useful part: it makes
conditional work inside a vectorised loop cheap, which AVX2 handles clumsily with blends.

:::key
SIMD multiplies **throughput**, never latency. One `vpaddd` adding eight lanes has roughly the
same latency as one scalar `add` — around one cycle. If you have one price to compare, SIMD
does nothing for you. If you have a hundred, it can do them in thirteen instructions instead
of a hundred. Tick-to-trade paths that touch one message benefit rarely; batch work like book
rebuilds, risk sweeps, and checksum validation benefits a lot.
:::

## What the compiler needs before it will vectorise

GCC and Clang both auto-vectorise at `-O3` (and GCC at `-O2` since GCC 12, at a less
aggressive setting). A loop qualifies only if all of the following hold.

1. **Contiguous, unit-stride access.** `a[i]` is fine. `a[i * 3]` needs a gather, which may
   still vectorise but usually is not worth it. `a[idx[i]]` is a gather and is often slower
   than scalar.
2. **A trip count known before the loop starts.** `for (i = 0; i < n; ++i)` qualifies even
   though `n` is a run-time value, because `n` is fixed on entry. A `while` loop whose
   condition depends on data read inside the body does not.
3. **No early exit.** A `break`, a `return`, or a thrown exception inside the body means the
   compiler cannot execute eight iterations speculatively. Search loops do not auto-vectorise.
4. **No loop-carried dependency.** Iteration `i` must not read what iteration `i-1` wrote.
   Reductions like `sum += a[i]` are a special case the compiler can handle for integers, and
   cannot for floating point without permission, which is the next section.
5. **No possible aliasing.** If the loop writes through `out` and reads through `in`, and both
   are `int32_t*`, the compiler must assume they may overlap. It often emits a run-time
   overlap check and two loop bodies, which costs code size; sometimes it just gives up.
6. **No calls.** Any call the compiler cannot see through kills it. Inlined functions are
   fine; so are the handful of maths functions with vector versions available.

Here is a loop that satisfies all six.

```cpp vectorizable.cpp
#include <cstddef>
#include <cstdint>

// Notional per level, in ticks. Contiguous, no aliasing possible between
// the two const inputs and the output because of __restrict below.
void notional_all(const std::int32_t* __restrict px,
                  const std::int32_t* __restrict qty,
                  std::int64_t* __restrict out,
                  std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::int64_t>(px[i]) * qty[i];
    }
}
```

## Did it actually vectorise?

Never assume. There are three ways to check, and you should know all three.

```sh GCC
$ g++ -std=c++23 -O3 -march=x86-64-v3 -fopt-info-vec vectorizable.cpp -c
vectorizable.cpp:9:26: optimized: loop vectorized using 32 byte vectors

$ g++ -std=c++23 -O3 -march=x86-64-v3 -fopt-info-vec-missed vectorizable.cpp -c
# every loop it declined, with the reason
```

```sh Clang
$ clang++ -std=c++23 -O3 -march=x86-64-v3 -Rpass=loop-vectorize vectorizable.cpp -c
remark: vectorized loop (vectorization width: 8, interleaved count: 4)

$ clang++ -std=c++23 -O3 -march=x86-64-v3 -Rpass-missed=loop-vectorize \
          -Rpass-analysis=loop-vectorize vectorizable.cpp -c
remark: loop not vectorized: cannot identify array bounds
```

The third way is to read the assembly, which is the only method that cannot lie to you.

```sh
$ g++ -std=c++23 -O3 -march=x86-64-v3 -S -masm=intel -o - vectorizable.cpp | grep -c ymm
```

Look for register names: `xmm` means 128-bit, `ymm` means 256-bit, `zmm` means 512-bit. Note
that `xmm` alone is not evidence of vectorisation — scalar `double` and `float` arithmetic also
uses `xmm` registers, one lane at a time. What you want to see is `ymm`/`zmm`, or `xmm` in an
instruction with a `p` for "packed" like `paddd` or `pmulld`.

:::pitfall
`-march=native` on your dev box and a different microarchitecture in production means the
vectorisation you verified locally may not exist on the machine that trades. Pin the target
explicitly, `-march=x86-64-v3` for an AVX2 baseline or `-march=icelake-server` for a specific
part, and check the assembly from the build that actually ships. Lesson 1 covers the illegal
instruction crash that this prevents.
:::

## Removing the compiler's doubts

### Aliasing: `__restrict`

`__restrict` on a pointer parameter is a promise that, for the lifetime of that pointer, the
memory it points to is not reached through any other pointer. It is not standard C++ — it is a
GCC, Clang and MSVC extension inherited from C99 — but it is universally available and it is
the single most effective vectorisation hint.

```cpp restrict.cpp
#include <cstdint>
#include <cstddef>

// Without __restrict the compiler must assume dst and src may overlap
// and emits a run-time check plus a scalar fallback loop.
void apply_tick_shift(std::int32_t* __restrict dst,
                      const std::int32_t* __restrict src,
                      std::size_t n, std::int32_t shift) noexcept {
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i] + shift;
}
```

:::warn
`__restrict` is a promise you can break silently. Pass overlapping ranges and the compiler is
entitled to produce a result that matches neither the overlapping nor the non-overlapping
interpretation. Use it on leaf functions whose callers you control, and never on a public API
where a caller might reasonably pass the same buffer twice.
:::

### Alignment: `std::assume_aligned`

A 256-bit load from an address that is not 32-byte aligned is legal and, on modern parts, only
slightly slower — except when it straddles a 64-byte cache line, where it costs an extra
access. More importantly, the compiler does not know your alignment, so it may emit a scalar
"peel" loop to reach an aligned boundary before the vector body starts.

```cpp aligned.cpp
#include <cstddef>
#include <cstdint>
#include <memory>

// alignas on the storage plus assume_aligned on the pointer: the compiler
// can now emit aligned loads and skip the peel loop.
alignas(64) static std::int32_t g_qty[4096];

std::int64_t total_qty(std::size_t n) noexcept {
    const std::int32_t* q = std::assume_aligned<64>(g_qty);
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += q[i];
    return sum;
}
```

Like `__restrict`, `std::assume_aligned` is a promise. A wrong one gives you a general
protection fault on an aligned load instruction, which at least fails loudly.

### Floating point associativity, and why `-ffast-math` is the wrong answer

This is the reduction that will not vectorise.

```cpp fp_reduction.cpp
#include <cstddef>

double sum_pnl(const double* p, std::size_t n) noexcept {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += p[i];   // stays scalar at -O3
    return s;
}
```

To vectorise this the compiler would keep four partial sums in one register and add them at
the end. That changes the order of the additions. Floating-point addition is **not
associative**: `(a + b) + c` and `a + (b + c)` can differ, because each addition rounds. So the
transformation changes the answer, and the compiler is not permitted to change the answer.

`-ffast-math` grants permission — and a great deal more. It is an umbrella that also enables
`-fno-signed-zeros`, `-freciprocal-math` (turning `x / y` into `x * (1/y)`), `-fno-math-errno`,
and critically `-ffinite-math-only`, which tells the compiler that no value is ever NaN or
infinity. The compiler then deletes your `std::isnan` checks as provably false.

:::hft
Do not put `-ffast-math` in a trading build. A feed carrying a NaN price, a divide producing an
infinity, a risk check comparing against a sentinel — all of these become undefined behaviour
under `-ffinite-math-only`, and the NaN guard you carefully wrote is silently removed. The
reproducibility cost is just as bad: a P&L number that differs between the risk engine and the
backtest because one was compiled with reassociation and the other was not is a very expensive
afternoon.

The correct answers are, in order of preference: use integer ticks so the question does not
arise, which is why this series stores prices as `std::int64_t`; or enable exactly the one
relaxation you need with `#pragma GCC ivdep`, `#pragma clang loop vectorize(assume_safety)`, or
per-function `[[gnu::optimize("-fassociative-math", "-fno-trapping-math")]]`; or write the
multi-accumulator reduction by hand, so the summation order is explicit in the source and
identical everywhere.
:::

```cpp manual_accumulators.cpp
#include <cstddef>

// Four independent accumulators break the dependency chain and let the
// compiler vectorise without any relaxation of IEEE semantics, because
// the summation order is now what the source literally says.
double sum_pnl_4(const double* p, std::size_t n) noexcept {
    double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) { a += p[i]; b += p[i+1]; c += p[i+2]; d += p[i+3]; }
    double s = (a + b) + (c + d);
    for (; i < n; ++i) s += p[i];
    return s;
}
```

## Doing it by hand: AVX2 and SWAR

When the auto-vectoriser refuses — and for search loops it always refuses, because of the
early exit — you write the intrinsics yourself.

### Eight price levels at once

A compressed depth ladder stores each level's price as a 32-bit offset from a base tick price,
so eight levels fit in one `ymm` register. The task is to find the first level at or above a
target, which is a search loop with an early exit and therefore invisible to the vectoriser.

```cpp first_ge_avx2.cpp
#include <bit>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>

// Levels ascending, stored as int32 offsets from the book's base tick price.
// Returns the index of the first level >= target, or n if there is none.
// Precondition: target > INT32_MIN, so target - 1 does not overflow.
std::size_t first_ge_avx2(const std::int32_t* off, std::size_t n,
                          std::int32_t target) noexcept {
    // Broadcast target-1 into all eight lanes. AVX2 has a signed
    // greater-than but no greater-or-equal, so ">= t" becomes "> t-1".
    const __m256i t = _mm256_set1_epi32(target - 1);

    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        // Unaligned 256-bit load of eight offsets.
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(off + i));
        // Lane-wise signed compare: each lane becomes all-ones or all-zeros.
        const __m256i gt = _mm256_cmpgt_epi32(v, t);
        // Collapse to an 8-bit mask, one bit per lane, taken from each lane's
        // sign bit. The float cast is free; it just picks the right movemask.
        const int mask = _mm256_movemask_ps(_mm256_castsi256_ps(gt));
        if (mask != 0) {
            // Lowest set bit is the lowest matching lane. countr_zero is tzcnt.
            return i + static_cast<std::size_t>(std::countr_zero(static_cast<unsigned>(mask)));
        }
    }
    // Scalar tail: n is rarely a multiple of eight, and this is not optional.
    for (; i < n; ++i) if (off[i] >= target) return i;
    return n;
}
```

Build it with `-mavx2`, or `-march=x86-64-v3`, and guard the call site so it only runs on
hardware that has AVX2:

```cpp cpu_check.cpp
#include <immintrin.h>

// GCC and Clang. Cheap enough to call once at startup, not per message.
bool have_avx2() noexcept {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
}
```

For a 32-level ladder this is four vector iterations instead of up to thirty-two scalar ones,
with no branch per level — a meaningful win on a book you walk on every update. Below about
eight or sixteen elements the setup cost dominates and the scalar loop is fine; measure the
crossover on your own data rather than assuming it.

### SWAR: SIMD on ordinary 64-bit registers

SWAR is "SIMD Within A Register": treating a `std::uint64_t` as eight independent byte lanes
and using ordinary arithmetic to operate on all of them. It needs no special instruction set,
no CPU feature check, and no vector registers, which makes it perfect for binary and ASCII
protocol decoding.

```cpp swar_digits.cpp
#include <cstdint>
#include <cstring>

// Are all eight bytes ASCII digits? One load, four ALU ops, no branches.
bool all_digits(const char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);                       // legal type-punning, free at -O2
    const std::uint64_t hi   = v & 0xF0F0F0F0F0F0F0F0ull;
    const std::uint64_t over = ((v + 0x0606060606060606ull) & 0xF0F0F0F0F0F0F0F0ull) >> 4;
    return (hi | over) == 0x3333333333333333ull;
}

// Parse exactly eight ASCII digits into an integer. Little-endian.
// A fixed-width price or quantity field in a text protocol, in ~10 instructions.
std::uint32_t parse_8_digits(const char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    v -= 0x3030303030303030ull;                                    // subtract '0' per byte
    // Combine adjacent bytes into 16-bit values: 10*b0 + b1, four at a time.
    const std::uint64_t pairs = ((v * (1 + (0x0Aull << 8))) >> 8)
                                & 0x00FF00FF00FF00FFull;
    // Combine adjacent pairs into 32-bit values: 100*p0 + p1, two at a time.
    const std::uint64_t quads = ((pairs * (1 + (0x64ull << 16))) >> 16)
                                & 0x0000FFFF0000FFFFull;
    // Combine the two halves: 10000*q0 + q1.
    return static_cast<std::uint32_t>((quads * (1 + (10000ull << 32))) >> 32);
}
```

The `memcpy` is the standard-blessed way to reinterpret bytes as an integer; both GCC and Clang
turn an 8-byte `memcpy` into a single `mov` at `-O2`. The multiply-shift-mask trick works
because a multiply is a shift-and-add network, so multiplying by `1 + (10 << 8)` computes
`b + 10*b` shifted, giving both the "keep" and the "scale by ten and add" in one instruction.

:::hft
Fixed-width ASCII fields are everywhere in exchange protocols that predate binary encodings,
and naive parsing with `std::from_chars` or, worse, `atoi`, shows up clearly in a profile of a
text-protocol feed handler. A SWAR parser is typically a few times faster than `from_chars` for
a known-width all-digit field on a modern server core, because it has no length loop, no
validation branch per character, and no error path. Keep the `all_digits` check: a malformed
field must be rejected, not silently parsed into garbage, and the check costs four instructions.
:::

## Libraries, `std::simd`, and where SIMD does not pay

Writing raw intrinsics ties you to one instruction set and one vendor. Three practical
options above them:

- **`std::simd`.** Voted into C++26 as `<simd>`, derived from the Parallelism TS. libstdc++
  ships the predecessor as `std::experimental::simd` in `<experimental/simd>` from GCC 11
  onward. It gives you `simd<int32_t>` types with operators and lane-wise masks, portable
  across SSE, AVX and NEON. Use it if your toolchain has it; check availability before
  designing around it.
- **Google Highway.** Runtime dispatch across instruction sets from one source, so one binary
  runs SSE4 on an old server and AVX-512 on a new one. The most production-ready option today.
- **xsimd** and **Vc.** Header-only wrapper types, simpler than Highway, compile-time target
  selection only.

The caveats matter as much as the capability.

**AVX-512 frequency behaviour.** On Skylake-SP and Cascade Lake server parts, sustained use of
512-bit instructions — especially heavy floating-point ones — drops the core's frequency by a
licence level, and neighbouring cores can be affected. The throughput win can be swallowed by
a 10 to 20 percent clock reduction on those generations, which is disastrous for a latency
path where most code is scalar. Ice Lake and later reduced this considerably and Sapphire
Rapids largely resolved it. If your fleet is mixed, benchmark the whole application, not the
kernel: the kernel gets faster while everything around it gets slower.

**Gather and scatter are slower than they look.** `_mm256_i32gather_epi32` looks like eight
loads for the price of one. On most implementations it is microcoded into roughly eight
separate loads plus overhead, so it wins only when the alternative is eight loads *and* eight
address computations *and* the loads mostly hit L1. Indexing into a hash table with a gather is
almost always a loss. This is another argument for the structure-of-arrays layout in lesson 20:
SoA makes accesses contiguous so you never need a gather.

**A memory-bound loop gains nothing.** If your loop reads a large array once and does one add
per element, you are limited by DRAM bandwidth, not by arithmetic throughput. Widening the
registers means the core waits in bigger units. Check the ratio: a loop doing fewer than
roughly one arithmetic operation per byte loaded is bandwidth-bound, and the fix is a smaller
data layout or better cache reuse, not SIMD. Lesson 28 shows how to confirm this with the
top-down methodology rather than guessing.

:::exercise
Write three versions of "find the first level at or above a target price" over an ascending
array of 32 `std::int32_t` offsets: a plain scalar loop, a branchless scalar loop that computes
the count without an early exit, and the AVX2 version from this lesson.

1. Confirm with `-fopt-info-vec-missed` that the scalar search loop is *not* vectorised, and
   read the reason the compiler gives.
2. Benchmark all three with the answer uniformly distributed across the 32 positions, then with
   the answer always at index 0, then always at index 31. Predict which version wins in each
   case before you run it.
3. Re-run at sizes 8, 32 and 256 to find where the AVX2 version starts to pay. Report p50 and
   p99, not the mean, using the histogram from lesson 26.
:::

## Takeaways

- SIMD multiplies throughput, not latency: 128-bit SSE holds four `int32` lanes, 256-bit AVX2
  holds eight, 512-bit AVX-512 holds sixteen plus per-lane mask registers.
- The auto-vectoriser needs contiguous access, a trip count fixed on entry, no early exit, no
  loop-carried dependency, no possible aliasing, and no opaque calls. Search loops fail on the
  early exit and always need hand-written intrinsics.
- Verify with `-fopt-info-vec`/`-fopt-info-vec-missed` on GCC, `-Rpass=loop-vectorize` and
  `-Rpass-missed` on Clang, and by grepping the assembly for `ymm` and `zmm`. `__restrict` and
  `std::assume_aligned` remove the aliasing and alignment doubts, and both are promises with
  undefined behaviour if broken.
- Floating-point reductions do not vectorise because addition is not associative.
  `-ffast-math` is the wrong fix for a trading system: it deletes your NaN checks. Use integer
  ticks, a targeted pragma, or explicit multiple accumulators.
- SWAR on plain 64-bit integers needs no instruction-set support and is the right tool for
  fixed-width ASCII protocol fields.
- Check the caveats before committing: AVX-512 downclocking on older server parts, gathers
  being roughly as slow as the loads they replace, and memory-bound loops that wider registers
  cannot help.
