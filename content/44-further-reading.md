---
title: Further Reading and Tools
part: Appendix
summary: An annotated map of the books, talks, tools, source code and reference sites worth your time, marked by whether they are a starting point or a follow-on to this series.
time: 10 min
level: beginner
tags: reading, tools, references, learning
---

There is an enormous amount written about C++ and about performance, and most of it is
either out of date or repeats what you already know. This is the filtered list: what each
item is, and the specific reason it earns your hours. Nothing here is on the list because
it is famous.

Two markers throughout. **[start]** means you can read it now, with no prerequisites
beyond the earlier parts of this series. **[deep]** means it assumes the material in Parts
III to VI and will be frustrating before that.

## Books

| Book | Marker | What it is and why |
|---|---|---|
| *Effective Modern C++*, Scott Meyers | **[start]** | Forty-two items on C++11 and C++14. Still the clearest explanation of move semantics, forwarding references and what `std::shared_ptr` actually costs. Dated in that it stops at C++14; read it anyway, then read lesson 7 again. |
| *A Tour of C++*, Bjarne Stroustrup | **[start]** | The fastest on-ramp for a strong programmer who is new to C++. Two hundred pages, no hand-holding, current with recent standards. |
| *Computer Systems: A Programmer's Perspective*, Bryant and O'Hallaron | **[start]** | The single best foundation for Part III. Caches, virtual memory, linking, and the machine's view of your program, taught from the bottom up. If one book on this page, this one. |
| *C++ Concurrency in Action*, Anthony Williams | **[start]** after lesson 31 | The clearest prose on the memory model outside the standard itself, plus practical lock-free structures. Read chapter 5 alongside lesson 32. |
| *Optimized C++*, Kurt Guntheroth | **[start]** | Measurement-first and practical, strong on containers and string handling, weaker on hardware. Good bridge between Parts I and III. |
| *Performance Analysis and Tuning on Modern CPUs*, Denis Bakhvalov | **[deep]** | Free PDF. Top-down microarchitecture analysis explained properly, with real `perf` and VTune workflows. The natural companion to lessons 28 and 29. |
| *Data-Oriented Design*, Richard Fabian | **[start]** | Free online. The argument behind lesson 20, made from games. The chapters on the real cost of object orientation will change how you lay out an order book. |
| *What Every Programmer Should Know About Memory*, Ulrich Drepper | **[deep]** | Free paper from 2007, around a hundred pages. The absolute numbers are stale; the explanation of DRAM, cache coherence and TLBs has not been bettered. Read sections 2, 3 and 6. |
| *Agner Fog's optimization manuals* | **[deep]** | Five free manuals. Manual 3 (microarchitecture) explains how each generation's front end and execution ports behave; manual 4 (instruction tables) is where you look up a latency. Reference, not narrative. |
| *Intel 64 and IA-32 Optimization Reference Manual*, and the *AMD Software Optimization Guide* for your Zen generation | **[deep]** | Vendor truth: prefetcher behaviour, store-forwarding rules, cache line and buffer sizes. Badly organised and indispensable. Search it, do not read it. |
| *The Art of Writing Efficient Programs*, Fedor Pikus | **[deep]** | Concurrency and performance together, with an unusually honest treatment of when lock-free is not worth it. |

## Talks

Conference talks are the fastest way into a topic because the speaker has already
discarded the boring nine tenths. All of these are on YouTube; titles are approximate.

