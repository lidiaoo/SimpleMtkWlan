<!--
  README.md
  SimpleMtkWlan

  Created by laobamac (王孝慈) on 2026/5/5.
  Copyright © 2026 laobamac (王孝慈). All rights reserved.
-->

**简体中文** ｜ [English](README.en.md)

# SimpleMtkWlan

[![Build](https://github.com/e5u/SimpleMtkWlan/actions/workflows/build.yml/badge.svg)](https://github.com/e5u/SimpleMtkWlan/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/e5u/SimpleMtkWlan)](https://github.com/e5u/SimpleMtkWlan/releases)
[![License: GPL v2](https://img.shields.io/badge/license-GPLv2-blue.svg)](LICENSE)

**让 MediaTek MT7921 / MT7921K（AMD RZ608）/ MT7922 PCIe 无线网卡在 macOS 上跑起来。**

一个实验性的 macOS 内核扩展，基于 OpenBSD 的 `mwx(4)` 驱动与 itlwm 风格的 `net80211` 移植。
两种形态共用同一套硬件层：

* **`AirportMtkWlan.kext`** —— 网卡是一块真正的 macOS Wi-Fi 接口：原生 Wi-Fi 菜单、系统设置、
  私有 Wi-Fi 地址、钥匙串里保存的密码。为 **macOS 26 Tahoe**（以及 Sequoia 15.2+）构建，
  不需要 OCLP root patch，也不需要降级 IOSkywalkFamily。
* **`SimpleMtkWlan.kext`** —— 网卡表现为一块以太网接口（`enX`），由
  [SimpleMTK](https://github.com/laobamac/SimpleMTKClient) 菜单栏客户端负责扫描和连接。
  凡是这个 kext 能加载的 macOS 版本都能用。

> **同一张卡的蓝牙**：**[SimpleMtkBtFw](https://github.com/flxxxxddd/SimpleMtkBtFw)**
> （固件加载器 + 补丁；AirPods 音频可用）。

> 本仓库包含让 **MT7921 / RZ608** 真正可用的修复（此前它能 attach，但完全不通数据），
> 以及在 Tahoe 上补齐的 802.11n、802.11ac（VHT80）和原生 Wi-Fi 菜单。
> 见 [本 fork 改了什么](#本-fork-改了什么)。

> **本仓库（`lidiaoo/SimpleMtkWlan`）额外补充**：`14c3:7961`（MT7921）在 macOS 26.7.2 Tahoe
> 上的实测记录，含吞吐数据和两个害我们多花好几天的坑。
> 见 [实测记录](#实测记录mt7921-在-tahoe-上)。英文版见 [README.en.md](README.en.md)。

## 支持状态

| 功能 | 状态 |
|---|---|
| 探测、固件加载、MSI 中断 | ✅ |
| 扫描（2.4 GHz + 5 GHz） | ✅ |
| WPA2-PSK（CCMP）关联、四次握手 | ✅ |
| 硬件 RX 解密（芯片没解出来的走软件兜底） | ✅ |
| 网络 / 频段切换 | ✅ |
| 802.11n（HT20/HT40，MCS 0–15，SGI，RX STBC，WMM） | ✅ |
| 802.11ac（VHT20/40/80，2×2 MCS 0–9，RX STBC） | ✅ |
| 802.11ac VHT160（MT7922） | ⚠️ 实验性（`-mtk160`），需手动开启 |
| A-MPDU：RX Block Ack + TX 聚合 | ✅ |
| 硬件 TX A-MSDU（封装在 A-MPDU 内）、VHT beamformee | ✅ |
| 开机自动连接，同一路由器优先 5 GHz 而非 2.4 GHz | ✅ |
| AUTH / ASSOC 重传（丢帧不再导致整轮重新扫描） | ✅ |
| 芯片卡死自动恢复（DMA 停滞看门狗、attach 时 PCIe FLR） | ✅ |
| macOS 原生 Wi-Fi 菜单（IO80211Family / Skywalk），Tahoe | ✅ `AirportMtkWlan` |
| 原生 Wi-Fi 菜单，Sequoia 15.2+ | ⚠️ 能编译，尚未在本 HAL 的实机上验证 |
| 802.11ax（HE，MT7922） | ⚠️ 实验性（`-mtkhe`），需手动开启 |
| 仅 WPA3（SAE）的网络 | ❌ |
| 睡眠 / 唤醒（唤醒后重载固件，自动重连） | ✅ |
| AirDrop / Handoff / 接力（AWDL） | ❌ |

### 已测试硬件

| 网卡 | PCI ID | macOS | 结果 |
|---|---|---|---|
| MediaTek MT7921K（AMD RZ608） | `14c3:0608` | 26.7.1 Tahoe，Intel 台式机 | `AirportMtkWlan`：原生 Wi-Fi 菜单，可从钥匙串连接，5 GHz VHT80 下 50+ Mbit/s（信号较弱，百兆线路） |
| MediaTek MT7921K（AMD RZ608） | `14c3:0608` | 15.8.1 Sequoia，Intel 台式机 | `SimpleMtkWlan`：5 GHz VHT80 下 30–50 Mbit/s（同位置 MacBook 约 85）；2.4 GHz 可用；开机自动连接 |
| MediaTek MT7922（RZ616） | `14c3:0616` | 15.6 / 26.7.1 Tahoe | PCIe attach 与 MT7922 固件选择已验证；Tahoe 原生 Wi-Fi、WPA2、VHT160/HE160 2-NSS，本地 HTTPS 测试约 460–500 Mbit/s |
| MediaTek MT7922 | `14c3:7922` | Tahoe（上游 PR #5） | 5 GHz 下 20 MHz legacy |
| MediaTek MT7921 | `14c3:7961` | **26.7.2 Tahoe**，Acer Nitro 5（i7-11800H） | `AirportMtkWlan` 1.6.0 + `-mtkrxamsdu`：原生 Wi-Fi 菜单，PHY 867 Mbit/s、5 GHz VHT80 下 **507–589 Mbit/s** —— 见 [实测记录](#实测记录mt7921-在-tahoe-上) |

其他 MediaTek 设备不受支持。

### 实测记录：MT7921 在 Tahoe 上

`14c3:7961` 之前只被标注为「与 RZ608 同族，预期可用」，从未实测。**它可以用，而且很快** ——
下面是在笔记本上、用官方 v1.6.0 Tahoe 预编译包、未改一行源码复现的结果。

| 项目 | 值 |
|---|---|
| 机器 | Acer Nitro 5，Intel i7-11800H，32 GB |
| 系统 | macOS 26.7.2 (25G332)，Darwin 25.6.0 |
| 引导 | OpenCore REL-108，SIP Custom，AMFI 已关 |
| 驱动 | `AirportMtkWlan-Tahoe-1.6.0-RELEASE.zip`（sha256 `599c95df…13fb1`） |
| 启动参数 | 仅在原有基础上追加 `-mtkrxamsdu` |
| 链路 | 802.11ac，5 GHz ch40，80 MHz，2×2 MCS9，SGI，LDPC，867 Mbit/s，RSSI −31 dBm，WPA2-PSK |

**吞吐**（iperf3 对有线千兆主机，10 秒，除注明外均为 4 流）：

| 测试 | 第一次 | 第二次 |
|---|---|---|
| TCP 上行 4 流 | 514 / 507 Mbit/s | 424 / 415 |
| TCP 下行 4 流 | 576 / 565 | 589 / 574 |
| TCP 上行 单流 | 426 | — |
| TCP 下行 单流 | 578 | — |
| UDP 上行（发送端给到 600 Mbit/s） | 534（丢包 1.6%） | — |
| UDP 下行（发送端给到 600 Mbit/s） | 589 | — |

达到 867 Mbit/s 物理速率的 58–68%，属于健康的半双工上限。

**驱动日志**（`tools/mtklog.sh` 原样输出）：

```
rxamsdu: enabled=1 started=32984 completed=32984 subframes=105167 singleton=0
rxrate:  n=3641 vht=3641 bw80=3641 nss2=3641 mcs9=3641 sgi=3641 ldpc=3641
tx:      txwi_free=2045 ring_free=2047 full=0 tx_ba=2 amsdu=1
stats:   ipkts=688375 ierr=0 opkts=789138 oerr=0 ba_below=103
```

`singleton=0` 且平均每个聚合 3.2 个子帧，说明 RX A-MSDU 解封装确实在工作；
`full=0` 说明 TX 环已经不再是瓶颈。

#### 让我们多花好几天的两个坑（建议先查这两条）

1. **上一张 Intel 卡留下的 OCLP「Modern Wireless」/ IO80211 root patch 会静默地毁掉扫描列表。**
   驱动能 attach、能发布扫描结果、`postMessage` 返回 0，但 Wi-Fi 菜单始终是空的 ——
   因为旧的 IO80211 消费端不认识 Tahoe 的 WCL 消息。
   `airportd` 日志里是 `SCAN_CACHE_UPDATED` 配 `results=0 liveCount=0 error=-3905`。
   **请先清掉 root patch，再去调试驱动**；我们这边仅这一步就让「列表永远为空」变成了正常连网。
2. **`CFBundleVersion` 必须是纯数字。** 把 kext 重新打包成 `1.5.0-myfix` 会让 OpenCore
   在预链接阶段直接拒绝（`Prelinked injection ... - Invalid Parameter`），内核根本看不到它。

本机还观察到几个不影响吞吐的小问题：
`networksetup -getairportnetwork en1` 在已连网时仍报 "not associated"；
扫描请求只带 2.4 GHz 信道（`channels=14 2g=14 5g=0`），但 5 GHz 网络仍可从菜单直接连接。

更长的中英双语记录（含被证伪的若干假设）：
[laobamac/SimpleMtkWlan#9 评论](https://github.com/laobamac/SimpleMtkWlan/issues/9#issuecomment-6681009999)。

## 安装

`AirportMtkWlan.kext` 与 `SimpleMtkWlan.kext` **只能二选一**，绝不要同时启用：
它们匹配的是同一张卡。

### 原生 Wi-Fi 菜单（AirportMtkWlan，Tahoe）

1. 从 [Releases](https://github.com/e5u/SimpleMtkWlan/releases) 下载
   `AirportMtkWlan-Tahoe-x.y.z-RELEASE.zip`（macOS 15.2+ 用 `-Sequoia-` 那个）。
2. 把 `AirportMtkWlan.kext` 复制到 `EFI/OC/Kexts/`，并加入
   `Kernel → Add`（条目写法与下文相同，`BundlePath` 填 `AirportMtkWlan.kext`，
   `ExecutablePath` 填 `Contents/MacOS/AirportMtkWlan`）。Tahoe 版设 `MinKernel`
   `25.0.0` / `MaxKernel` `25.99.99`（Sequoia 用 `24.2.0` / `24.99.99`）：
   每个构建只适配对应的 IO80211 版本，装错会在另一个版本上 panic。
3. `Kernel → Quirks → DisableIoMapper` = `true`（驱动目前还不支持 AppleVTD DMA 重映射）。
4. **不要**打 OCLP 的「Modern Wireless」root patch，也**不要** Block 或替换
   `IOSkywalkFamily`、**不要**注入 `IO80211FamilyLegacy`：这个 kext 用的是系统自带的
   Wi-Fi 栈。请把 Intel / 博通教程留下的这类残留清理掉。
5. 如果装过 `SimpleMtkWlan.kext`，请禁用，并退出 / 删除 SimpleMTK App。
6. 确认网卡没有被隐藏（见下文第 4 步），重启，然后在 Wi-Fi 菜单里选网络。

`-mtkhe` 会在 MT7922 上启用 802.11ax 的关联与数据通路。它属于实验性功能，默认关闭。

### SimpleMtkWlan + SimpleMTK 客户端

1. 从 [Releases](https://github.com/e5u/SimpleMtkWlan/releases) 下载最新的
   `SimpleMtkWlan-x.y.z-RELEASE.zip`（也可以自行编译，见下文）。
2. 把 `SimpleMtkWlan.kext` 复制到 `EFI/OC/Kexts/`。
3. 加入 `config.plist → Kernel → Add`：

   ```xml
   <dict>
       <key>Arch</key>           <string>x86_64</string>
       <key>BundlePath</key>     <string>SimpleMtkWlan.kext</string>
       <key>Comment</key>        <string>MediaTek Wi-Fi</string>
       <key>Enabled</key>        <true/>
       <key>ExecutablePath</key> <string>Contents/MacOS/SimpleMtkWlan</string>
       <key>MaxKernel</key>      <string></string>
       <key>MinKernel</key>      <string></string>
       <key>PlistPath</key>      <string>Contents/Info.plist</string>
   </dict>
   ```

4. **确认网卡没有被隐藏。** 很多现成 EFI 会带一个 SSDT
   （例如 `SSDT-Disable_Network_*.aml`），把网卡的 `vendor-id` / `device-id`
   改写成 `0xFFFF`，让 macOS 忽略它。请禁用该 SSDT，否则驱动永远匹配不上。
   `ioreg -l | grep -A40 'pci14c3'` 应当能看到 `vendor-id = <c3140000>`。
5. 安装 [SimpleMTK](https://github.com/laobamac/SimpleMTKClient)，重启，然后在菜单栏里连接网络。

可选启动参数：

* `-mtkhe` —— 在 MT7922 上启用实验性 802.11ax。
* `-mtk160` —— 在 MT7922 上启用实验性 160 MHz。
* `-mtkrxamsdu` —— 启用 RX A-MSDU 解封装。
* `-mtknoamsdu` —— 关闭硬件 TX A-MSDU。
* `-mtkno160`、`-mtknopool`、`-mtknowtblba`、`-novht`、`-noht40` —— 针对性的诊断回退开关。

## 排错

* **先看驱动日志。** 驱动维护一个持久环形缓冲，即使启动失败也依然存在，且无需 root 即可读取：

  ```sh
  ./tools/mtklog.sh > mtk.txt
  ```

  周期性的 `stats` 行会显示 RX 计数、硬件解密与软件解密的对比
  （`rx_hwdec` / `rx_swdec`）以及丢帧原因。两个 kext 写的是同一份日志。
* **AirportMtkWlan 下菜单里没有 Wi-Fi。** 检查是否只启用了两个 kext 中的一个、
  没有 OCLP 无线补丁 / `IOSkywalkFamily` block 生效、`DisableIoMapper` 已开启、
  构建版本与系统匹配（Tahoe 还是 Sequoia）。`ioreg -l -w0 | grep IO80211InterfaceRole`
  应当输出 `Infrastructure`。遗留的 OCLP `IO80211Family` root patch 会让列表为空，
  即使驱动正常发布了结果 —— 见 [实测记录](#实测记录mt7921-在-tahoe-上)。
* **重启后提示 "SimpleMtkWlan is not running"。** 热重启不会给 M.2 网卡断电。
  驱动现在会自动恢复芯片（WFSYS reset，必要时再做 PCIe function-level reset）；
  如果仍然失败，请彻底关机再开机。
* **速度慢。** 看 stats 行里的 `rx_swdec`，它应当远小于 `rx_hwdec`。
  测速只在 SimpleMTK 菜单关闭时才有意义（客户端会触发扫描；已连接时扫描被限制为两分钟一次）。
* **Wi-Fi 工作在 2.4 GHz 时，2.4 GHz 无线鼠标/键盘卡顿。** 这是同频段的射频共存问题，
  不是驱动 bug：把网卡固定在 5 GHz（SimpleMTK 里只保留 5 GHz 网络），
  或者用 USB 延长线把接收器挪离天线。
* **开机后连接很慢。** 网络由 SimpleMTK App 选择，而它要登录之后才启动；
  在那之前驱动一直在扫描。
* **吞吐不如真 Mac。** 信号弱时 AP 大多发 VHT MCS 3–4（看 `rxrate` 行）。
  固件不投递 Block Ack Request 帧，所以偶尔出现的重排空洞只能靠 100 ms 超时关闭
  （`ba gap_timeout`）。
* 无关的内核 panic（启动约 2 秒时 `element modified after free`）在测试机上由
  `NVMeFix.kext` 引起，不是本驱动的问题。

## 编译

需要装了 Xcode 的 macOS（仅有命令行工具不够）。内核头文件来自
[acidanthera/MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)，
它是一个 git submodule。

```sh
git clone --recurse-submodules https://github.com/e5u/SimpleMtkWlan.git
cd SimpleMtkWlan
xcodebuild -project SimpleMtkWlan.xcodeproj -scheme SimpleMtkWlan \
  -configuration Release -derivedDataPath build CODE_SIGNING_ALLOWED=NO build
# -> build/Build/Products/Release/SimpleMtkWlan.kext
```

克隆时忘了 submodule？`git submodule update --init`。

> **改动 `mtk80211/` 或 `include/` 下的头文件后，请做一次干净构建（`rm -rf build`）。**
> 这两个目录在系统头文件搜索路径上，Xcode 不会跟踪它们的依赖关系，
> 否则会链接到过期的目标文件。

原生 Wi-Fi kext 用脚本构建（不在 Xcode 工程里）：

```sh
./build-macos.sh Release AirportMtkWlan                        # Tahoe（默认）
AIRPORT_OS=sequoia ./build-macos.sh Release AirportMtkWlan     # Sequoia 15.2+
# -> build/Release/{tahoe,sequoia}/AirportMtkWlan.kext
```

Host 测试（把真实驱动函数抽出来跑桩代码，默认开 UBSan；CI 还可以额外开 ASan）：

```sh
./tests/run-all.sh
```

CI 在每次 push 和 pull request 上构建 Debug 与 Release；推送 `vX.Y.Z`
标签会发布一个带 zip 的 GitHub release。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `SimpleMtkWlan/SimpleMtkWlan.cpp` | IOEthernetController 前端（SimpleMtkWlan.kext）、看门狗 |
| `AirportMtkWlan/` | IO80211 / Skywalk 前端（AirportMtkWlan.kext），来自 AirportItlwm（WCL） |
| `include/Airport/` | IO80211Family / Skywalk 头文件（`macOS15`、`macOS26` 两套 ABI） |
| `SimpleMtkWlan/MtkLog.cpp` | 两个 kext 共用的持久调试日志 |
| `build-macos.sh` | 不改 Xcode 工程即可构建 AirportMtkWlan（以及 SimpleMtkWlan） |
| `SimpleMtkWlan/MtkWlanUserClient.cpp` | SimpleMTK App 使用的 IOUserClient |
| `SimpleMtkWlan/hal_mwx/` | MT7921/MT7922 硬件层（OpenBSD `mwx` 的移植） |
| `mtk80211/` | `net80211` 与 BSD/Linux 兼容层 |
| `include/` | HAL 接口、内嵌固件（`FwBinary.cpp`）、客户端 API |
| `tests/` | host 测试 |
| `tools/mtklog.sh` | 导出驱动日志 |
| `MacKernelSDK/` | 内核头文件子模块 |

## 本 fork 改了什么

在上游与 [PR #5](https://github.com/laobamac/SimpleMtkWlan/pull/5)
（关联上下文、TX 串行化、数据包归属）之上：

* **固件加载：** 固件分片不再需要 16 KB 的 jumbo mbuf cluster
  （开机时拿不到 → `ENOMEM`，没有 MAC 地址，也没有接口）。
* **中断：** 注册真正的 MSI 向量而不是 INTx，并且不再手工置 MSI 使能位
  （设备不再拉 INTx，只能通过共享中断线被服务）；重新使能前先排空挂起的状态。
* **TX DMA 停滞：** 多 mbuf 的帧被线性化进一个 cluster；此前 WFDMA 在第一个
  DHCP 帧上就永久卡死。
* **组播 / 广播流量：** 芯片不解密组地址帧，导致广播 ARP/DHCP 被丢弃。
  现在同时为 `net80211` 的软件 CCMP 建立密钥，作为兜底。
* **切换网络：** 切换前先在旧 AP 所在信道上发出 deauth；认证期间忽略过期的
  disassoc 帧；离开时在固件里删除 peer station / 关闭 BSS
  （否则第一次切换之后硬件解密就失效了）。
* **漫游：** 关闭由 RSSI 触发的后台漫游（它会在同一路由器的 2.4 GHz 与 5 GHz
  网络之间每几秒来回跳）。
* **吞吐：** 已关联状态下的后台扫描做了限流。
* **802.11n：** HT 能力、WMM、关联时下发完整的 peer STA record、把 RX Block Ack
  与 TX A-MPDU 会话交给固件。对外声明 HT 20/40（AP 只会对声称支持 40 MHz 的
  客户端发 80 MHz）。
* **802.11ac：** VHT 能力、STA_REC_VHT、工作带宽（20/40/80 MHz）取自关联结果、
  RX STBC；调试日志里有按周期统计的 RX 速率直方图。
* **连接过程：** 与 2.4 GHz 的 RSSI 差在 15 点以内时优先选 5 GHz；清理过期的
  peer 记录和扫描结果；首次 AUTH 之前以及停止时发 deauth；AUTH/ASSOC 最多重传
  3 次（此前丢一个关联响应就要多等约 20 秒）。
* **健壮性：** DMA 停滞看门狗（硬复位 + 重载固件）、attach 时的芯片恢复
  （WFSYS reset → PCIe FLR）、修复 TXWI 泄漏。
* **可调试性：** 持久的 `MtkDebugLog` 环形缓冲发布在 PCI 设备上，
  周期性输出硬件 / 统计信息，数据路径上保持安静。
* **原生 Wi-Fi 菜单（AirportMtkWlan）：** laobamac 的 AirportItlwm Tahoe/Sequoia
  （WCL）前端，经 pengubow 适配到 MediaTek HAL，再叠在本 fork 的 MT7921 硬件层之上。
  针对这张卡的改动：Tahoe ABI 构建、由前端看门狗驱动 HAL 的日常维护
  （停滞恢复、AUTH/ASSOC 重传）、不再手工使能 MSI、共用同一份持久日志、
  链路速率取自实测 RX 速率、已连接时允许系统请求的短扫描（全量扫描仍然限流）。
* **TX 路径：** 数据帧进入其接入类别（AC）对应的硬件队列（此前所有帧都排在 VO），
  去掉逐帧 TX 状态事件，对没有聚合协议的 TID 关闭 Block Ack，TX 环满时暂停输出
  而不是丢包，AP 允许时在 A-MPDU 内使用硬件 A-MSDU。对外声明 VHT SU/MU
  beamformee 与 HT LDPC（沿用 Linux 的默认值）。
* **睡眠 / 唤醒：** S3 时网卡断电。睡眠时驱动离开网络并屏蔽中断；唤醒后重新接管
  芯片（必要时 WFSYS reset / PCIe FLR）并重载固件。此前唤醒后第一个中断会读到
  过期的 DMA 环并直接 panic。
* **MT7922 PCIe / 802.11ax：** 识别 RZ616 的 PCIe ID（`14c3:0616`）、选择 MT7922
  固件、在 `-mtkhe` 后协商 HE80/HE160，并保留 `-mtk160` 之后的 VHT160 作为回退路径。
* **RX 路径：** 使用低 4 GiB 的 DMA 页池，让收到的帧直接落进 mbuf，
  同时保留常规 MCLGET 路径作为兜底。
* **中断调度：** 分发改到 `IOFilterInterruptEventSource`，
  并把 RX 重调度限制为每次中断一个工作项。
* **WCL：** 遵守系统的漫游锁，并暴露 TX 反压计数器供原生 Wi-Fi 诊断使用。
* **测试与构建：** 扩展 host 测试覆盖 HE TLV、DMA 池、A-MSDU/A-MPDU、扫描与漫游；
  支持从干净克隆构建，并由 CI 发布。

## 许可证

GPLv2，见 [LICENSE](LICENSE)。部分文件带有从上游 GPL/BSD/OpenBSD/NetBSD/FreeBSD
组件继承而来的额外或更具体的声明，这些声明分别适用于对应文件。
固件依 MediaTek 固件许可再分发（与 linux-firmware 一致）。

## 致谢

* [laobamac（王孝慈）](https://github.com/laobamac) —— SimpleMtkWlan 与 SimpleMTK
* [mavizao](https://github.com/mavizao) —— PR #5（MT7922 关联/TX 修复、host 测试）
* [pengubow](https://github.com/pengubow/SimpleMtkWlan) —— AirportMtkWlan：把 AirportItlwm 的原生 Wi-Fi 前端接到 MediaTek HAL 上（WCL 扫描/连接钩子、私有地址、Infrastructure 角色修复、密码连接、命令门控睡眠）
* [laobamac/itlwm](https://github.com/laobamac/itlwm) —— Sequoia/Tahoe 上的 AirportItlwm（WCL 层、macOS 15/26 的 IO80211 头文件、TX pull 模型、链路质量）
* OpenBSD `mwx(4)` 驱动的作者们
* [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm)（zxystd）—— net80211 移植、驱动架构与最初的 AirportItlwm
* Linux [mt76](https://github.com/openwrt/mt76) —— 硬件参考
* [acidanthera](https://github.com/acidanthera) —— MacKernelSDK

相关项目：[SimpleMtkBtFw](https://github.com/flxxxxddd/SimpleMtkBtFw) —— MT7921 / RZ608 / MT7922 的蓝牙。
另见 [CREDITS.md](CREDITS.md)。
