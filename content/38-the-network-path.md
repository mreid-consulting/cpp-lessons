---
title: The Network Path and Kernel Bypass
part: Part VI - Trading Systems
summary: Where the microseconds go between the wire and your recv, which socket options are latency settings in disguise, and what each kernel-bypass family actually buys.
time: 35 min
level: expert
tags: networking, kernel-bypass, multicast, onload, ef_vi, dpdk, ptp
---

You have spent eight lessons shaving nanoseconds off your book update. Then a packet arrives
and the kernel spends eight microseconds handing it to you. Understanding that path is not
optional systems trivia — it is the only way to know whether your next week of C++ work will
show up in the number that matters, and it usually tells you that it will not.

## Where the microseconds go

A packet arriving on a conventional Linux socket takes this route.

```text
wire -> NIC PHY/MAC -> DMA into rx ring -> interrupt -> softirq (NAPI)
     -> skb alloc, GRO -> IP/UDP -> socket lookup -> sk_receive_queue
     -> wake blocked thread -> scheduler -> copy_to_user -> your recv() returns
```

Typical figures on a tuned server with a 10G or 25G NIC. Every number here is
hardware-, kernel- and load-dependent; measure your own with the methods from lesson 26.

| Stage | Typical cost |
|---|---|
| PHY and MAC, wire to descriptor | 100-300 ns |
| DMA into the ring plus descriptor writeback | 200-500 ns |
| Interrupt delivery, or the coalescing wait | 1-10 µs |
| softirq: NAPI poll, `skb` allocation, GRO | 1-3 µs |
| IP/UDP processing, socket lookup, enqueue | 0.5-2 µs |
| Waking the blocked thread, scheduler latency | 1-5 µs |
| `copy_to_user` inside `recv` | 0.2-1 µs per KB |

Two things should jump out. First, the actual protocol work — checksums, header parsing — is
a small minority of the total. Second, the two largest terms are both about *control
transfer*: the interrupt, and waking your thread. Everything kernel bypass does follows from
attacking those two.

:::key
The kernel is not slow at networking. It is slow at *asynchronously notifying a sleeping
process*, and a market data feed is nothing but a stream of asynchronous notifications. That
mismatch, not the protocol stack, is what you are paying for.
:::

**Interrupt coalescing** is the clearest example of a setting that looks like a throughput
knob and is really a latency knob. The NIC batches interrupts, waiting up to
`rx-usecs` microseconds or `rx-frames` packets before raising one. Left at a vendor default
of 50 µs, you have just added up to 50 µs of jitter to every packet that arrives on an idle
link, in exchange for fewer interrupts under load.

```sh Linux: look at it before you touch anything else
$ ethtool -c eth0
$ ethtool -C eth0 rx-usecs 0 rx-frames 1 adaptive-rx off
```

Setting it to zero raises the interrupt rate and burns CPU. That is the trade. On a
dedicated feed handler core it is almost always the right trade.

## Turning the knobs the kernel gives you

`TCP_NODELAY` disables **Nagle's algorithm**, which holds a small write back until the
previous unacknowledged data is acknowledged, so that many small sends coalesce into one
segment. Paired with the receiver's **delayed ACK**, which waits up to 40 ms before
acknowledging in the hope of piggybacking on a reply, you get the classic pathology: your
40-byte order sits in the sender's buffer waiting for an ACK that the peer is deliberately
not sending yet. The observed latency is not microseconds. It is tens of milliseconds.

```cpp Linux/POSIX: the socket options that are actually about latency
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

void tune_order_entry(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // never batch our orders

    unsigned busy_us = 50;                                          // spin in recv/poll
    ::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));

    int quickack = 1;                                               // resets after each recv
    ::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
}
```

`SO_BUSY_POLL` tells the kernel that when `recv` finds nothing, it should poll the device
driver directly for up to that many microseconds instead of sleeping. It removes the
interrupt and the thread wake from the path, which is most of the two largest rows in the
table above, at the cost of a core spinning. It also needs `net.core.busy_poll` and
`net.core.busy_read` set, and a driver that implements the `ndo_busy_poll` path.

Busy-polled blocking `recv` on a dedicated core generally beats `epoll` for a single feed,
because `epoll` adds a readiness notification and a second syscall before the data. `epoll`
wins when you have hundreds of sockets and the alternative is hundreds of spinning cores. On
a feed handler you have one or two sockets. Spin.

`recvmmsg` amortises the syscall across a batch. It does nothing for the first packet's
latency — that packet still waited for the whole batch to be assembled or for the poll to
find it — but under a burst it stops you falling behind, and falling behind is how you drop.

