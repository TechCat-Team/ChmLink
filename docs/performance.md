# Performance

**English** | [简体中文](zh-CN/performance.md)

All figures come from one test VM on an AMD Ryzen 9 9950X host with Windows
11 build 26200: the throughput and latency tables with 4 vCPUs and 32 GB for
the guest and package 0.5.1.0, medians of three runs; the resource figures
with 8 vCPUs and 16 GB and package 0.5.1.3. They measure the adapter and the
Windows stack on one machine; there is no encryption and no physical NIC, so a
real tunnel adds its own costs.

## Method

`bench/packet_bench.c` measures packet rates and latency: Windows UDP sockets
send through the adapter while one thread receives from the ring (*from
Windows*), and injecting threads send UDP packets that Windows sockets receive
(*to Windows*). `bench/tcp_bench.c` runs TCP through the stack: a reflector
thread receives every packet from the adapter, rewrites its addresses and
injects it back, so one machine carries both ends of each connection. The same
programs run on Wintun and tap-windows6 through `bench/wintun_shim.c` and
`bench/tap_shim.c`; see [development.md](development.md#benchmarks) for the
runners.

## Against Wintun and tap-windows6

Wintun 0.14.1 (official signed `wintun.dll`) and tap-windows6 9.27.0 in TUN
mode (official signed driver; 64 reads kept posted, as a tuned application
would). The three ran in turn on the same VM, three rounds each, at MTU 1500
unless noted, at normal priority.

| scenario | ChmLink | Wintun | tap-windows6 |
|---|---|---|---|
| 64 B round trip, median / 99th percentile | 14.6 / 77 µs | 14.7 / 56 µs | 41 / 178 µs |
| UDP from Windows, one sender, 64 B | 479k pps | 473k pps | 45k pps |
| UDP from Windows, four senders, 64 B | 1.24M pps | 1.23M pps | 148k pps |
| UDP to Windows, one thread, one flow, 64 B | 550k pps | 350k pps | 72k pps |
| UDP to Windows, one thread, 64 flows, 64 B | 1.75M pps | 348k pps | 74k pps |
| UDP to Windows, two threads, 64 B | 1.18M pps | 352k pps | 72k pps |
| CPU time per packet to Windows | 2.3 µs | 3.8 µs | 16.4 µs |
| TCP, one stream / four streams | 21.0 / 23.9 Gbps | 4.2 / 4.2 Gbps | 0.89 / 0.90 Gbps |
| TCP at MTU 65535, packets passed whole | 97 Gbps | 52 Gbps | — |

From Windows and in the round trip, ChmLink and Wintun are on par: the Windows
stack and the sending application set the pace there. Towards Windows,
ChmLink indicates batches without copying, on several cores by flow and by
sending thread; Wintun indicates from one thread. tap-windows6 moves one
packet per read or write request and stays an order of magnitude behind.

Wintun's round trip above comes from an application loop that polls the ring
before it waits, as the ChmLink SDK does when packets come quickly. Waiting on
Wintun's event at once, as its example does, the round trip took 28 µs at the
median and 113 µs at the 99th percentile, and varied widely from run to run
(8 vCPUs, median of three runs).

## ChmLink in detail

| scenario | result |
|---|---|
| UDP from Windows, one sender, 64 / 1400 B | 475k / 463k pps |
| UDP from Windows, four senders, 64 B | 1.26M pps |
| UDP to Windows, one thread, one flow, batches of 1 / 32 | 664k / 616k pps |
| UDP to Windows, one thread, 64 flows | 1.78M pps |
| UDP to Windows, two threads | 1.20M pps |
| both directions at once, 64 B | 624k to / 458k from Windows |
| 64 B round trip, median / 99th percentile | 14.6 / 70 µs |
| TCP, IP mode, one / four streams | 23.6 / 26.0 Gbps |
| TCP, IP mode at MTU 65535, cut to 1500 before injecting | 20.8 Gbps |
| … and merged again by `chmlink_coalescer` | 34.6 Gbps |
| … injected whole | 98 Gbps |
| TCP, Ethernet mode, no offloads | 19.9 Gbps |
| TCP, Ethernet mode, 1500-byte frames merged into RSC by `chmlink_coalescer` | 33.6 Gbps |
| TCP, Ethernet mode, LSO and RSC, one / four streams | 116 / 96 Gbps |

A consumer that stalls for 20 ms every 100 ms still receives 85% to 100% of the
offered rate with the default 4 MiB ring, depending on packet size.

## Resource use

`tools/bench-resources.ps1` (`bench/resource_bench.c`) sends 64-byte UDP
packets both ways at fixed rates through an open session and reports the CPU
the whole machine spends above idle, without the pacing threads that stand in
for the network. Rates above 1000 per second run twice: in bursts every
millisecond, as a NIC's interrupt moderation delivers traffic, and evenly
spaced, the worst case for polling. Wintun runs twice: with the application
polling the ring before it waits, and waiting on its event at once, as
Wintun's example does. One run each; differences under 0.05 cores are noise.

| packets per second, each way | ChmLink | Wintun, application polls | Wintun, application waits |
|---|---|---|---|
| idle session, 100, 1000 | ≤ 0.03 cores | ≤ 0.03 | ≤ 0.03 |
| 10,000 in 1 ms bursts | 0.06 | 0.10 | 0.04 |
| 10,000 evenly spaced | **0.27** | 1.88 | 1.09 |
| 50,000 in 1 ms bursts | 0.21 | 0.22 | 0.26 |
| 50,000 evenly spaced | 2.06 | 2.09 | 1.18 |

Polling an empty ring before sleeping keeps latency low, but with packets
100 µs apart it would only keep a core busy. ChmLink's SDK and driver
therefore poll only while packets have recently come within 64 µs
([architecture.md](architecture.md#data-path)). Wintun's driver keeps a core
busy in both evenly spaced cases, however the application waits. With
packets 20 µs apart ChmLink still polls on both sides; an application that
wants the fewest cores there and accepts more latency can wait on
`chmlink_get_read_event` itself, as Wintun's example does with its event. On
this VM the 64-byte round trip through ChmLink 0.5.1.3 took 14.5 µs at the
median and about 100 µs at the 99th percentile (three runs).

Memory and threads per session:

| | ChmLink | Wintun |
|---|---|---|
| locked ring memory | 8 MiB with the default 4 MiB rings; 4 MiB more per additional sending thread, up to 20 MiB | 8 MiB with the 4 MiB rings used here |
| kernel threads | one per sending ring, plus up to four flow workers (5 to 8) | 1 to 2 |
| kernel pool | under 1 MiB | under 1 MiB |

**Soak.** One process created an IP adapter, gave it an address, opened a
session, moved traffic both ways, closed it and deleted the adapter, over and
over for an hour: 1081 cycles without a failure. Its handles stayed at 249 and
its threads at 6 to 8; private memory went from 6.6 to 7.4 MiB in the first 45
minutes and stayed there, which is the heap and system caches settling rather
than a leak per cycle. The driver's own allocations are checked for leaks
under Driver Verifier in the nightly lane.

## Notes

- **One flow, one core.** Windows processes a received packet on the thread
  that indicates it, so a single flow is limited to one core (about 650k
  packets per second here). Many flows spread over up to four cores; several
  sending threads each get their own ring as well.
- **Thread priority.** Run sending threads at normal priority. A sender below
  normal priority competes with the driver's indicating threads and loses:
  64 flows measured 1.02M pps from a below-normal scheduled task.
- **Large packets.** At MTU 65535 the sending side saves CPU, but if the
  tunnel path is 1500 the receiving side still handles 1500-byte packets; the
  coalescer recovers about two thirds of the difference. In the TCP benchmark
  the remaining limit is the reflector, one thread that receives, cuts, merges
  and injects every packet.
- **Regressions.** `tools/ci/vm-run.sh run full` compares every build with a
  recorded baseline for the machine (`tools/ci/bench-baseline.json`) and
  fails on a drop beyond its tolerance.
