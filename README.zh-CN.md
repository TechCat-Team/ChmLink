# ChmLink

[English](README.md) | **简体中文**

[![Check](https://github.com/TechCat-Team/ChmLink/actions/workflows/check.yml/badge.svg)](https://github.com/TechCat-Team/ChmLink/actions/workflows/check.yml)
[![Driver](https://github.com/TechCat-Team/ChmLink/actions/workflows/driver.yml/badge.svg)](https://github.com/TechCat-Team/ChmLink/actions/workflows/driver.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**高性能的 Windows 虚拟网卡。** 一个驱动同时提供 IP 网卡（TUN）和以太网网卡（TAP）；
一个 DLL 让你的 VPN、组网工具或抓包工具创建网卡，并通过它们收发数据包。

> **预发布版本。** ChmLink 还没有正式签名，只能安装在开启了测试签名的机器上。
> 1.0 之前接口仍可能变化。

## 为什么选择 ChmLink

- **发往 Windows 的方向很快。** 数据包经由共享内存环形队列传递，不需要逐包复制，
  也不需要逐包系统调用；驱动按流把数据包分到最多四个 CPU 核心交给协议栈，
  和物理网卡的 RSS 一样：单线程每秒 180 万个包，TCP 吞吐是 Wintun 的 5 倍。
- **TUN 和 TAP 合一。** 三层隧道用 IP 网卡，二层隧道用带 MAC 地址的以太网网卡，两种可以同时存在。
- **大包通路。** 以太网网卡支持 LSO、校验和卸载和 RSC；IP 网卡的 MTU 最大 65535；
  软件 GRO 会在 Windows 收到之前把隧道里的 1500 字节小包合并成大包。
- **只需附带一个文件。** `chmlink.dll` 内置已签名的驱动，会自行安装、升级或复用驱动。
  网卡会设置好 Windows 中的名称并记录所属程序，创建失败时自动清理。
- **多个应用可以共存。** 附带不同 SDK 版本的应用可以装在同一台机器上：较新的驱动生效，
  每个 SDK 都能继续使用同一主版本下更新的驱动。
- **经过严格测试。** 在 Driver Verifier、随机与系统性低资源故障注入、IOCTL 与环形队列模糊测试下验证，
  并有性能基准把关，防止性能回退。
- **MIT 许可**，纯 C 实现，支持 Windows 10 1607 及以后版本（x64）。

## 性能

同一台虚拟机、同样的测试程序，MTU 1500（细节和方法见 [docs/zh-CN/performance.md](docs/zh-CN/performance.md)）：

| | ChmLink | Wintun 0.14.1 | tap-windows6 9.27.0 |
|---|---|---|---|
| UDP 发往 Windows，单线程，64 条流 | **181 万 pps** | 34.8 万 pps | 7.4 万 pps |
| UDP 发往 Windows，单线程，1 条流 | **57.8 万 pps** | 35 万 pps | 7.2 万 pps |
| UDP 来自 Windows，单个发送方 | 48.0 万 pps | 47.3 万 pps | 4.5 万 pps |
| TCP，单条连接 | **23.1 Gbps** | 4.2 Gbps | 0.89 Gbps |
| TCP，MTU 65535 | **100 Gbps** | 52 Gbps | — |
| 64 字节往返延迟（中位数） | 14.4 µs | 14.7 µs | 41 µs |

## 快速上手

```c
#include <chmlink/api.h>

chmlink_create_options options = { sizeof(options) };
chmlink_device device;
options.mode = CHMLINK_MODE_IP;              /* or CHMLINK_MODE_ETHERNET */
options.name = L"My VPN";                    /* the Windows connection name */
chmlink_create_adapter(&options, &device);   /* installs the driver if needed */

chmlink_adapter *adapter = chmlink_open(device.interface_index);
/* add an address (CreateUnicastIpAddressEntry), then: */
for (;;) {
    uint32_t size;
    const uint8_t *packet;
    chmlink_wait_readable(adapter, CHMLINK_WAIT_INFINITE);
    while ((packet = chmlink_receive_packet(adapter, &size, NULL)) != NULL) {
        /* a packet Windows sent: encrypt and forward it */
        chmlink_release_packet(adapter, packet);
    }
}
/* packets for Windows: chmlink_allocate_packet, fill in, chmlink_send_packet */

chmlink_close(adapter);
chmlink_delete_adapter(device.instance_id, NULL);
```

[`examples/quickstart.c`](examples/quickstart.c) 是一个完整的程序：它创建一块 IP 网卡，
响应 `ping 192.0.2.2`，退出时删除网卡。其他内容见 [SDK 指南](docs/zh-CN/sdk.md)。

## 构建

需要：Windows 10 或更高版本（x64）、安装了 C++ 工作负载的 Visual Studio 2022、
CMake 3.16+、PowerShell 5.1。WDK 由构建脚本自动下载。

```powershell
powershell -File tools\build-driver.ps1 -Restore    # driver -> build\driver
# sign it for your test machine (docs/development.md), then:
cmake -S . -B build -A x64 -DCHMLINK_DRIVER_DIR=C:\path\to\signed
cmake --build build --config Release
cmake --install build --config Release --prefix C:\sdk\chmlink   # bin\chmlink.dll, lib\chmlink.lib, include\chmlink
```

测试签名的方法见 [docs/zh-CN/development.md](docs/zh-CN/development.md)。

## 文档

| | |
|---|---|
| [SDK 指南](docs/zh-CN/sdk.md) | 创建网卡、会话、收发数据包、卸载、GRO、错误处理 |
| [架构](docs/zh-CN/architecture.md) | 驱动和 SDK 的工作原理：环形队列、按流分发、卸载、设备契约 |
| [管理](docs/zh-CN/administration.md) | `chmlink-device`、PowerShell 管理脚本、可回滚的驱动升级 |
| [开发](docs/zh-CN/development.md) | 构建、测试签名、测试套件、版本规则 |
| [性能](docs/zh-CN/performance.md) | 基准结果、与 Wintun 和 tap-windows6 的对比、测试方法 |
| [运行时 CI](tools/ci/README.md)（英文） | 测试虚拟机流水线：smoke、full 和 nightly 三条线 |

## 状态

已完成：两种网卡、按流分发的共享环形队列数据通路、卸载、可配置 MTU、软件 GRO、
SDK 中的网卡管理与驱动安装（会话打开时的升级、并发创建、干净卸载）、版本协商、意外移除，
以及在 Driver Verifier 和故障注入下的验证。已在 Windows 11 和最低支持版本 Windows 10 1607 上测试，
后者未安装 Visual C++ 运行库。

1.0 之前：正式签名、在支持睡眠的真机上测试睡眠与唤醒（测试虚拟机不支持睡眠）、ARM64 和 x86 构建。

## 参与贡献

欢迎提交 Issue 和 Pull Request。请先阅读 [docs/zh-CN/development.md](docs/zh-CN/development.md)：
驱动改动需要提高 `DriverVer`，提交信息遵循 Conventional Commits。

## 许可证

[MIT](LICENSE)
