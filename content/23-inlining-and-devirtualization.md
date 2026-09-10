---
title: Inlining, virtual and Devirtualization
part: Part III - The Machine
summary: What a function call really costs, why the inline keyword does not inline, how virtual dispatch works, and four ways to get polymorphism without paying for it.
time: 35 min
level: advanced
tags: inlining, virtual, crtp, variant, lto
---

The most expensive thing about a function call is not the call. It is the wall the call
puts up in front of the optimiser. On one side the compiler knows the value of every
variable; on the other it knows nothing, must assume memory was clobbered, and must
materialise values into the registers the ABI dictates. Inlining knocks the wall down.
Virtual dispatch builds it out of concrete and adds an indirect branch on top.

## What a call actually costs

Consider a call to a small function in another translation unit.

```asm what a non-inlined call looks like
    mov     rdi, QWORD PTR [rbx]      ; marshal args into ABI registers
    mov     esi, r12d
    call    notional(Order const&)    ; push return address, jump
    ; ... callee runs, may clobber rax rcx rdx rsi rdi r8-r11
    mov     QWORD PTR [rbp-8], rax    ; take the result back
```

The pieces, in increasing order of importance:

1. **Argument setup.** Values that were living happily in whatever register the compiler
   chose must be moved into the registers the calling convention names. On the System V
   x86-64 ABI that is `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` for integers and pointers.
2. **`call` and `ret`.** A push of the return address, a jump, and a return. The return is
   predicted through a return-stack buffer and is normally accurate. Call it 2 to 5 cycles
   of direct overhead on a modern x86 server core.
3. **Register spills.** The callee may clobber every caller-saved register. Anything live
   across the call and held in one of those must be spilled to the stack and reloaded.
4. **The optimisation barrier.** This is the real cost. The compiler cannot constant-fold
   through the call, cannot prove your `std::int64_t` is still in range afterwards, must
   assume any memory reachable through a pointer argument may have been written, and cannot
   hoist a load across it. A loop containing an opaque call will not be vectorised, will not
   be unrolled usefully, and will re-load values it already had.

:::key
For a two-line accessor, items 1 to 3 might be 5 cycles and item 4 might be 50. Inlining is
not primarily about removing `call`; it is about letting the optimiser see one contiguous
region of code instead of two isolated ones.
:::

## The `inline` keyword does not inline

This surprises everyone, so state it flatly.

```cpp mid.hpp
#include <cstdint>

inline std::int64_t mid_ticks(std::int64_t bid, std::int64_t ask) noexcept {
    return (bid + ask) / 2;
}
```

The `inline` keyword is a **linkage property**. It relaxes the One Definition Rule so this
function may be defined in every translation unit that includes the header, and tells the
linker to keep one copy and discard the rest. That is its entire standard-mandated meaning.
It is a weak, ignorable hint about inlining and modern compilers largely disregard it as
such.

What actually causes inlining is the compiler's cost model. Roughly:

- **Body size**, measured in the compiler's internal instruction count, against a threshold.
  GCC's is tuned by `-finline-limit` and friends; Clang's by `-mllvm -inline-threshold`.
- **Call-site count.** A function called from one place is nearly free to inline because the
  out-of-line copy can then be deleted. A function called from two hundred places is not.
- **Whether the callee body is visible at all.** Defined in a header, or in the same `.cpp`,
  or reachable through link-time optimisation. Otherwise the question does not arise.
- **Call-site context.** Arguments that are compile-time constants make inlining far more
  attractive, because constant propagation into the body may collapse most of it.
- **Whether it is `static`/anonymous-namespace.** Internal linkage means the compiler knows
  it has seen every call site.

Note the corollary: a function defined in a header without `inline` and used from two TUs is
an ODR violation, and a `static` function in a header gives every TU its own private copy,
which bloats the binary. `inline` remains the right spelling for a header-defined function —
just not for the reason its name suggests.

### Forcing the issue

```cpp force.hpp
#include <cstdint>

// Always inline: the compiler's heuristic will be overridden.
// Reserve for genuinely tiny hot-path helpers where you have measured a win.
[[gnu::always_inline]]
inline std::int64_t px_to_ticks(std::int64_t price_e9, std::int64_t tick_e9) noexcept {
    return price_e9 / tick_e9;
}

// Never inline: keep this out of the hot path's instruction footprint.
// Right for cold error handling, logging, and slow-path recovery.
[[gnu::noinline]]
void log_reject(std::uint64_t order_id, const char* reason) noexcept;
```

