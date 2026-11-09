# SimpleMtkWlan（虚拟有线网卡版）编译 / 安装记录

日期：2026-11-08
来源：`https://github.com/lidiaoo/SimpleMtkWlan` HEAD `c28b1d8`（2026-11-06 17:14，`docs: make README bilingual…`）
上游链路：laobamac/SimpleMtkWlan → PR #5 → e5u 1.5.x/1.6.0 → lidiaoo fork（802.11n/ac VHT80 + RX/TX A-MSDU + MSI + TX DMA 线性化 + 组播软件 CCMP + 换网清理 peer）

## 编译

```bash
cd mtk-fix/SimpleMtkWlan-lidiaoo
xcodebuild -project SimpleMtkWlan.xcodeproj -scheme SimpleMtkWlan \
  -configuration Release -derivedDataPath build \
  -destination "platform=macOS,arch=x86_64" CODE_SIGNING_ALLOWED=NO build
```

- `-destination platform=macOS,arch=x86_64` 必须带，否则报 *Supported platforms for the buildables in the current scheme is empty*
- MacKernelSDK 是 submodule，clone 时用 `--recurse-submodules`
- 结果：**BUILD SUCCEEDED**（18 s，仅 `-stdlib=libc++ unused` / 重复 `-lkmod` / deployment target 10.9 三条无害警告）

产物：

| 项 | 值 |
|---|---|
| 路径 | `build/Build/Products/Release/SimpleMtkWlan.kext` |
| 二进制大小 | 2,641,920 B |
| sha256 | `e7b850baf7800f534a69e975a57ba1f527fc89547e688d600e417b7a0e86bbb6` |
| 架构 | x86_64 |
| CFBundleIdentifier | `com.laobamac.SimpleMtkWlan` |
| CFBundleVersion | `1.6.0`（纯数字，符合 README 的 OC 预链接要求）|
| IOPCIPrimaryMatch | `0x796114c3 0x060814c3 0x792214c3 0x061614c3` |
| OSBundleLibraries | 仅 IONetworkingFamily / IOPCIFamily / kpi.* — **不依赖 Lilu** |

客户端 ABI 已核对：`include/ClientKit/Common.h` 与 `IoctlId.h` 和已编译的 SimpleMTK.app
（laobamac/SimpleMTKClient HEAD `45e1b22`）**结构体布局、ioctl 编号、IOCTL_VERSION=1 全部一致**，
差异只有版权注释与空白的排版。App 里的 `#define API_TEST` 是死宏（全仓库无引用），不影响。

## 安装

```bash
bash mtk-fix/SimpleMtkWlan-lidiaoo/install-160.sh            # 安装
bash mtk-fix/SimpleMtkWlan-lidiaoo/install-160.sh --list     # 看备份
bash mtk-fix/SimpleMtkWlan-lidiaoo/install-160.sh --rollback # 回滚
```

- 目标：`/Volumes/H/EFI/OC/Kexts/SimpleMtkWlan.kext`
- 旧版备份：`mtk-fix/wex-mtk-backup/SimpleMtkWlan.kext.bak-20261108-194656`
  （e5u 1.6.0 预编译，2,867,672 B，sha256 `35ed064e…190d94`）
- config.plist **无需改动**：`Kernel > Add[39]` 已 `Enabled=true`，BundlePath / ExecutablePath / 空 Min-MaxKernel 全部匹配新产物；`DisableIoMapper=true` 已在位
- `AirportMtkWlan.kext`（`Add[37]`）保持 `Enabled=false`，两者互斥

## `-mtkrxamsdu` 在虚拟有线网卡上是否生效 —— 生效

解析点在 **共享 HAL**，不在任何一个前端：

`SimpleMtkWlan/hal_mwx/MtkMwx.cpp : mwx_attach()` 第 3021–3028 行

```c
{
    int on = 0;
    sc->sc_rx_amsdu_enabled = PE_parse_boot_argn("-mtkrxamsdu", &on, sizeof(on));
    if (sc->sc_rx_amsdu_enabled)
        ic->ic_caps |= IEEE80211_C_AMSDU_IN_AMPDU;
    MWX_DEV_LOG(sc, "RX A-MSDU reassembly enabled=%d max_body=3839\n",
        sc->sc_rx_amsdu_enabled);
}
```

`SimpleMtkWlan.kext` 和 `AirportMtkWlan.kext` 都经 `MtkHalService → mwx_attach()` 起卡，
`hal_mwx/` 是同一份目标文件，所以这个 boot-arg 对两者等价。

完整生效链：

1. `mwx_attach` 解析 → `sc->sc_rx_amsdu_enabled = true`，并给 `ic_caps` 置 `IEEE80211_C_AMSDU_IN_AMPDU`
2. net80211 收到 AP 的 ADDBA Request：`ieee80211_input.c:3154`
   `if (ic->ic_caps & IEEE80211_C_AMSDU_IN_AMPDU) ba->ba_params |= IEEE80211_ADDBA_AMSDU;`
