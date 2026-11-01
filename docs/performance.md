# Performance

All figures come from one test VM: an AMD Ryzen 9 9950X host, 4 vCPUs and
32 GB for the guest, Windows 11 build 26200, package 0.5.1.0, medians of three
runs. They measure the adapter and the Windows stack on one machine; there is
no encryption and no physical NIC, so a real tunnel adds its own costs.

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

## ChmLink in detail

| scenario | result |
|---|---|
| UDP from Windows, one sender, 64 / 1400 B | 475k / 463k pps |
| UDP from Windows, four senders, 64 B | 1.26M pps |
| UDP to Windows, one thread, one flow, batches of 1 / 32 / 128 | 664k / 616k / 659k pps |
| UDP to Windows, one thread, 64 flows | 1.78M pps |
| UDP to Windows, two threads | 1.20M pps |
| both directions at once, 64 B | 624k to / 458k from Windows |
| 64 B round trip, median / 99th percentile | 14.6 / 70 µs |
| TCP, IP mode, one / four streams | 23.6 / 26.0 Gbps |
| TCP, IP mode at MTU 65535, cut to 1500 before injecting | 20.8 Gbps |
| … and merged again by `chmlink_coalescer` | 34.6 Gbps |
| … injected whole | 98–106 Gbps |
| TCP, Ethernet mode, no offloads, one / four streams | 19.9 / 25.3 Gbps |
| TCP, Ethernet mode, 1500-byte frames merged into RSC by `chmlink_coalescer` | 33.6 / 29.4 Gbps |
| TCP, Ethernet mode, LSO and RSC, one / four streams | 116 / 96 Gbps |

A consumer that stalls for 20 ms every 100 ms still receives 85% to 100% of the
offered rate with the default 4 MiB ring, depending on packet size.

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