`[[gnu::always_inline]]` is the right tool when a small function sits in a loop and the
compiler's size heuristic declines because the function is called from many places. It is
the wrong tool for anything with a non-trivial body: force-inlining a 200-instruction
function into forty call sites will multiply your instruction footprint by forty.

`[[gnu::noinline]]` is more often the useful one. Marking the cold path `noinline` shrinks
the hot function, which makes the hot function more attractive to inline into *its* callers,
and keeps error-handling code out of the instruction cache lines you execute per tick. It is
also indispensable in benchmarks as a barrier the optimiser will not cross.

:::perf
Cross-TU inlining requires **link-time optimisation**. Build and link with `-flto` (and
`-ffat-lto-objects` if you also need conventional object files) and the compiler defers real
code generation to link time, when it can see every TU at once. On a codebase split across
many files, `-flto` frequently recovers 5 to 15 percent on hot paths that cross file
boundaries — a typical figure for a mid-sized trading application, and one you must measure
on your own binary. Its cost is link time and slightly harder debugging.
:::

## Virtual dispatch, mechanically

```cpp virtual_strategy.cpp
#include <cstdint>

struct Strategy {
    virtual ~Strategy() = default;
    virtual std::int64_t quote(std::int64_t mid_ticks) noexcept = 0;
};

struct Passive final : Strategy {
    std::int64_t offset_ticks = 2;
    std::int64_t quote(std::int64_t mid_ticks) noexcept override {
        return mid_ticks - offset_ticks;
    }
};
```

A class with virtual functions gets a hidden pointer member, the **vptr**, normally at
offset 0. It points to that class's **vtable**, a static array of function addresses, one
slot per virtual function. Calling `s->quote(m)` compiles to:

```asm
    mov     rax, QWORD PTR [rdi]        ; load the vptr from the object
    mov     rax, QWORD PTR [rax+16]     ; load the slot for quote() from the vtable
    call    rax                         ; indirect call
```

Four costs, in increasing order of importance:

1. **The vptr load.** A dependent load from the object, normally an L1 hit if you were about
   to touch the object anyway.
2. **The vtable load.** A second dependent load, from a different cache line than your data.
   Vtables are shared and usually hot, but this is a pointer chase and the two loads are
   serialised.
3. **The indirect call.** Predicted through the **branch target buffer**. If this site always
   reaches the same target, the BTB predicts it and the cost is close to a direct call. If
   the target genuinely varies from call to call, you take a mispredict — a full pipeline
   flush, typically 15 to 20 cycles on a modern x86 server core.
4. **No inlining.** The compiler does not know the callee, so it cannot inline it, cannot
   propagate constants into it, and must treat it as a full optimisation barrier.

:::perf
Typical figures on a modern x86 server core, for a call to a body of a few instructions:

| Form | Approximate cost |
|---|---|
| Inlined call | 0, plus whatever the body costs |
| Direct out-of-line call | 2 to 5 cycles, plus the lost optimisation |
| Virtual call, single hot target | 3 to 8 cycles |
| Virtual call, target varies unpredictably | 20 to 30 cycles |

Measure your own; these move with microarchitecture and with how hot the vtable line is. The
gap between rows one and four is the number that decides your design.
:::

### `final` and speculative devirtualization

```cpp devirt.cpp
#include <cstdint>

struct Strategy {
    virtual ~Strategy() = default;
    virtual std::int64_t quote(std::int64_t mid_ticks) noexcept = 0;
};

struct Passive final : Strategy {          // no further derivation possible
    std::int64_t offset_ticks = 2;
    std::int64_t quote(std::int64_t mid_ticks) noexcept override {
        return mid_ticks - offset_ticks;
    }
};

std::int64_t run(Passive& p, std::int64_t mid) noexcept {
    // The static type is final, so quote() cannot be overridden further.
    // The compiler resolves this at compile time and can inline the body:
    // no vptr load, no vtable load, no indirect call.
    return p.quote(mid);
}
```

`final` on a class, or on an individual `override`, tells the compiler that no further
override exists, which lets it devirtualize any call where it knows the static type. Even
without `final`, with LTO or whole-program visibility the compiler may find that exactly one
class derives from `Strategy` and perform **speculative devirtualization**: emit a compare
of the vptr against the expected vtable, inline the likely body on the fast path, and fall
back to the indirect call otherwise. Marking every leaf class `final` costs nothing and
occasionally hands you this for free.

