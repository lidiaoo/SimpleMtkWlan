# MT7921 macOS 26 (Tahoe) Wi-Fi 修复补丁集（P20–P29）

针对 [`lidiaoo/SimpleMtkWlan`](https://github.com/lidiaoo/SimpleMtkWlan) 的 MediaTek MT7921 (`14c3:7961`) 驱动修复集，
解决 macOS 26.7 (Tahoe) 上的一系列 Wi-Fi 缺陷：5G 网络扫不到/进不了列表、列表剧烈跳动、
连接全挂（僵尸关联）、PHY Mode 显示 None 等。

**测试环境**：Acer Nitro 5（i7-11800H）· MT7921 · macOS Tahoe 26.7.2 · OpenCore

![关于本机](screenshots/about-mac-crop.png)

![Wi-Fi 面板](screenshots/wifi-panel-crop.png)

---

## 修复内容

### 1. 全信道扫描（P20+P21+P22）
- 扫描窗口与 dwell 时间统一（全扫 80ms/信道），去掉 `SPLIT_SCAN` 分割扫描
- 关联状态下每信道扫描耗时 **230ms → 138ms**
- 实测 **26/26 轮全部覆盖 41 信道**，5G 信道不再漏扫

### 2. 节点表扩容（P24）
- net80211 节点缓存 `IEEE80211_CACHE_SIZE` 200 → 512
- 原 因：环境 AP 多时 200 上限被 2.4G 节点占满，`ieee80211_find_rxnode` 退化到
  `ic_bss`，**5G 管理帧全被吸收**——这是「5G 网络永远进不了列表」的根因
- 实测：扩容后节点树 2g=84 / 5g=128，5G 首次入树

### 3. Beacon IE 完整合成（P25 + P29）
- 发布给 airportd 的扫描结果重建完整 IE 链（Rates / HT Cap / HT Op / VHT Cap /
  VHT Op / RSN），macOS 对 5GHz 网络硬性要求 HT 以上，缺 IE 的条目会被判非法丢弃
- 实测：**PHY Mode 从 None 恢复为 802.11a/n/ac/ax**，airportd Failed=0
- P29：仅 2.4G 节点携带 DS Params（与真实 beacon 一致，避免污染 airportd 信道记录）

### 4. 列表发布策略（P23+P26+P27）
- 扫描期只收集不发布，扫描结束**一次性批量发布**，消除列表每轮抖动
  （实测每轮单条 `WCL scan finish completed=1`）
- 发布上限 40 条（RSSI 降序，已连接 BSS 保底保留）；实时逐条发布通道关闭
  （airportd 只合并不淘汰，实时流会越攒越全）
- 需要 40 条以上时加 boot-arg **`-mtk-allwifi`** 解除上限并恢复实时流

### 5. 僵尸关联修复（P28）★ 最关键
- 原因：WCL join 成功后 `ic_wcl_join_requested` 会话标志永久残留，任何电源循环 /
  掉线后 net80211 栈会**绕过 airportd 从节点缓存自动重连**，airportd 完全不知情
  （UI 显示断开、ping 却通），后续所有 join 的 live scan 被卡死拒绝
- 修复：join 成功也结束会话；`end_scan` 无活跃 join 请求不自行选网络——重连一律
  由 airportd 下发
- 实测：join 成功率 **0/10 → 9/9**（含 7 轮电源循环压力测试），面板「忽略此网络」
  流程干净回扫、无复活

---

## 验证数据（最终版，macOS 26.7.2 实测）

| 项目 | 结果 |
|---|---|
| 全信道扫描覆盖 | 41/41 信道，无漏扫 |
| 关联态扫描速度 | 138 ms/信道 |
| 每轮发布 | 单次完成，40 条（`-mtk-allwifi` 全量） |
| join 成功率 | 9/9（电源循环压力测试 7/7） |
| 4-way 握手 | 邻居中继器偶发拒绝 1~2 次后放行（AP 侧问题，驱动重试兜住） |
| 互联网连通 | 关联后 ping 网关/公网 0% 丢包，链路 MCS 9 / 867 Mbps |

> 已知限制：系统设置 Wi-Fi 面板的列表内容由 airportd 按请求 bitmap 过滤展示，
> 偶尔显示 2.4G 子集、偶尔全量属于 macOS 自身行为；join 走驱动节点缓存，
> **不受面板显示影响**（实测面板 5G=0 时 join 仍 2.2 秒成功）。

---

## 使用方法

### 应用补丁

```bash
git clone https://github.com/lidiaoo/SimpleMtkWlan && cd SimpleMtkWlan
git checkout c28b1d8                              # 基线 commit（2026-11-06）
git apply patches/lidiaoo-local-ALL-baseline-to-P19.patch
for n in P20-scan-window P21-all-channel-scan P22-full-sweep-always \
         P23-rssi-order-single-burst P24-node-cap-512 P25-beacon-ies \
         P26-single-completion P27-live-hold P28-zombie-join-session \
         P29-dsparams-5g; do
  git apply "patches/lidiaoo-local-$n.patch"
done
```

### 编译

```bash
./build-macos.sh Release AirportMtkWlan
# 产物: build/Release/tahoe/AirportMtkWlan.kext
```

### 部署

把 `AirportMtkWlan.kext` 放入 EFI 的 `OC/Kexts/`（config.plist 中该 kext 需
`Enabled=true`），重启生效。可选 boot-arg：

- `-mtk-allwifi`：解除 40 条发布上限，显示扫描到的全部网络

---

## 补丁校验值（sha256 前 16 位）

| 补丁 | 行数 | sha256 |
|---|---|---|
| `ALL-baseline-to-P19`（c28b1d8→P19 全量） | 1,299 | `a483aa8f1e95a405` |
| `P20-scan-window` | 107 | `c2360557fc36586f` |
| `P21-all-channel-scan` | 93 | `6d9abafb204c2572` |
| `P22-full-sweep-always` | 111 | `beebf0b5f1c73b6d` |
| `P23-rssi-order-single-burst` | 95 | `e091e0837c003228` |
| `P24-node-cap-512` | 93 | `cb6203b5e636b5ef` |
| `P25-beacon-ies` | 167 | `584094ff6f6d6919` |
| `P26-single-completion` | 142 | `eb5e272fa5261a93` |
| `P27-live-hold` | 67 | `1ab2209deb52a50a` |
| `P28-zombie-join-session` | 49 | `0794166e2b4dadba` |
| `P29-dsparams-5g` | 60 | `459393d5d7a543fc` |

全链已验证：`c28b1d8` + 11 个补丁 = 与实测源码树逐字节一致；编译产物
`AirportMtkWlan.kext`（2,697,704 B）sha256 `e53b43745507a71a…`。

## 已知问题

- 邻居中继器 44:df:65:cd:00:00 会反复拒绝 4-way msg 2/4（重试后通过），偶发
  长时间不响应 msg 1/4——AP 侧问题
- `networksetup -getairportnetwork` 在 macOS 26 会误报未关联；判断关联状态请用
  `system_profiler SPAirPortDataType` 或 `ipconfig getsummary`