3. `mwx_ampdu_rx_start()`（`MtkMwx.cpp:8874`）二次确认：
   `amsdu = sc_rx_amsdu_enabled && crypto_supported && (ba_params & ADDBA_AMSDU)`，
   `crypto_supported` 要求成对密钥是 CCMP（WPA2-PSK 满足），然后把 `amsdu` 传给固件
   `mwx_mcu_sta_ba(..., amsdu)` —— 这一步才真正让 MT7921 按 A-MSDU 投递
4. RX 路径 `mwx_rx()`（`MtkMwx.cpp:2328`）→ `mtkRxAmsduReadDescriptor()` → `mwx_rx_amsdu_feed()`
   做重组，然后才把 mbuf 交给 IOEthernetController

二进制佐证：已装 kext 里 `strings -x` 精确命中 `-mtkrxamsdu`（1 次），并含
`RX A-MSDU reassembly enabled=%d max_body=3839`、`rxamsdu cumulative enabled=… started=… subframes=…`
三条日志格式串；符号表里 `mwx_ampdu_rx_start`、`mwx_rx_amsdu_purge/invalid/barrier_gated` 均在。

**不会生效的三种情况**（此时 `started` 一直是 0）：

- 成对密钥不是 CCMP（WEP / TKIP / 开放）
- AP 的 ADDBA Request 没带 A-MSDU-in-A-MPDU 位（老路由器 / 某些 2.4G 模式）
- 根本没建立 RX Block Ack 会话（信号太差、协商失败）

## 重启后验证

```bash
kextstat | grep SimpleMtkWlan                 # 应为 1.6.0
bash mtk-fix/SimpleMtkWlan-1.1.0/mtklog.sh | grep -E "RX A-MSDU|rxamsdu|rx BA start"
```

期望看到：

```
RX A-MSDU reassembly enabled=1 max_body=3839
rx BA start tid=0 ssn=... win=64 amsdu=1 rv=0
rxamsdu cumulative enabled=1 started=N completed=N subframes=M singleton=0 ...
```

## 实测验证（2026-11-08 20:04，开机 53 分钟）

`mtklog.sh` 最新快照（t=3191 s，连接 `meiyoumima` 2.4 GHz ch5）：

```
rxamsdu   enabled=1 started=35944 completed=35944 subframes=93825 singleton=0
          orphan=0 mismatch=0 invalid=0 timeout=0 allocfail=0 pending=0
stats     rx_hwdec=306777 rx_swdec=150 ipkts=363372 ierr=2 opkts=222714 oerr=0 dup=137 ba_below=1204
rxampdu   completed=628 mpdu=629 max=2 bins[1=627 2-3=1 ...] rx_agreements=3
rxrate    vht=4244 bw[20=4246] nss[1=4246] mcs[9=3847 8=388] sgi=4151 ldpc=4244
tx        full=0 txwi_free=2045 ring_free=2047 amsdu=1
rxpool    total=16384 free=14279 exhausted=0 fallbacks=0
poll      state=4 chan=5
```

结论：

- `started == completed`（35944/35944）= 每条聚合都完整收完，**零中途丢弃**
- `subframes / started = 2.61`，`singleton=0` = 全是真聚合，没有单词帧凑数
- 重组状态机所有错误计数为 0（orphan / mismatch / invalid / timeout / allocfail / pending）
- 硬件解密占比 99.95%（150 / 306777 走软件兜底，即组播/广播帧）
- **参数确认在虚拟有线网卡上生效**

两点观察：

1. **当前链路是 2.4 GHz 信道 5、20 MHz、单流**（`poll chan=5` 取自 `ieee80211_chan2ieee(ic_bss->ni_chan)`；
   `rxrate bw[20] nss[1]`）。信号极好（MCS9 占 90%、SGI 97.8%、LDPC 全开），但 PHY 上限只有
   VHT20/1SS 的 86.7 Mbps。要复现 README 里 500+ 的数字，得连 5 GHz 的 `meiyoumima_5G`（VHT80 2×2 = 867）。
2. **A-MPDU 没在聚合**：`max=2`，627/628 条是单 MPDU。也就是说现在起作用的是
   "A-MSDU in A-MPDU"（每 MPDU 内 2.6 个 MSDU），MPDU 之间不叠加。2.4G + 轻流量下属正常；
   若测速时仍然 `max≤2`，RX 天花板在 A-MPDU 一侧，与 A-MSDU 无关。
3. `ba_below=1204` / `gap_timeout` 约 0.38 次/秒 —— 固件不投递 BAR、靠 100 ms 超时收尾的已知行为，量级可忽略。

`singleton=0` 且 `subframes / started ≈ 3` 说明 AP 真的在发聚合帧且重组在工作。