| Speaker | Talk | Marker | Why |
|---|---|---|---|
| Matt Godbolt | *What Has My Compiler Done for Me Lately? Unbolting the Compiler's Lid* (CppCon 2017) | **[start]** | Teaches you to read x86 assembly and to use Compiler Explorer, in an hour. Watch this before anything else on this page. |
| Carl Cook | *When a Microsecond Is an Eternity: High Performance Trading Systems in C++* (CppCon 2017) | **[start]** | The canonical trading talk. Keeping the hot path warm, removing branches from the critical path, template dispatch instead of virtual, and why the code you run most is the code you must never run cold. |
| Chandler Carruth | *Efficiency with Algorithms, Performance with Data Structures* (CppCon 2014) | **[start]** | The distinction in the title is the whole point, and most performance arguments confuse the two. |
| Chandler Carruth | *Tuning C++: Benchmarks, and CPUs, and Compilers! Oh My!* (CppCon 2015) | **[deep]** | Benchmarking discipline, and the escape hatches that stop the optimiser deleting your benchmark. Pairs directly with lesson 27. |
| David Gross | *Trading at Light Speed: Designing Low Latency Systems in C++* (Meeting C++ 2022) | **[deep]** | A working desk's view: what abstraction really costs, and how they measure it. Assumes Parts III and IV. |
| Fedor Pikus | *Live Lock-Free or Deadlock: Practical Lock-Free Programming* (CppCon 2015, two parts) | **[deep]** | The honest version of lock-free: what it buys, what it costs, and how often a lock wins. Read lessons 32 and 33 first. |
| Timur Doumler | *C++ Atomics: From Basic to Advanced* (CppCon 2023) | **[deep]** | The best modern explanation of the memory model, acquire/release and why `seq_cst` is not free. |
| Mike Acton | *Data-Oriented Design and C++* (CppCon 2014) | **[start]** | Polemical and worth the irritation. The clearest statement of why layout beats cleverness. |
| Alexander Radchenko | *Exchange Connectivity* talks (CppCon) | **[deep]** | What a real venue session and market data layer look like: sequencing, gap fills, recovery. Pairs with lessons 36 and 38. |
| Various | Exchange and market-data protocol talks from CppCon and Meeting C++ | **[deep]** | Search for the venue you connect to. The protocol details are where the surprises live. |

## Tools

| Tool | Marker | What it is for |
|---|---|---|
| Compiler Explorer (godbolt.org) | **[start]** | Source to assembly, several compilers side by side, with a diff view and an execution pane. The single most used tool in this series. |
| quick-bench.com | **[start]** | Google Benchmark in the browser. Good for relative comparisons of two small functions, useless for absolute numbers because the host is shared. |
| `perf` | **[start]** for `perf stat`, **[deep]** for the rest | The workhorse: `perf stat` for counters, `perf record`/`report`/`annotate` for profiles, `perf c2c` for false sharing, `perf mem` for load sources, `perf trace` for syscalls. Lesson 28. |
| Intel VTune | **[deep]** | Top-down microarchitecture analysis with a usable interface, plus memory access analysis and roofline. Free. |
| `toplev` from pmu-tools | **[deep]** | Top-down analysis without VTune, straight from `perf` counters. |
| BOLT (`llvm-bolt`) | **[deep]** | Post-link binary layout optimiser. Typically several percent on instruction-cache-bound server code. Lesson 29. |
| `hwloc` and `lstopo` | **[start]** | Draws your machine: sockets, NUMA nodes, cache sharing, and which node the network card hangs off. Run it once and pin the picture up. Lessons 30 and 35. |
| `cyclictest` (rt-tests) | **[deep]** | Measures wakeup jitter. This is how you prove an isolated core is actually isolated rather than nominally isolated. Lesson 30. |
| HdrHistogram | **[start]** | Records latencies at constant cost with full precision in the tail, and its documentation teaches coordinated omission better than anything else. Lesson 26. |
| The sanitizers (ASan, UBSan, TSan, MSan) | **[start]** | Undefined behaviour, races and memory errors, found by running the program. Flags and costs are in lesson 42. |
| `clang-tidy` | **[start]** | Static checks that actually pay: the `performance-*`, `bugprone-*` and `cppcoreguidelines-*` sets. Run it in CI, not on your desk. |
| Valgrind `cachegrind` and `callgrind` | **[deep]** | Simulated cache and call statistics: deterministic, repeatable and roughly fifty times slower than the real thing. Use it to compare two implementations' miss counts, never to measure time. |
| `uica` (uops.info) and `llvm-mca` | **[deep]** | Static throughput analysis of a single basic block. They tell you the port pressure and the loop-carried bottleneck without running anything. Lesson 25. |

