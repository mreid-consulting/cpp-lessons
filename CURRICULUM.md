# Curriculum manifest

Authoritative list. `NN slug | title | part | level | time`.

A lesson may carry a letter suffix (`23a`) to slot into the sequence without renumbering
what follows. `build.py` sorts on the number then the suffix.

## Part I - Foundations
01 how-a-cpp-program-is-built    | How a C++ Program Is Built            | beginner | 25 min
01a interactive-cpp              | Interactive C++: A REPL for Asking Questions | beginner | 30 min
02 types-and-representation      | Types, Bits and Representation        | beginner | 30 min
03 functions-references-const    | Functions, References and const       | beginner | 25 min
04 what-a-class-is               | What a Class Really Is                | beginner | 35 min
05 enum-class-and-strong-types   | enum class and Strong Types           | beginner | 25 min
06 pointers-arrays-memory        | Pointers, Arrays and the Memory Model | beginner | 30 min
07 value-categories-and-moves    | lvalues, rvalues and Move Semantics   | intermediate | 40 min
08 dynamic-memory-and-ownership  | new, delete and Smart Pointers        | intermediate | 35 min
09 templates-and-generic-code    | Templates and Generic Code            | intermediate | 35 min
10 stl-containers-tour           | The STL Containers, Honestly          | intermediate | 35 min
11 algorithms-and-ranges         | Algorithms, Iterators and Ranges      | intermediate | 30 min

## Part II - Modern C++23
12 constexpr-consteval-constinit | constexpr, consteval, constinit       | intermediate | 30 min
13 concepts-and-if-constexpr     | Concepts and if constexpr             | intermediate | 30 min
14 lambdas-and-callables         | Lambdas and the Cost of a Callable    | intermediate | 30 min
15 vocabulary-types              | optional, variant and expected        | intermediate | 30 min
16 span-string-view-mdspan       | span, string_view and mdspan          | intermediate | 25 min
17 cpp23-toolbox                 | The C++23 Toolbox                     | advanced | 30 min
18 error-handling-policy         | Error Handling Without Exceptions     | advanced | 30 min

## Part III - The Machine
19 memory-hierarchy              | The Memory Hierarchy                  | intermediate | 35 min
20 data-oriented-design          | Data-Oriented Design: AoS vs SoA      | advanced | 40 min
21 alignment-and-false-sharing   | Alignment, Padding and False Sharing  | advanced | 30 min
22 branches-and-prediction       | Branches, Prediction and [[likely]]   | advanced | 35 min
23 inlining-and-devirtualization | Inlining, virtual and Devirtualization| advanced | 35 min
23a crtp-and-static-polymorphism | CRTP and Static Polymorphism          | advanced | 35 min
24 allocation-free-programming   | Programming Without the Heap          | advanced | 45 min
25 vectorization-and-simd        | Vectorization and SIMD                | expert | 35 min

## Part IV - Latency Engineering
26 measuring-latency             | Measuring Latency Honestly            | advanced | 35 min
27 microbenchmarking             | Microbenchmarking Without Lying       | advanced | 30 min
28 profiling-with-perf           | Profiling: perf, Counters, Top-Down   | advanced | 35 min
28a tracing-and-instrumentation  | Tracing: XRay, Processor Trace, Friends| expert | 40 min
29 code-layout-and-pgo           | Code Layout, iCache, PGO and BOLT     | expert | 30 min
29a cheap-build-wins             | Cheap Wins from the Build             | advanced | 35 min
30 os-and-hardware-tuning        | Taming the OS and the Hardware        | expert | 35 min

## Part V - Concurrency
31 threads-and-data-races        | Threads, Races and Why Locks Hurt     | advanced | 35 min
32 atomics-and-memory-order      | Atomics and Memory Ordering           | expert | 45 min
33 spsc-ring-buffer              | Building a Wait-Free SPSC Queue       | expert | 40 min
34 seqlock-for-market-data       | The Seqlock: Publishing Market Data   | expert | 35 min
35 thread-topology               | Cores, Pinning and Spinning           | expert | 30 min

## Part VI - Trading Systems
36 binary-protocol-decoding      | Decoding Binary Market Data           | advanced | 40 min
37 order-book-design             | Designing a Fast Order Book           | expert | 45 min
38 the-network-path              | The Network Path and Kernel Bypass    | expert | 35 min
39 hot-path-discipline           | Hot Path Discipline                   | expert | 30 min
40 correctness-and-risk          | Fixed-Point Maths, Risk and Testing   | advanced | 35 min
41 capstone-tick-to-trade        | Capstone: A Tick-to-Trade Skeleton    | expert | 60 min

## Appendix
42 compiler-flags-reference      | Compiler Flag Reference               | intermediate | 10 min
43 hot-path-review-checklist     | Hot Path Review Checklist             | advanced | 10 min
44 further-reading               | Further Reading and Tools             | beginner | 10 min