```cpp Linux: draining a burst with one syscall
#include <array>
#include <cstddef>
#include <sys/socket.h>

constexpr int kBatch = 32;

int drain(int fd, std::array<std::array<std::byte, 2048>, kBatch>& bufs) {
    std::array<::mmsghdr, kBatch> msgs{};
    std::array<::iovec,   kBatch> iov{};
    for (int i = 0; i < kBatch; ++i) {
        iov[i].iov_base = bufs[i].data();
        iov[i].iov_len  = bufs[i].size();
        msgs[i].msg_hdr.msg_iov    = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    return ::recvmmsg(fd, msgs.data(), kBatch, MSG_DONTWAIT, nullptr);
}
```

**io_uring** replaces the syscall-per-operation model with two shared ring buffers: you write
submission entries, the kernel writes completion entries, and with `IORING_SETUP_SQPOLL` a
kernel thread polls the submission ring so you need no syscall at all in steady state. It is
a genuine improvement — typically 2-5 µs receive latency against 5-15 µs for interrupt-driven
sockets — and it is the best thing available if you cannot buy bypass hardware. It is not
bypass: the packet still traverses the full kernel stack.

## Multicast market data

Exchanges publish market data as UDP multicast, usually as two identical feeds, A and B, on
different groups taking different physical paths. Order entry is TCP, because you need
reliability and ordering for something that moves money and you can afford the handshake
once at session start. Market data is UDP because retransmitting a stale quote to a thousand
subscribers is worse than useless.

```cpp Linux/POSIX: joining a market data group
#include <arpa/inet.h>
#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int join_feed(const char* group, std::uint16_t port, const char* local_if) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));   // A and B on one port

    int rcvbuf = 16 * 1024 * 1024;                                   // capped by rmem_max
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    ::sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = ::htons(port);
    addr.sin_addr.s_addr = ::inet_addr(group);   // bind to the group, not INADDR_ANY
    ::bind(fd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr));

    ::ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(group);
    mreq.imr_interface.s_addr = ::inet_addr(local_if);   // pin the NIC explicitly
    ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    return fd;
}
```

Three details that bite. Bind to the group address rather than `INADDR_ANY`, or you will
receive every group on that port and filter in software. Always name the interface: on a
multi-homed box the kernel's default choice will not be your low-latency NIC. And
`SO_RCVBUF` is silently clamped to `net.core.rmem_max`, so raise that sysctl or your
sixteen-megabyte request quietly becomes 208 KB.

:::warn
Multicast is unreliable by design and you *will* drop. The socket receive buffer overflowing
during an open-auction burst is the common cause, and the kernel does not tell you unless you
ask: read the drop counter from `/proc/net/udp` or the `recvmsg` error queue, and alert on
it. A feed handler that silently skips a sequence number is building a book that is wrong in
a way no unit test will find.
:::

Gap recovery has three standard shapes and you will implement at least two. Arbitrate the A
and B feeds, taking whichever packet for a sequence number arrives first and discarding the
duplicate, which covers isolated single-path loss. Request a retransmit from the exchange's
recovery service, which costs milliseconds and is for small gaps. Or re-snapshot from the
refresh feed and rebuild the book, which is the only option for a large gap and which you
must be able to do without stopping the primary feed handler.

## Kernel bypass

The idea is one sentence: map the NIC's descriptor rings and doorbell registers into user
space so the application reads packets out of DMA buffers directly, with no interrupt, no
`skb`, no socket queue, no copy, and no context switch. The kernel is involved at setup and
never again on the data path.

The families, and what using each one costs you in engineering effort:

- **Solarflare Onload** (and Mellanox VMA) is a `LD_PRELOAD` shim that reimplements the BSD
  socket API in user space over the NIC. Your existing `recv` code is unchanged. This is the
  highest ratio of latency won to work done in the whole field.
- **ef_vi** (Solarflare) and the **Exablaze/Cisco Nexus SmartNIC** APIs are raw layer-2
  interfaces: you get descriptor rings and you write the parsing. No TCP, no IP, no ARP —
  those are now your problem. In exchange you get the shortest software path available.
- **DPDK** is the vendor-neutral version of the same idea, with a poll-mode driver, huge
  pages and a large ecosystem. Broad hardware support, more moving parts, and you inherit a
  userspace networking stack to maintain.
- **RDMA** (InfiniBand or RoCE) lets a remote machine write into your memory with no CPU
  involvement at all on the receive side. It is the right tool for internal fan-out between
  your own boxes; exchanges do not speak it.

