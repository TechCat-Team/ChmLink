# ChmLink SDK

[English](../sdk.md) | **简体中文**

SDK 是一个 C 库 `chmlink.dll`，头文件在 `include/chmlink/`。它负责创建和删除虚拟网卡、
安装网卡所需的驱动，并在你的应用与 Windows 网络协议栈之间传递数据包。

- [获取 SDK](#获取-sdk)
- [概念](#概念)
- [约定](#约定)
- [网卡](#网卡)：创建、列出、修改、删除
- [会话](#会话)：打开、查询、关闭
- [接收来自 Windows 的数据包](#接收来自-windows-的数据包)
- [向 Windows 发送数据包](#向-windows-发送数据包)
- [等待](#等待)
- [卸载（以太网模式）](#卸载以太网模式)
- [大 MTU 与软件 GRO](#大-mtu-与软件-gro)
- [数据包辅助函数](#数据包辅助函数)
- [批量格式](#批量格式)
- [诊断与版本](#诊断与版本)
- [驱动兼容性](#驱动兼容性)
- [错误](#错误)
- [示例](#示例)

## 获取 SDK

用 CMake 和 Visual Studio 2022（x64）构建：

```powershell
cmake -S . -B build -A x64 -DCHMLINK_DRIVER_DIR=C:\path\to\signed\package
cmake --build build --config Release
cmake --install build --config Release --prefix C:\sdk\chmlink
```

`CHMLINK_DRIVER_DIR` 指向一个包含已签名的 `chmlink.inf`、`chmlink.sys` 和 `chmlink.cat` 的目录
（驱动的构建和签名见 [development.md](development.md)）。这样 DLL 会把驱动包作为资源内置，
并自行安装，应用只需附带 `chmlink.dll` 一个文件。C 运行库已链接进 DLL，只有每个 Windows 10
都自带的 Universal CRT 除外，所以不需要安装 Visual C++ 运行库。不指定 `CHMLINK_DRIVER_DIR`
构建时，DLL 的功能完全一样，只是 `chmlink_create_adapter` 需要传入 INF 的路径。

安装步骤生成：

| 路径 | 内容 |
|---|---|
| `bin/chmlink.dll` | 库本身，带版本资源 |
| `lib/chmlink.lib` | 导入库 |
| `include/chmlink/*.h` | `api.h`（只需包含它）以及它引用的头文件 |

```c
#include <chmlink/api.h>   /* link chmlink.lib */
```

创建网卡和打开会话需要管理员（或 SYSTEM）权限。

## 概念

**网卡（adapter）。** Windows 视为网卡的虚拟网络接口。每块网卡有一个在创建时确定的模式：

| 模式 | Windows 看到的 | 你的应用收发的 |
|---|---|---|
| `CHMLINK_MODE_IP` | 点对点接口（类似 TUN） | IPv4 和 IPv6 数据包 |
| `CHMLINK_MODE_ETHERNET` | 带 MAC 地址的以太网网卡（类似 TAP） | 以太网帧（不含 FCS，允许 VLAN 标签） |

**会话（session）。** 通往一块网卡的数据通路。每块网卡同一时间只能有一个会话；
关闭会话或进程退出时会话结束。两次会话之间，网卡仍然存在并保留地址，但不传输流量。

**方向。** *来自 Windows* 是协议栈通过网卡发出的流量（由你接收）。*发往 Windows*
是你注入的流量（由协议栈接收）。`chmlink_adapter_info` 中的计数器沿用网卡驱动的叫法：
`tx` 表示来自 Windows，`rx` 表示发往 Windows。

**环形队列（ring）。** 会话通过共享内存传递数据包：一个来自 Windows 的环形队列，
最多四个发往 Windows 的环形队列。SDK 负责分配，驱动直接读写，不需要每个包一次系统调用。

**所有者（owner）。** 每块网卡记录一个所有者 GUID。删除网卡或修改其 MTU 需要相同的所有者，
这样不同应用不会删掉彼此的网卡。请为你的应用选定一个固定的 GUID。

## 约定

- 返回 `int` 的函数成功时返回非零，失败时返回零；返回指针的函数失败时返回 `NULL`。
  失败原因通过 `GetLastError()` 获取（见[错误](#错误)）。
- 带 `size` 字段的结构体必须把它设为该结构体的 `sizeof`；不设置的字段一律置零。
- 字符串使用 UTF-16（`wchar_t`），`interface_id`（ASCII 格式的 GUID 文本）除外。
- 会话函数可以按各函数的说明从多个线程同时调用。关闭会话前必须停止对它的所有调用，
  并且只能关闭一次。

## 网卡

声明在 `chmlink/manage.h`。

### 创建

```c
static const GUID my_owner = { 0x1b2c3d4e, 0x5f60, 0x4718, {0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0} };

chmlink_create_options options = { sizeof(options) };
chmlink_device device;
options.mode = CHMLINK_MODE_IP;
options.name = L"My VPN";
options.owner = &my_owner;
if (!chmlink_create_adapter(&options, &device))
    fail(GetLastError());
/* device.interface_index, device.interface_guid, device.instance_id ... */
```

`chmlink_create_options`：

| 字段 | 含义 | 为零或 NULL 时的默认值 |
|---|---|---|
| `size` | `sizeof(chmlink_create_options)` | 必填 |
| `mode` | `CHMLINK_MODE_IP` 或 `CHMLINK_MODE_ETHERNET` | 以太网 |
| `name` | Windows 中的连接名称，最多 128 个字符 | `ChmLink-<网卡 ID>` |
| `owner` | 所有者 GUID | `CHMLINK_DEFAULT_OWNER_ID`（供工具使用；应用请用自己的） |
| `mac` | 6 字节，仅以太网；必须是单播且本地管理的地址 | 随机的本地管理地址 |
| `mtu` | 仅 IP 模式，1280 到 65535 | 1500 |
| `driver_inf` | 从哪个已签名的 `chmlink.inf` 安装 | `chmlink.dll` 内置的驱动包 |

`chmlink_create_adapter` 在需要时安装驱动，创建设备，等待 Windows 为它建立网络接口，
并给接口命名。成功时 `device`（可以为 `NULL`）描述新网卡。如果 `device.flags` 含有
`CHMLINK_DEVICE_REBOOT_REQUIRED`，表示 Windows 要求重启后网卡才能工作。

失败时不会留下任何东西。唯一的例外是 `ERROR_INSTALL_FAILURE`：创建失败，而且删除创建了一半的设备也失败了；
这个设备会出现在 `chmlink_enumerate_devices` 中，可以删除。

常见错误：`ERROR_DUP_NAME`（名称、MAC 或身份已被占用）、`ERROR_REVISION_MISMATCH`
（驱动包不可用，见[驱动兼容性](#驱动兼容性)）、`ERROR_BUSY`（另一个进程正在修改网卡超过十秒，
或者现在无法更换驱动，见下文）、`ERROR_INVALID_PARAMETER`。

名称在网卡删除之前一直被占用。对于从未成功启动就被删除的网卡，Windows 可能保留其名称直到下次重启；
SDK 能识别这种残留，并重新使用该名称。

创建网卡不会配置地址或路由；请对 `device.interface_index` 或接口 LUID 使用 IP Helper API
（`CreateUnicastIpAddressEntry`、`CreateIpForwardEntry2`）或 `netsh`。先打开会话，再添加地址：
链路随会话一起启动。

### 安装哪个驱动

一台机器上的所有 ChmLink 网卡共用一个驱动。`driver_inf` 为 `NULL` 时，SDK 会比较内置的驱动包和现有网卡使用的驱动包，较新的一方胜出：

- 还没有网卡：安装内置的驱动包；
- 现有驱动包版本相同或更新：直接使用；
- 内置驱动包更新：先把所有现有网卡升级到它（它们的会话会结束），除非其中有网卡正打开着会话；
  这种情况下保留现有驱动包，前提是本 SDK 能与它通信；
- 各网卡使用不同的驱动包（升级中途被打断）：全部统一到最新的驱动包；
  如果其中有网卡正打开着会话，创建会以 `ERROR_BUSY` 失败，且不做任何改动。

指定了 `driver_inf` 时，现有网卡必须已经在使用完全相同的驱动包（INF、SYS 和 CAT 都相同）；
不会改动其他任何东西。

### 列出和读取

```c
static int show(const chmlink_device *d, void *context)
{
    wprintf(L"%ls  %ls  index %u  mtu %u\n", d->instance_id, d->name, d->interface_index, d->mtu);
    return 1;   /* 0 stops */
}

chmlink_enumerate_devices(&my_owner, show, NULL);   /* NULL owner: all adapters */
chmlink_get_device(L"ROOT\\NET\\0001", &device);
```

`chmlink_device`：

| 字段 | 含义 |
|---|---|
| `mode`、`mtu` | 配置值；MTU 从设备下次启动时生效 |
| `interface_index` | Windows 还没有为网卡建立接口时为 0；重启后可能变化 |
| `interface_guid` | Windows 接口 GUID（未知时为零） |
| `mac` | 以太网 MAC 地址；IP 网卡为零 |
| `adapter_id`、`owner_id` | 网卡的固定身份及其所有者 |
| `name` | 创建时指定的名称 |
| `instance_id` | 设备实例 ID，删除和修改 MTU 时用它指定网卡 |
| `flags` | 填写本结构的那次调用返回的 `CHMLINK_DEVICE_REBOOT_REQUIRED` |

枚举会跳过无法读取配置的设备。对没有配置的 ChmLink 设备，`chmlink_get_device` 以
`ERROR_FILE_NOT_FOUND` 失败；对不是 ChmLink 的设备，以 `ERROR_ACCESS_DENIED` 失败。

### 修改 MTU

```c
uint32_t flags;
chmlink_set_adapter_mtu(device.instance_id, &my_owner, 9000, &flags);
```

仅限 IP 网卡，范围 1280 到 65535；以太网网卡固定为 1500。网卡会重启，会话随之结束。
如果新值已保存但重启失败，调用以 `ERROR_INSTALL_FAILURE` 失败，MTU 在下次重启时生效。
见[大 MTU 与软件 GRO](#大-mtu-与软件-gro)。

### 删除

```c
chmlink_delete_adapter(device.instance_id, &my_owner);
chmlink_delete_driver();   /* in an uninstaller, after the last adapter */
```

删除网卡会结束其会话。`chmlink_delete_driver` 从驱动存储（Driver Store）中删除所有未被网卡使用的
ChmLink 驱动包；如果仍有网卡存在，它会在删除未使用的驱动包之后以 `ERROR_INF_IN_USE_BY_DEVICES` 失败。
最后一块网卡删除后再调用，它不会留下任何东西：除了驱动包，还会删除驱动服务，以及在会把驱动复制到
`System32\drivers` 的 Windows 版本上（build 25319 之前）删除那份副本。

网卡的修改操作在所有进程之间串行进行：一次调用最多等待另一次调用十秒，超时则以 `ERROR_BUSY` 失败。

## 会话

声明在 `chmlink/api.h`。

```c
chmlink_adapter *adapter = chmlink_open(device.interface_index);
chmlink_adapter_info info;
chmlink_query(adapter, &info);
/* ... */
chmlink_close(adapter);
```

| 函数 | |
|---|---|
| `chmlink_open(index)` | 按接口索引打开，环形队列各 4 MiB |
| `chmlink_open_ex(index, &options)` | `chmlink_open_options.ring_capacity`：每个环形队列的字节数，128 KiB 到 64 MiB 之间的 2 的幂 |
| `chmlink_open_by_id("{GUID}")` | 按接口 GUID 打开（网卡存在期间不变） |
| `chmlink_close(adapter)` | 取消等待，让进行中的调用完成，结束会话，释放所有资源 |
| `chmlink_query(adapter, &info)` | 网卡的状态和计数器 |
| `chmlink_inspect(index, &info)` | 同上，但不打开会话 |
| `chmlink_enumerate(callback, context)` | 每块网卡的 `chmlink_adapter_info` |

打开会话失败的情况：已有其他会话打开时为 `ERROR_SHARING_VIOLATION`，没有管理员权限时为
`ERROR_ACCESS_DENIED`，驱动无法为本 SDK 提供服务时为 `ERROR_REVISION_MISMATCH`。

`chmlink_adapter_info`：

| 字段 | 含义 |
|---|---|
| `mode`、`mtu`、`packet_limit` | 数据包格式和最大包长（IP：等于 MTU；以太网：1522 字节，含 VLAN 标签） |
| `mac`、`interface_index`、`interface_id` | 身份信息 |
| `state` | `CHMLINK_STATE_*` 标志位，见下表 |
| `capabilities` | `CHMLINK_CAP_*` 标志位；`CHMLINK_CAP_OFFLOAD` 表示可以请求卸载功能 |
| `offload` | 本会话生效的卸载功能 |
| `version`、`min_version` | 驱动支持的设备契约版本范围 |
| `ring_capacity`、`ring_pending_bytes` | 来自 Windows 的环形队列大小，以及其中尚未读取的字节数 |
| `packet_filter` | Windows 的数据包过滤设置（以太网） |
| `tx_packets`、`tx_bytes`、`tx_errors`、`tx_queue_drops` | 来自 Windows；drops 是遇到环形队列已满而丢弃的包 |
| `rx_packets`、`rx_bytes`、`rx_discards` | 发往 Windows；discards 是驱动拒绝的包 |

状态：

| 标志位 | 含义 |
|---|---|
| `CHMLINK_STATE_RUNNING` | 数据通路在运行 |
| `CHMLINK_STATE_SESSION` | 有会话打开 |
| `CHMLINK_STATE_PAUSED`、`CHMLINK_STATE_PAUSING` | Windows 暂停了网卡（配置变更、禁用） |
| `CHMLINK_STATE_LOW_POWER` | 系统处于睡眠；唤醒后数据通路恢复 |
| `CHMLINK_STATE_SHUTDOWN` | 系统正在关机 |
| `CHMLINK_STATE_REMOVING` | 网卡正在被移除；会话即将结束 |

网卡暂停或睡眠期间，等待会以 `ERROR_NOT_READY` 失败，数据包也不会传递；保留会话，再次等待即可。

## 接收来自 Windows 的数据包

零拷贝：数据包在你释放之前一直留在环形队列中。

```c
uint32_t size;
const uint8_t *packet;
while ((packet = chmlink_receive_packet(adapter, &size, NULL)) != NULL) {
    handle(packet, size);
    chmlink_release_packet(adapter, packet);
}
if (GetLastError() != ERROR_NO_MORE_ITEMS) fail(GetLastError());
```

- 返回 `NULL` 且错误为 `ERROR_NO_MORE_ITEMS` 表示环形队列已空：请[等待](#等待)。
- 数据包可以按任意顺序释放；环形队列的空间只回收到最早一个未释放的包为止，所以不要长时间持有某个包。
- 会话启用了卸载功能时，第三个参数接收卸载描述符（见下文）；否则传 `NULL`。
- 环形队列满时，Windows 之后发出的包会被丢弃，计入 `tx_queue_drops`；4 MiB 的环形队列能吸收几十毫秒的停顿。

`chmlink_receive(adapter, buffer, capacity, &bytes)` 则把最多 256 个包复制到一个[批量格式](#批量格式)的缓冲区中。

## 向 Windows 发送数据包

零拷贝：在环形队列中预留空间，写入数据包，然后发布。

```c
uint8_t *slot = chmlink_allocate_packet(adapter, size, NULL);
if (slot == NULL) {
    /* ERROR_BUFFER_OVERFLOW: the ring is full; the driver is behind. Retry shortly. */
}
memcpy(slot, packet, size);   /* or decrypt straight into it */
chmlink_send_packet(adapter, slot);
```

- 正好写入 `size` 字节；每次分配放一个完整的 IP 包（IP 模式）或以太网帧（以太网模式）。
  驱动会检查格式，丢弃无效的包（计入 `rx_discards`）。
- 多个线程可以同时分配和发送。每个发送线程有自己的环形队列（每个会话最多四个），
  驱动在各自的核心上把每个环形队列交给协议栈。同一线程的包按顺序到达。
- 驱动还会按流（地址、协议、端口）把数据包分到最多四个核心，和网卡的 RSS 一样：
  一个线程承载多条连接时也能用上多个核心，每条连接内部保持顺序。发送线程请使用普通或更高的优先级。
- 同一环形队列内按分配顺序发布：不要分配之后迟迟不发送。

`chmlink_inject(adapter, batch, length)` 则复制一整个[批量格式](#批量格式)的缓冲区：要么全部写入，
要么一个都不写（放不下时返回 `ERROR_BUFFER_OVERFLOW`，可以用同一批数据重试）。

## 等待

```c
for (;;) {
    if (!chmlink_wait_readable(adapter, CHMLINK_WAIT_INFINITE)) {
        DWORD e = GetLastError();
        if (e == ERROR_NOT_READY) continue;   /* paused or asleep */
        break;                                /* aborted, removed, shutting down */
    }
    /* receive until ERROR_NO_MORE_ITEMS */
}
```

- `chmlink_wait_readable(adapter, timeout_ms)` 在有来自 Windows 的数据包时返回。`0` 表示只检查不等待；
  `CHMLINK_WAIT_INFINITE` 表示不限时。它在休眠前会先短暂轮询环形队列，使高包速下的延迟保持很低。
- 每个会话只能有一个等待者（第二个会得到 `ERROR_BUSY`）。
- 失败情况：`ERROR_TIMEOUT`；`ERROR_OPERATION_ABORTED`（被取消、会话关闭或会话结束）；
  `ERROR_NOT_READY`（暂停或睡眠）；设备移除相关的错误；`ERROR_SHUTDOWN_IN_PROGRESS`。
- `chmlink_cancel_wait(adapter)` 从另一个线程唤醒当前的等待。
- `chmlink_get_read_event(adapter)` 返回等待所用的自动重置事件，供你自己的 `WaitForMultipleObjects`
  循环使用。不要关闭它。事件触发后，请一直接收到环形队列为空。

## 卸载（以太网模式）

以太网网卡可以像物理网卡一样承担分段和校验和计算。Windows 不会在 IP 模式的网卡上使用任务卸载，
所以 IP 网卡不提供卸载功能；请改用[大 MTU](#大-mtu-与软件-gro)。

```c
uint32_t granted;
chmlink_set_offload(adapter, CHMLINK_OFFLOAD_ALL, &granted);
```

| 标志位 | 作用 |
|---|---|
| `CHMLINK_OFFLOAD_TX_CHECKSUM` | Windows 交出的 TCP/UDP 包不计算校验和 |
| `CHMLINK_OFFLOAD_TX_LSO` | Windows 把最多 64 KiB 的 TCP 数据作为一个包交出（LSOv2） |
| `CHMLINK_OFFLOAD_RX_CHECKSUM` | 你可以把注入的包标记为已校验 |
| `CHMLINK_OFFLOAD_RX_RSC` | 你可以把合并后的 TCP 段作为一个包注入 |

卸载设置在会话期间有效；`granted` 给出 Windows 接受了哪些。启用卸载后，数据包带有一个
`chmlink_offload` 描述符（语义与 Linux 的 `virtio_net_hdr` 相同）：

| 字段 | 含义 |
|---|---|
| `flags` | `CHMLINK_OFFLOAD_NEEDS_CSUM`：位于 `csum_start + csum_offset` 的四层校验和尚未计算。`CHMLINK_OFFLOAD_DATA_VALID`：校验和已验证（仅用于注入） |
| `gso_type` | `CHMLINK_GSO_TCPV4` / `CHMLINK_GSO_TCPV6`：一个代表多个分段的 TCP 包 |
| `gso_size` | 每个分段的负载字节数 |
| `hdr_len` | 以太网、IP 和 TCP 头的总字节数 |
| `csum_start`、`csum_offset` | 校验和的位置（相对包起始的偏移） |

来自 Windows 的方向，`chmlink_receive_packet` 会填写描述符。如果对端需要普通的包，
`chmlink_gso_segment` 可把 GSO 包切成线路上的分段，`chmlink_offload_complete` 可补上缺失的校验和。

发往 Windows 的方向，把描述符传给 `chmlink_allocate_packet`：对你已验证校验和的包设置 `DATA_VALID`，
对同一 TCP 流合并后的分段再加上 GSO 类型（以 RSC 形式交给 Windows）。也可以让
[合并器](#大-mtu-与软件-gro)替你构造。

## 大 MTU 与软件 GRO

Windows 的开销大多是按包计算的，所以包越少、越大就越快。

**大 MTU（IP 模式）。** 创建网卡时把 `mtu` 设为最大 65535（或之后再修改）。这样 Windows 发出的
TCP 包最大可达 MTU，也接受这么大的注入包。原本由网卡承担的工作改由你的应用负责：
数据包要经过 MTU 更小的路径发出之前，用 `chmlink_ip_split` 切分：

```c
static int forward(const uint8_t *piece, size_t length, void *context) { tunnel_send(piece, length); return 1; }

uint8_t scratch[1500];
chmlink_result r = chmlink_ip_split(packet, size, 1500, next_ipv6_id++, scratch, sizeof(scratch), forward, NULL);
if (r == CHMLINK_TOO_BIG) { /* IPv4 with DF that is not TCP: drop, or answer ICMP "fragmentation needed" */ }
```

TCP 会被切成完整的分段；其他 IPv4 和 IPv6 包则被分片。使用默认的 1500 时，网卡的行为和普通网卡一样，不需要这些处理。

**软件 GRO（两种模式）。** 隧道送来的是 1500 字节的包。合并器（coalescer）在它们到达 Windows 之前，
把同一条流的连续 TCP 分段合并：在 MTU 大于 1500 的 IP 网卡上合并成一个大 IP 包，在会话启用了
`CHMLINK_OFFLOAD_RX_RSC` 的以太网网卡上合并成一个 RSC 帧。

```c
chmlink_coalescer *c = chmlink_coalescer_create(adapter, 0);   /* after chmlink_set_offload */
for (;;) {
    while (tunnel_has_packet()) {
        packet = tunnel_receive(&size);
        chmlink_coalescer_send(c, packet, size);
    }
    chmlink_coalescer_flush(c);   /* before waiting: segments are held until here */
    tunnel_wait();
}
chmlink_coalescer_free(c);        /* flushes; before chmlink_close */
```

- 其他包原样通过；每条流保持顺序。
- 每个发送线程一个合并器；它不是线程安全的。环形队列满时，send 和 flush 会等待。
- IP 模式不读取负载：合并后的校验和由各分段的校验和推算得出，Windows 仍会检查它。以太网模式会验证每个分段的校验和，
  因为 Windows 信任 RSC 数据；如果已知数据完好（例如经过认证的隧道），传入 `CHMLINK_GRO_TRUSTED` 可跳过验证。
- 无法合并时（MTU 为 1500，或没有 RSC），数据包直接通过。

`chmlink/gro.h` 以可移植、不分配内存的 API（`chmlink_gro_init`、`chmlink_gro_add`、`chmlink_gro_flush`）
提供同样的合并功能，通过输出回调交出结果，适合经由自己的通路注入的应用。

## 数据包辅助函数

可移植的函数（`chmlink/packet.h`、`chmlink/offload.h`），不需要会话：

| 函数 | |
|---|---|
| `chmlink_packet_valid(mode, data, length, limit)` | 驱动所做的格式检查 |
| `chmlink_ip_packet_valid(data, length, limit)` | 是否为一个完整的 IPv4 或 IPv6 包 |
| `chmlink_flow_hash(mode, data, length)` | 驱动用于分流的流哈希（双向对称，跳过 IPv6 扩展头；分片不计端口；非 IP 包为 0） |
| `chmlink_checksum_add(data, length, seed)` | 反码求和 |
| `chmlink_offload_complete(mode, packet, length, &offload)` | 补上 `NEEDS_CSUM` 的校验和 |
| `chmlink_gso_segment(...)` | 把 GSO 包切成线路上的分段 |
| `chmlink_ip_split(...)` | 把 IP 包切到更小的 MTU |

## 批量格式

`chmlink_receive` 和 `chmlink_inject` 使用一种简单的缓冲区格式（`chmlink/protocol.h`）：
24 字节的头，然后每个包依次是长度、可选的 12 字节描述符、按 4 字节对齐填充的包内容。
用 `chmlink_batch_begin`（或 `_begin_offload`）和 `chmlink_batch_append`（或 `_append_offload`）构造；
用 `chmlink_batch_read_begin` 和 `chmlink_batch_read_next` 读取；用 `chmlink_batch_validate` 检查。
一批最多 256 个包、1 MiB。零拷贝调用更快；批量格式适合希望每次调用传一个缓冲区的代码。

## 诊断与版本

```c
static void log_line(chmlink_log_level level, const wchar_t *message, void *context)
{
    fwprintf(stderr, L"chmlink[%d] %ls\n", level, message);
}
chmlink_set_logger(log_line, NULL);
```

没有设置日志回调时，SDK 不输出任何信息。设置后，它会报告自己做了什么（驱动已安装、已升级或被保留，
网卡已创建或删除），以及失败的原因，包括出错的步骤和 Windows 的错误描述。回调可能在任意线程上执行；
不要在回调里调用 `chmlink_set_logger`。

版本号是用 `CHMLINK_MAKE_VERSION(major, minor, patch, build)` 构造的 64 位值，可以直接按数值比较：

| 调用 | 返回 |
|---|---|
| `chmlink_get_sdk_version()` | 当前 `chmlink.dll` 的版本（也写在文件版本资源中） |
| `chmlink_get_driver_version(&v)` | 所有网卡中最新的驱动包版本（没有网卡时为 `ERROR_FILE_NOT_FOUND`） |
| `info.version`、`info.min_version` | 正在运行的驱动支持的设备契约版本 |

## 驱动兼容性

多个应用可能各自附带不同版本的 `chmlink.dll`，而所有网卡共用一个驱动。SDK 和驱动通过带编号的设备契约达成一致：
驱动支持一个契约版本范围，SDK 使用其中一个；范围包含 SDK 的版本时，会话才能打开。新契约只做增加，
并且同一主版本内，驱动会一直支持旧契约，因此：

- 另一个应用安装了同一主版本下更新的驱动后，你的应用可以继续工作；
- 创建网卡时绝不会用旧驱动替换新驱动；
- SDK 需要的驱动比已安装的更新时，以 `ERROR_REVISION_MISMATCH` 失败，并在日志中记录双方的版本。

## 错误

| `GetLastError()` | 常见原因 |
|---|---|
| `ERROR_INVALID_PARAMETER` | 参数、size 字段或选项错误 |
| `ERROR_ACCESS_DENIED` | 不是管理员；所有者不符；不是 ChmLink 设备 |
| `ERROR_SHARING_VIOLATION` | 网卡上已有其他会话打开 |
| `ERROR_NO_MORE_ITEMS` | 当前没有可接收的数据 |
| `ERROR_BUFFER_OVERFLOW` | 发往 Windows 的环形队列已满；请重试 |
| `ERROR_TIMEOUT` | 等待超时 |
| `ERROR_OPERATION_ABORTED` | 等待被取消，或会话已结束 |
| `ERROR_NOT_READY` | 网卡已暂停，或系统处于睡眠 |
| `ERROR_SHUTDOWN_IN_PROGRESS` | 系统正在关机 |
| `ERROR_REVISION_MISMATCH` | 驱动和 SDK 没有共同的契约版本，或驱动包不可用 |
| `ERROR_DUP_NAME` | 名称、MAC 或身份已被占用 |
| `ERROR_BUSY` | 另一个进程正在修改网卡；有网卡打开着会话，无法更换驱动；或已有一个等待者 |
| `ERROR_INSTALL_FAILURE` | 修改只完成了一部分；请检查该网卡 |
| `ERROR_RESOURCE_NAME_NOT_FOUND` | 没有提供 INF，且 DLL 没有内置驱动 |
| `ERROR_INF_IN_USE_BY_DEVICES` | 仍有网卡时调用了 `chmlink_delete_driver` |

## 示例

| 程序 | 演示内容 |
|---|---|
| [`examples/quickstart.c`](../../examples/quickstart.c) | 完整的生命周期：创建 IP 网卡、配置地址、响应 ping、删除网卡，并设置日志回调 |
| [`examples/list.c`](../../examples/list.c) | 不打开会话，列出网卡及其计数器 |
| [`examples/capture.c`](../../examples/capture.c) | 打印 Windows 通过网卡发出的内容 |
| [`examples/inject.c`](../../examples/inject.c) | 从文件注入一个原始帧或数据包 |

`bench/packet_bench.c` 和 `bench/tcp_bench.c` 是更大规模的 API 使用示例，包括多个发送线程、卸载功能和合并器。
