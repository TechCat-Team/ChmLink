# 开发

[English](../development.md) | **简体中文**

构建、测试和修改 ChmLink。

## 目录结构

| 路径 | 内容 |
|---|---|
| `include/chmlink/` | 公开头文件（`api.h`、`manage.h`、`packet.h`、`protocol.h`、`offload.h`、`gro.h`、`version.h`）和驱动契约（`device.h`、`query.h`、`session.h`、`ring.h`、`config.h`） |
| `driver/` | NDIS miniport 驱动及其 INF |
| `src/windows/` | SDK（`chmlink.dll`，导出函数列在 `chmlink.def`） |
| `src/ring/`、`src/protocol/`、`src/packet/` | 驱动和 SDK 共用的可移植代码 |
| `examples/` | 使用 SDK 的小程序 |
| `tools/` | `chmlink-device`、PowerShell 管理脚本、驱动构建脚本、测试和基准运行脚本 |
| `tools/ci/` | 用于 Windows 测试虚拟机的运行时 CI（[README](../../tools/ci/README.md)，英文） |
| `tests/unit/` | 可移植的测试（CTest）和批量格式模糊测试器 |
| `tests/windows/` | 在测试虚拟机上运行的运行时测试 |
| `bench/` | 包速率、延迟和 TCP 基准测试，带 Wintun 和 tap-windows6 后端 |

## 构建

用户态部分（SDK、工具、测试、示例），使用 Visual Studio 2022：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

可移植部分及其测试也能在 Linux 上构建（`cmake -S . -B build && cmake --build build && ctest --test-dir build`）。

驱动（首次使用时会下载固定版本的 WDK 和 SDK 包）：

```powershell
powershell -File tools\build-driver.ps1 -Restore -Analyze
```

它在 `build/driver/` 中生成未签名的 `chmlink.sys`、INF 和编录文件，校验 INF（`/u`，以及针对 build 25319
及以后版本的 `/w` 隔离检查），加上 `-Analyze` 时还会对驱动源码运行 PREfast，有发现即失败。驱动使用
`/W4 /WX` 编译。

### 为测试机签名

在正式签名就绪之前，只能安装测试签名的驱动包，安装到开启了测试签名（`bcdedit /set testsigning on`）
并有可丢弃快照的机器上。先签名 `chmlink.sys`，用 Inf2Cat 重新生成编录文件，再签名编录文件：

```powershell
signtool sign /fd sha256 /sha1 <test-cert-thumbprint> build\driver\chmlink.sys
Inf2Cat /driver:build\driver /os:10_X64,Server10_X64
signtool sign /fd sha256 /sha1 <test-cert-thumbprint> build\driver\chmlink.cat
```

