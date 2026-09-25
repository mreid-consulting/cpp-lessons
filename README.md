# C++23 for Low-Latency Trading

**Read it: <https://mreid-consulting.github.io/cpp-lessons/>**

A 48-lesson lecture series that starts at "what is a class" and ends at a compilable
tick-to-trade skeleton. Static HTML, no dependencies, no network access required.

Part I covers the language from zero. Parts II and III cover C++23 and the machine:
caches, alignment, branch prediction, inlining, and programming without the heap. Parts IV
to VI cover measuring nanoseconds honestly, lock-free concurrency, and the trading systems
that use all of it. Every chapter earns its place on a hot path.

## Read it locally

```sh
./serve.sh          # builds, then serves on http://localhost:8000
./serve.sh 9000     # pick another port
```

Or in two steps:

```sh
python3 build.py
python3 -m http.server 8000
```

Then open <http://localhost:8000>.

## How the site is published

`content/*.md` is the only source of truth. Pushing to `main` triggers
`.github/workflows/pages.yml`, which lints the lesson structure, runs `build.py`, checks
every internal link, and deploys to GitHub Pages. The generated `index.html` and
`lessons/` are deliberately **not** committed, so clone and run `python3 build.py` to get
them locally.

The same workflow compiles and runs every example program on Linux with GCC 14, so a
snippet that only works on macOS fails the build rather than reaching a reader.

## Layout

| Path | What it is |
|---|---|
| `content/NN-slug.md` | lesson sources, the only files you edit |
| `build.py` | generator: markdown to HTML, C++ syntax highlighting, navigation |
| `assets/style.css` | single stylesheet, dark theme, print styles included |
| `assets/site.js` | copy buttons, lesson filter, arrow-key paging |
| `lessons/*.html` | generated output, safe to delete |
| `index.html` | generated curriculum page |
| `examples/` | standalone compilable programs, with a Makefile |
| `lint.py` | structural check on lesson sources, run it before building |
| `AUTHORING.md` | markup dialect and house style for writing lessons |
| `CURRICULUM.md` | the lesson manifest |

`build.py` deletes and regenerates `lessons/` on every run. Nothing outside that
directory and `index.html` is touched.

## Examples

Three self-contained programs that produce the numbers the lessons argue from.

```sh
$ cd examples && make run
```

| Program | What it shows |
|---|---|
| `cache_effects` | array-of-structs vs struct-of-arrays, pointer chasing, false sharing |
| `spsc_demo` | inter-core handoff latency and streaming throughput of the wait-free queue |
| `alloc_guard_demo` | fixed-capacity containers, an arena, a handle pool, and an `operator new` that aborts |
| `capstone/trace_demo` | always-on ring tracing, and which pipeline stage was slow on the worst messages |
| `repl/run.sh` | replays every interactive session from lesson 01a, including the inlining cost of splitting code across inputs |

The headers `spsc_queue.hpp`, `static_vector.hpp` and `trace_ring.hpp` are the reference
implementations from lessons 33, 24 and 28a, and are meant to be copied.

`examples/capstone/` holds the tick-to-trade skeleton from lesson 41: a decoder, a
price-ladder order book, a micro-price strategy, a pre-trade risk gate, and a synthetic
feed generator so it runs without an exchange connection.

```sh
$ cd examples/capstone && make run
```

It prints an uninstrumented throughput figure and an instrumented latency distribution,
so you can see how much the measurement itself costs.

On macOS the Makefile detects the Command Line Tools libc++ path and drops
`-march=native`, which Apple clang does not accept on arm64. On a Linux trading host,
build with `NATIVE=1`, which is the default there.

## Adding a lesson

Create `content/NN-slug.md` with the front matter described in `AUTHORING.md`,
then run `python3 lint.py` and rebuild. Ordering, navigation, the index page and prev/next links all follow
from the filename number and the `part` field.

## Requirements

Python 3.9 or newer to build. A C++23 compiler to run the code: GCC 14+ or Clang 18+.
The lessons assume Linux on x86-64 for the system-level material and flag it where
that matters.

## Keyboard

- `/` focuses the lesson filter
- Left and right arrows move between lessons