```text The shape of a raw-API receive loop, e.g. ef_vi
for (;;) {
    n = poll_event_queue(vi, events, kMaxEvents);   // no syscall, no interrupt
    for (i = 0; i < n; ++i) {
        if (type(events[i]) == RX) {
            pkt = dma_buffer_for(events[i]);        // the NIC wrote here directly
            handle(pkt + kEthIpUdpHeaderBytes, len(events[i]));
            repost(vi, events[i]);                  // hand the buffer back to the NIC
        }
    }
}
```

Realistic one-way bands, user-space application to user-space application across a single
switch. Treat these as orders of magnitude, not specifications.

| Path | Typical |
|---|---|
| Kernel sockets, interrupt driven, untuned | 10-30 µs |
| Kernel sockets, tuned, busy polling | 3-8 µs |
| io_uring with SQPOLL | 2-5 µs |
| Onload / VMA socket shim | 1.5-3 µs |
| DPDK | 1-2 µs |
| ef_vi / Exablaze raw API | 0.8-1.5 µs |
| FPGA, wire in to wire out | 50-500 ns |

## The wire, the switch and the clock

A **store-and-forward** switch receives an entire frame, checks its CRC, and then transmits
it. At 10 Gbps a 1500-byte frame takes 1.2 µs to serialise, so the switch adds at least that
much. A **cut-through** switch begins transmitting as soon as it has read the destination
MAC, adding perhaps 300-500 ns regardless of frame size, at the cost of forwarding the
occasional corrupt frame. Every switch on a trading path is cut-through, and the good ones
are under 100 ns.

You cannot manage what you cannot measure, and software timestamps taken in your process
include everything you are trying to isolate. NIC **hardware timestamping** stamps the frame
at the MAC, giving you a clean wire-arrival time you can subtract from your own clock read.

```cpp Linux: ask the NIC to stamp arrivals
#include <linux/net_tstamp.h>
#include <sys/socket.h>

void enable_hw_timestamps(int fd) {
    int flags = SOF_TIMESTAMPING_RX_HARDWARE
              | SOF_TIMESTAMPING_RAW_HARDWARE
              | SOF_TIMESTAMPING_SOFTWARE;
    ::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
    // Timestamps arrive as an SCM_TIMESTAMPING control message on recvmsg.
}
```

Those stamps come from the NIC's own oscillator, which drifts. **PTP** (IEEE 1588) disciplines
it against a grandmaster clock, typically GPS-backed, to tens of nanoseconds — good enough to
compare your arrival time against the exchange's own timestamp and against your neighbours'.
NTP, at hundreds of microseconds to milliseconds, is not.

The rest is physics and real estate. Light in fibre travels about 5 µs per kilometre, so
**colocation** — your server in the exchange's own data centre — is not an optimisation, it
is a precondition. Within the building, a **cross-connect** is a direct fibre run from your
cage to the exchange's, bypassing any shared infrastructure. Exchanges sell equal-length
cables to every cage precisely so that this stops being a differentiator.

:::hft
Order the terms by size before you optimise anything. A colocated, cross-connected,
Onload-enabled system might see 1.5 µs from wire to your handler, 500 ns of decode and book
update, 300 ns of strategy and risk, 1.5 µs back out, and then 20-100 µs inside the exchange
matching engine. Halving your 300 ns of strategy code moves the total by well under one
percent. Moving from interrupt-driven sockets to a bypass stack moves it by ten. Know which
number you are working on.
:::

:::exercise
Build a two-process ping-pong over UDP on the same host and on two hosts through one switch,
timestamping with `rdtsc` on both sides and reporting the full distribution, not the mean.
Then vary one thing at a time: `ethtool -C rx-usecs` from the default to 0; `SO_BUSY_POLL`
off and on; the receiving thread unpinned, pinned, and pinned with the interrupt steered to
the same core. Record the p50, p99 and p99.9 for each of the eight combinations. The point
is not the numbers — it is that two of these knobs will matter far more than you expect and
one will matter not at all.
:::

## Takeaways

- The kernel receive path costs 5-15 µs typically, and most of it is interrupt delivery and
  waking your thread, not protocol processing.
- Interrupt coalescing is a latency setting wearing a throughput costume. Look at it first.
- `TCP_NODELAY` is mandatory for order entry; Nagle plus delayed ACK turns microseconds into
  tens of milliseconds.
- Busy-poll one or two sockets on a dedicated core rather than reaching for `epoll`, and use
  `recvmmsg` so bursts do not become drops.
- Kernel bypass moves you from microseconds to hundreds of nanoseconds. Onload gives most of
  that with no code change; ef_vi and DPDK give the rest in exchange for writing your own
  stack.
- Timestamp in hardware, discipline the clock with PTP, and colocate — then re-check whether
  your C++ is still the bottleneck. Below a few microseconds, usually it is not.