```sh The five commands that answer most questions
$ perf stat -e cycles,instructions,branches,branch-misses,cache-misses ./trader
$ perf record -g --call-graph=fp ./trader && perf report --stdio
$ perf c2c record ./trader && perf c2c report            # false sharing, lesson 21
$ lstopo --output-format txt                             # the machine's real shape
$ strace -c -f ./trader                                  # syscalls you did not know about
```

```sh Static analysis of one hot loop
$ g++ -std=c++23 -O3 -march=native -S -masm=intel -o - book.cpp | llvm-mca -mcpu=native
```

## Source worth reading

Reading a good implementation is worth more than reading about one. These are all small
enough or well-commented enough to repay an afternoon.

- **rigtorp/SPSCQueue and rigtorp/MPMCQueue** **[start]** after lesson 33. A few hundred lines each. The clearest real code that shows cache line padding, index caching and acquire/release used correctly. Read SPSCQueue immediately after building your own.
- **abseil** **[start]**. `flat_hash_map` is the swiss-table design that everyone else copied, and the comments explain the probing scheme properly. `InlinedVector` is the small-buffer pattern from lesson 24.
- **folly** **[deep]**. Facebook's library, and the comments are the value: `ProducerConsumerQueue`, `MPMCQueue`, `Function`, `small_vector`, `hazptr`. Written by people who measured.
- **boost.lockfree** **[deep]**. `spsc_queue`, `queue` and `stack`, conservative and unusually well documented for lock-free code.
- **Google Highway** and **xsimd** **[deep]**. Portable SIMD without intrinsics soup. Highway's documentation is the best available taxonomy of vector operations. Lesson 25.
- **fmt** **[start]**. Fast, compile-time-checked formatting, and the direct ancestor of `std::format`. Worth reading for the compile-time parsing alone.
- **Seastar** **[deep]**. Shared-nothing, one thread per core, no locks anywhere, explicit message passing between shards. Read the architecture documents even if you never use it; the argument is the point. Lesson 35.
- **DPDK** **[deep]**. Kernel bypass in production form. Read the ring buffer and the memory pool, which are the ideas from lessons 24 and 33 at industrial scale. Lesson 38.

## Reference sites

| Site | Marker | Use |
|---|---|---|
| cppreference.com | **[start]** | The standard library, usable. Not normative; when it matters, check the working draft at eel.is/c++draft. |
| The WG21 paper index (open-std.org) and the cplusplus/papers tracker | **[deep]** | Where the language is decided. Reading the paper that introduced a feature tells you what problem it was meant to solve, which the reference pages never do. |
| uops.info | **[deep]** | Machine-measured latency, throughput and port assignment for x86 instructions, per microarchitecture. More complete than any hand-made table. |
| Agner Fog's instruction tables | **[deep]** | The hand-curated counterpart, with commentary explaining the anomalies. |
| 7-cpu.com | **[start]** | Measured cache and memory latencies for specific CPU models. Use it to sanity-check your own numbers before believing them. |
| brendangregg.com | **[start]** | Flame graphs, the USE method, `perf` one-liners and eBPF. The best free systems-performance material anywhere. |
| easyperf.net (Denis Bakhvalov) | **[deep]** | Focused articles on microarchitecture, code layout and benchmarking methodology. |

:::hft
A practical reading order for someone joining a desk: Godbolt's compiler talk, then
Carl Cook's, then the caching and virtual memory chapters of *Computer Systems*, then
rigtorp's SPSCQueue source with lesson 33 open beside it. That is roughly eight hours and
it covers the vocabulary you will be expected to already have in your first week. The
vendor manuals and Agner Fog are for the day you have a specific number to look up, not
for the first month.
:::