:::hft
The desk-level rule is not "never use `virtual`". It is: virtual dispatch is fine wherever it
runs once per session and forbidden wherever it runs once per tick. Strategy selection at
startup, configuration, venue plugins, the risk report — use virtual, it is clearer. The
per-message dispatch inside a feed handler, the price update path, the order-book mutation —
resolve those statically. A million messages a day through one unpredictable indirect call is
about 20 ms of pure mispredict, and worse, it is 20 ms concentrated in exactly the bursts
where the market is moving and every path is cold.
:::

## Four ways to dispatch without a vtable

### CRTP: static polymorphism by inheritance

The base class is a template parameterised on the derived class, so the "virtual" call is a
static call the compiler can inline.

```cpp crtp.cpp
#include <cstdint>

template <class Derived>
struct StrategyBase {
    // Non-virtual: resolved at compile time, fully inlinable.
    std::int64_t quote(std::int64_t mid_ticks) noexcept {
        return static_cast<Derived*>(this)->quote_impl(mid_ticks);
    }
};

struct Passive : StrategyBase<Passive> {
    std::int64_t offset_ticks = 2;
    std::int64_t quote_impl(std::int64_t mid_ticks) noexcept {
        return mid_ticks - offset_ticks;
    }
};

// Callers must be templates too; the concrete type travels with them.
template <class S>
std::int64_t run_once(StrategyBase<S>& s, std::int64_t mid) noexcept {
    return s.quote(mid);
}
```

CRTP does more than dispatch: a base can inject behaviour and per-derived state back into
the class below it, which is how zero-cost instrumentation and risk layers get built.
Lesson 23a covers the pattern in full, including the ways it bites and the C++23 feature
that replaces most of it.

### Templates plus concepts

Usually simpler than CRTP and, since C++20, with readable error messages.

```cpp concepts_dispatch.cpp
#include <concepts>
#include <cstdint>

template <class S>
concept Quoter = requires(S& s, std::int64_t mid) {
    { s.quote(mid) } noexcept -> std::same_as<std::int64_t>;
};

struct Aggressive {
    std::int64_t edge_ticks = 1;
    std::int64_t quote(std::int64_t mid_ticks) noexcept { return mid_ticks + edge_ticks; }
};

template <Quoter S>
std::int64_t on_tick(S& s, std::int64_t mid_ticks) noexcept {
    return s.quote(mid_ticks);       // direct call, inlined at -O2
}
```

### `std::variant` plus `visit` over a closed set

When the set of implementations is known at compile time but the choice is made at run time,
a variant gives you a closed-world dispatch with no heap allocation and no vtable pointer
chase. The visit itself compiles to a jump table over the variant's index.

```cpp variant_dispatch.cpp
#include <cstdint>
#include <variant>

struct Passive    { std::int64_t off = 2; std::int64_t quote(std::int64_t m) const noexcept { return m - off; } };
struct Aggressive { std::int64_t off = 1; std::int64_t quote(std::int64_t m) const noexcept { return m + off; } };
struct Inactive   {                        std::int64_t quote(std::int64_t  ) const noexcept { return 0; } };

using AnyStrategy = std::variant<Passive, Aggressive, Inactive>;

std::int64_t quote(const AnyStrategy& s, std::int64_t mid_ticks) noexcept {
    return std::visit([mid_ticks](const auto& impl) noexcept {
        return impl.quote(mid_ticks);      // each alternative inlined into its arm
    }, s);
}
```

The object is stored inline, so there is no pointer chase to the implementation's data — a
real cache advantage over a `std::unique_ptr<Strategy>`. The dispatch is still an indirect
jump through a table, so an unpredictable index still costs a BTB mispredict; what you have
removed is the vtable load and the inlining barrier.

### A function-pointer table

The lowest-level option, and the right one when the dispatch key arrives as an integer from
the wire, as message types do.

```cpp fptr_table.cpp
#include <array>
#include <cstddef>
#include <cstdint>

struct Book;

using Handler = void (*)(Book&, const std::byte*) noexcept;

void on_add(Book&, const std::byte*) noexcept;
void on_modify(Book&, const std::byte*) noexcept;
void on_cancel(Book&, const std::byte*) noexcept;
void on_trade(Book&, const std::byte*) noexcept;
void on_unknown(Book&, const std::byte*) noexcept;

inline constexpr std::array<Handler, 8> kHandlers{
    &on_add, &on_modify, &on_cancel, &on_trade,
    &on_unknown, &on_unknown, &on_unknown, &on_unknown,
};

// Table is padded to a power of two so the index mask replaces a bounds branch.
inline void dispatch(Book& b, std::uint8_t msg_type, const std::byte* payload) noexcept {
    kHandlers[msg_type & 0x7u](b, payload);
}
```

