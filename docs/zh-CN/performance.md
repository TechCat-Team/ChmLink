# 性能

[English](../performance.md) | **简体中文**

所有数据都来自同一台测试虚拟机：宿主机 AMD Ryzen 9 9950X，虚拟机 4 个 vCPU、32 GB 内存，
Windows 11 build 26200，驱动包 0.5.1.0，取三次运行的中位数。它们衡量的是同一台机器上的网卡和 Windows 协议栈；
没有加密，也没有物理网卡，真实的隧道还会有它自己的开销。

## 方法

`bench/packet_bench.c` 测量包速率和延迟：Windows UDP 套接字通过网卡发送，一个线程从环形队列接收（*来自 Windows*）；
注入线程发送 UDP 包，由 Windows 套接字接收（*发往 Windows*）。`bench/tcp_bench.c` 让 TCP 流量经过协议栈：
一个反射线程从网卡接收每个包，改写地址后再注入回去，这样一台机器就承载了每条连接的两端。
同样的程序通过 `bench/wintun_shim.c` 和 `bench/tap_shim.c` 在 Wintun 和 tap-windows6 上运行；
运行脚本见 [development.md](development.md#基准测试)。

## 与 Wintun 和 tap-windows6 对比

Wintun 0.14.1（官方签名的 `wintun.dll`），以及 TUN 模式的 tap-windows6 9.27.0（官方签名的驱动；
保持 64 个读请求在途，就像经过调优的应用那样）。三者在同一台虚拟机上轮流运行，各三轮，
除特别注明外 MTU 均为 1500，均为普通优先级。

| 场景 | ChmLink | Wintun | tap-windows6 |
|---|---|---|---|
| 64 字节往返，中位数 / 99 分位 | 14.6 / 77 µs | 14.7 / 56 µs | 41 / 178 µs |
| UDP 来自 Windows，一个发送方，64 字节 | 47.9 万 pps | 47.3 万 pps | 4.5 万 pps |
| UDP 来自 Windows，四个发送方，64 字节 | 124 万 pps | 123 万 pps | 14.8 万 pps |
| UDP 发往 Windows，单线程，1 条流，64 字节 | 55 万 pps | 35 万 pps | 7.2 万 pps |
| UDP 发往 Windows，单线程，64 条流，64 字节 | 175 万 pps | 34.8 万 pps | 7.4 万 pps |
| UDP 发往 Windows，两个线程，64 字节 | 118 万 pps | 35.2 万 pps | 7.2 万 pps |
| 发往 Windows 每个包的 CPU 时间 | 2.3 µs | 3.8 µs | 16.4 µs |
| TCP，1 条连接 / 4 条连接 | 21.0 / 23.9 Gbps | 4.2 / 4.2 Gbps | 0.89 / 0.90 Gbps |
| TCP，MTU 65535，包整体传递 | 97 Gbps | 52 Gbps | — |

来自 Windows 的方向和往返延迟上，ChmLink 和 Wintun 持平：这里的速度由 Windows 协议栈和发送程序决定。
发往 Windows 的方向上，ChmLink 不复制、成批地交出数据包，并按流和按发送线程分到多个核心；Wintun 只用一个线程交出。
tap-windows6 每个读写请求只传一个包，落后一个数量级。

## ChmLink 详细数据

| 场景 | 结果 |
|---|---|
| UDP 来自 Windows，一个发送方，64 / 1400 字节 | 47.5 万 / 46.3 万 pps |
| UDP 来自 Windows，四个发送方，64 字节 | 126 万 pps |
| UDP 发往 Windows，单线程，1 条流，每批 1 / 32 个包 | 66.4 万 / 61.6 万 pps |
| UDP 发往 Windows，单线程，64 条流 | 178 万 pps |
| UDP 发往 Windows，两个线程 | 120 万 pps |
| 双向同时，64 字节 | 发往 Windows 62.4 万，来自 Windows 45.8 万 |
| 64 字节往返，中位数 / 99 分位 | 14.6 / 70 µs |
| TCP，IP 模式，1 条 / 4 条连接 | 23.6 / 26.0 Gbps |
| TCP，IP 模式，MTU 65535，注入前切成 1500 | 20.8 Gbps |
| ……再由 `chmlink_coalescer` 合并 | 34.6 Gbps |
| ……整体注入 | 98 Gbps |
| TCP，以太网模式，不启用卸载 | 19.9 Gbps |
| TCP，以太网模式，1500 字节的帧由 `chmlink_coalescer` 合并成 RSC | 33.6 Gbps |
| TCP，以太网模式，LSO 和 RSC，1 条 / 4 条连接 | 116 / 96 Gbps |

使用默认的 4 MiB 环形队列时，即使接收方每 100 ms 停顿 20 ms，仍能收到 85% 到 100% 的流量，具体取决于包的大小。

## 说明

- **一条流，一个核心。** Windows 在交出数据包的线程上处理它，所以单条流最多用满一个核心（在这里约每秒 65 万个包）。
  多条流会分散到最多四个核心；多个发送线程也各有自己的环形队列。
- **线程优先级。** 发送线程请使用普通优先级。低于普通优先级的发送方会与驱动的交付线程争抢 CPU 并落于下风：
  以低于普通优先级运行的计划任务测得 64 条流只有 102 万 pps。
- **大包。** MTU 65535 时发送端能省下 CPU，但如果隧道路径是 1500，接收端处理的仍是 1500 字节的包；
  合并器能挽回约三分之二的差距。在 TCP 基准中剩下的瓶颈是反射线程：它一个线程要接收、切分、合并并注入每一个包。
- **性能回退。** `tools/ci/vm-run.sh run full` 会把每次构建与该机器记录的基线（`tools/ci/bench-baseline.json`）对比，
  下降超过容差即判定失败。