## Links

Everything above that lives at a stable address. Conference talks are not listed
individually because their URLs change; search the CppCon channel for the speaker and
title given in the talks table.

| Resource | Address |
|---|---|
| Compiler Explorer | `https://godbolt.org` |
| quick-bench | `https://quick-bench.com` |
| cppreference | `https://en.cppreference.com` |
| WG21 paper lookup | `https://wg21.link` |
| Agner Fog, manuals and instruction tables | `https://agner.org/optimize/` |
| Drepper, What Every Programmer Should Know About Memory | `https://akkadia.org/drepper/cpumemory.pdf` |
| uops.info instruction data | `https://uops.info` |
| 7-cpu cache and memory latency measurements | `https://www.7-cpu.com` |
| Brendan Gregg on performance and flame graphs | `https://brendangregg.com` |
| Erik Rigtorp's articles and queues | `https://rigtorp.se` |
| rigtorp/SPSCQueue | `https://github.com/rigtorp/SPSCQueue` |
| rigtorp/MPMCQueue | `https://github.com/rigtorp/MPMCQueue` |
| Google Benchmark | `https://github.com/google/benchmark` |
| HdrHistogram | `https://hdrhistogram.github.io/HdrHistogram/` |
| folly | `https://github.com/facebook/folly` |
| abseil | `https://abseil.io` |
| Boost.Lockfree | `https://www.boost.org/doc/libs/release/doc/html/lockfree.html` |
| Google Highway | `https://github.com/google/highway` |
| xsimd | `https://github.com/xtensor-stack/xsimd` |
| fmt | `https://fmt.dev` |
| Seastar | `https://seastar.io` |
| DPDK | `https://www.dpdk.org` |
| BOLT | `https://github.com/llvm/llvm-project/tree/main/bolt` |
| hwloc and lstopo | `https://www.open-mpi.org/projects/hwloc/` |
| llvm-mca | `https://llvm.org/docs/CommandGuide/llvm-mca.html` |

:::note
Addresses are written as text rather than as links so that a printed copy of this page
stays useful, and so nothing here reaches out to the network when you open the site.
:::

## How to keep learning

Three habits, in decreasing order of return.

**Read assembly regularly.** Ten minutes a week on Compiler Explorer, looking at something
you wrote that day, is enough to build the instinct for what a line of C++ costs. The
point is not to memorise instructions; it is to stop being surprised. When you eventually
find a function that did not inline or a loop that did not vectorise, you will notice it
because the shape looks wrong.

**Follow the standards process, selectively.** You do not need to track every paper. Skim
the trip reports after each WG21 meeting, and when a feature you use changes, read the
paper that changed it. Papers are written to persuade a committee, so they state the
problem and the alternatives, which is exactly the context missing from documentation.

**Measure everything, and keep the numbers.** The habit that separates people who improve
systems from people who have opinions about them is writing the number down: which host,
which flags, which percentile, which date. A directory of past measurements is how you
notice a slow regression, how you settle an argument in one minute, and how you avoid
re-running an experiment someone already did. Lesson 26 is about the measurement; this is
about the filing.

:::exercise
Pick one item from each of the five sections above, in the **[start]** category, and book
the time for them this month. Then take the last function you wrote for the hot path, open
it in Compiler Explorer at `-O2` and at `-O3`, and write one paragraph about what changed.
That paragraph is the first entry in the measurement directory.
:::

## Takeaways

- *Computer Systems: A Programmer's Perspective* and Godbolt's compiler talk are the highest-return starting points; almost everything else builds on them.
- Vendor manuals and instruction tables are reference material for a specific question, not reading material.
- Reading a small, good implementation such as rigtorp's SPSCQueue teaches more than any article about lock-free programming.
- `perf`, Compiler Explorer, `lstopo` and HdrHistogram answer most questions you will have; the heavier tools are for the questions those leave open.
- Build the habit of reading assembly weekly, and of recording every measurement with its host, flags and date.
