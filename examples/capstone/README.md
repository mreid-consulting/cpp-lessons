# Capstone: runnable tick-to-trade skeleton

The system described in lesson 41, as files you can build and run. It needs no exchange
connection: `feed.hpp` generates a synthetic ITCH-like feed at startup.

```sh
$ make run
```

## Files

| File | Role | Lesson |
|---|---|---|
| `types.hpp` | strong price and quantity types, wire and decoded message layouts | 05, 36 |
| `decoder.hpp` | big-endian zero-copy decode, sequence gap detection | 36 |
| `book.hpp` | price-ladder book, order slab, open-addressing id index | 37 |
| `strategy.hpp` | micro-price quoting with inventory lean | 41 |
| `risk.hpp` | pre-trade gate: size, position, price band, rate limit | 40 |
| `spsc.hpp` | the wait-free queue from lesson 33 | 33 |
| `feed.hpp` | synthetic feed generator, startup only | 36 |
| `main.cpp` | the loop, warmup, instrumentation, cold logging thread | 26, 39 |

## What it does

One thread runs decode, book update, strategy, risk check and send. A second, cold thread
drains binary log records from the queue. Every allocation happens before the loop starts.
The run reports two numbers: an uninstrumented throughput pass, and a per-message latency
distribution where each sample carries two clock reads.

Typical output on an unpinned laptop:

```text
uninstrumented: 2000000 messages in 0.050 s, 40.33 M msg/s, 24.8 ns/msg
quotes sent      : 16595
fills simulated  : 260, peak |position| 100
  tick to trade  n=2000000  p50=40  p99=124  p99.9=208  max=196250  (ns)
```

The 24.8 ns figure is the real per-message cost. The p50 of 40 ns is mostly the two
`steady_clock` reads. The max is scheduler noise on an untuned machine, which is exactly
what lessons 26 and 30 exist to remove.

## What it is not

It has no network, no session management, no gap recovery, no multi-symbol sharding, no
persistence and no failover. Fills are simulated by a counter, not consumed from an
execution report. It is a shape to measure and optimise against, not a trading system.

## Exercises

1. Profile it. Which stage dominates, and does the answer match your guess?
2. Make the book struct-of-arrays and measure the change in cache misses. Lesson 20.
3. Replace the division in the micro-price with a reciprocal multiply. Lesson 40.
4. Pin the thread and rerun. Compare the max, not the median. Lesson 30.
5. Replace the `switch` on message type with a function-pointer table, then with a
   template dispatch, and measure all three. Lessons 22 and 23.