### Choosing between them

| Approach | Dispatch cost | Set of impls | Compile-time cost | Notes |
|---|---|---|---|---|
| `virtual` | vptr + vtable load + indirect call | open, run-time extensible | low | Clearest; hides the callee from the optimiser |
| CRTP | zero, fully inlined | closed, compile-time | moderate; templates spread | Caller must be a template |
| Concepts + templates | zero, fully inlined | closed, compile-time | moderate | Simplest static option; prefer this |
| `std::variant` + `visit` | jump table, no pointer chase | closed, chosen at run time | moderate | Object stored inline; good cache behaviour |
| Function-pointer table | indirect call, no vtable load | open, mutable at run time | none | Natural fit for wire message types |

The pragmatic answer for most trading code: concepts and templates on the hot path, a
`std::variant` when the choice must be made at run time from a fixed set, `virtual` for
everything cold.

## When inlining makes things slower

Inlining is not free and more is not better.

The instruction cache is small — commonly 32 KB per core on recent Intel and AMD server
parts, backed by a micro-op cache of a few thousand entries. Every copy of an inlined body
occupies distinct bytes in that cache. Inline a 200-byte function into thirty call sites in a
loop nest and you have spent 6 KB of a 32 KB budget on one function. The result is
instruction-cache misses, front-end stalls, and a program that is slower despite having
strictly fewer instructions executed.

Secondary effects compound it: a bigger function needs more registers live at once, so you
get stack spills; more distinct branches spread across more addresses dilute the branch
predictor's tables; and the loop that used to fit in the loop-stream detector no longer does.

How to spot it:

```sh
$ perf stat -e cycles,instructions,L1-icache-load-misses,icache_64b.iftag_stall ./trader
$ perf stat -e idq_uops_not_delivered.core ./trader     # front-end starvation, Intel
$ nm --size-sort -S ./trader | tail -20                  # which functions exploded
```

A rising instruction count with falling instructions-per-cycle after an inlining change, or a
climbing L1 instruction-cache miss rate, is the signature. The counter names differ by vendor
and generation, so check `perf list` on the actual machine.

:::pitfall
The classic self-inflicted version: `[[gnu::always_inline]]` on a "small" function that grows
over a year of maintenance until it is no longer small, in a header included everywhere. Nobody
re-measures, the annotation stays, and the hot loop quietly stops fitting in the instruction
cache. Re-check every `always_inline` when you touch the function it decorates.
:::

:::exercise
Build a feed handler that dispatches one million synthetic messages four ways: through a
`virtual` call on `std::unique_ptr<Strategy>`, through CRTP, through `std::visit` on a
`std::variant`, and through a function-pointer table. Run each with the message type held
constant, then with the type drawn at random from four values. Collect
`perf stat -e cycles,branch-misses,L1-icache-load-misses`.

Predict before you run: virtual and the constant type should be within a few percent of the
static versions because the BTB pins the target. Virtual with a random type should be several
times worse. Then check whether the compiler devirtualized anything by adding `final` to the
concrete strategy and diffing the assembly.
:::

## Takeaways

- The dominant cost of a call is that it stops the optimiser, not the `call` instruction.
  Inlining is about visibility, not about saving a few cycles.
- `inline` is an ODR relaxation, not an inlining command. Inlining is decided by the
  compiler's cost model on body size, call-site count, and argument constancy.
- `[[gnu::noinline]]` on cold code is more often the right lever than `[[gnu::always_inline]]`
  on hot code. `-flto` is what restores inlining across translation units.
- A virtual call costs a vptr load, a vtable load, an indirect call, and the loss of inlining.
  With one stable target it is nearly free; with a varying target it costs a full mispredict.
- Use concepts and templates for hot-path polymorphism, `std::variant` for a run-time choice
  from a closed set, function-pointer tables for wire message types, and `virtual` for cold
  code. Mark every leaf class `final`.
- Over-inlining bloats the instruction cache and can lose. Watch instructions-per-cycle and
  the L1 instruction-cache miss rate, not the instruction count.