把 `CHMLINK_DRIVER_DIR` 指向这个目录，即可把驱动包内置进 `chmlink.dll`（[sdk.md](sdk.md#获取-sdk)）。

## 测试

| 内容 | 方法 |
|---|---|
| 可移植的单元测试 | `ctest`（见上）；包括环形队列、批量格式、查询和会话编码、卸载辅助函数、`chmlink_ip_split`、GRO、流哈希、配置 |
| 批量格式模糊测试 | `clang -fsanitize=fuzzer,address,undefined -Iinclude src/packet/packet.c src/protocol/batch.c tests/unit/fuzz_batch.c` |
| CI 脚本 | `python3 -m unittest discover -s tools/ci` |
| 运行时测试套件 | 在测试虚拟机上，通过 `tools/ci/vm-run.sh`（见下文）或手动运行 |

每次推送到 GitHub 都会在 Linux 和 Windows 上运行可移植测试、模糊测试、带 PREfast 的驱动构建、
PowerShell 语法检查，以及下文的版本规则检查。

### 运行时测试

这些测试会改动机器（网卡、地址、睡眠），应在装有已签名驱动包的可丢弃虚拟机上运行：

| 运行脚本 | 覆盖内容 |
|---|---|
| `tools\test-lifecycle.ps1 -InfPath <inf> -BuildDirectory <dir> -OutputDirectory <new>` | 等待、取消、关闭时的竞争、环形队列写满、进程退出、禁用和移除、16 线程同时关闭、IOCTL 和环形队列内容的模糊测试，可选睡眠与唤醒（`-PowerCycle`） |
| `tools\test-ip.ps1 -InfPath <inf> -OutputDirectory <new>` | MTU 9000 的 IP 网卡上 IPv4 和 IPv6 往返、以太网隔离、MTU 修改、命名 |
| `tools\test-lowres.ps1` | 低资源模拟下反复创建删除网卡并进行模糊测试（nightly 线使用） |
| `chmlink-arp-test <ifIndex> <local-ipv4> <peer-ipv4>` | 通过 ARP 测试以太网网卡的双向收发 |
| `chmlink-quickstart --self-test` | 用内置驱动包创建网卡、一次 ping 往返、删除 |
| `tools\test-package.ps1` | 通过 `chmlink.dll`：有无打开会话时的驱动升级、新旧驱动包混装时的修复、`chmlink_delete_driver`、八个进程同时创建、打开会话时的意外移除（借助仅供测试的过滤驱动 `tests/windows/surprise_filter.c`） |

`tools/ci/vm-run.sh` 可以从 Linux 或 macOS 机器驱动以上全部测试：它在虚拟机上构建某个提交，用虚拟机的测试证书签名，
覆盖安装到基线驱动之上，运行测试套件，然后恢复基线。共三条线：`smoke`（约 1.5 分钟）、`full`（另加三轮基准测试，
与记录的基线对比把关）、`nightly`（另加 Driver Verifier、随机和系统性低资源模拟、泄漏检查和重启）。
详见 [tools/ci/README.md](../../tools/ci/README.md)（英文）。

### 基准测试

```powershell
.\tools\bench.ps1 -InfPath <signed-inf> -BuildDirectory build\Release -OutputDirectory <new>
.\tools\bench-wintun.ps1 -WintunDll <official wintun.dll> -BuildDirectory build\Release -OutputDirectory <new>
.\tools\bench-tap.ps1 -BuildDirectory build\Release -OutputDirectory <new>   # needs one TAP-Windows Adapter V9
```

`bench-tap.ps1` 需要已安装一块 TAP-Windows Adapter V9。同样的基准程序通过 `bench/wintun_shim.c` 和
`bench/tap_shim.c` 在三种驱动上运行。结果和方法见 [performance.md](performance.md)。

## 版本规则

- **驱动改动需要提高 `DriverVer`。** Windows 按 INF 识别已发布的驱动包：二进制换了而 `DriverVer` 不变，
  新驱动会被悄悄地不安装。只要编译进驱动的任何内容有改动（`driver/`、`src/ring/`、`src/protocol/`、
  `src/packet/`、契约头文件），就同时提高 `driver/chmlink.inf` 中的 `DriverVer` 和 `include/chmlink/version.h`
  中的 `CHMLINK_VERSION_*`。`version_test` 保证两者一致；CI 会拒绝没有提高版本的驱动改动
  （`tools/ci/check_driver_version.py`）。
- **`DriverVer` 的日期用 UTC 日期。** Inf2Cat 按构建机器的时钟拒绝未来的日期，而托管的 CI 机器使用 UTC。
- **设备契约只增不减。** 跨越内核边界的内容的修改遵循 [architecture.md](architecture.md#设备契约) 中的规则；
  新契约提高 `CHMLINK_DEVICE_VERSION`，只有主版本升级时才提高 `CHMLINK_DEVICE_VERSION_MIN`。
- **配置格式带版本号**（`include/chmlink/config.h`）；驱动会继续读取旧版本。

提交信息遵循 Conventional Commits（`feat(sdk): ...`、`fix(driver): ...`，破坏性改动加 `!`）。
