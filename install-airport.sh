#!/bin/bash
# 安装本地打补丁后编译的 AirportMtkWlan.kext（macOS 原生 Wi-Fi 前端）
#
# 打过什么：
#   P1  arxusion-37aa87e  AUTH/ASSOC 三次失败后回 S_INIT（终止无限重试循环）
#   P2  local-scan-5g-diag 扫描信道诊断日志（只加日志，不改行为）
#   P3  port-itlwm-scan-cache 扫描完成不再报 timeout / 0 结果时补发 node tree
#       （修 Wi-Fi 列表"闪一下白"：airportd 收到 timeout 会丢弃整份缓存）
#   P4  local-retry-key       debugPoll 按 BSSID 计重试次数，6 次后回 S_INIT
#   P5  local-assoc-window    S_ASSOC 立即补发 ASSOC_REQ，去掉 newstate 里的 mwx_start
#   P6  local-mgmt-tx         mwx_start 不再因 if_snd oactive 卡住 ic_mgtq；
#                             S_ASSOC 内按 20 ms 间隔补发两次 ASSOC_REQ；
#                             加 txstart/mgmt 诊断日志
#   P7  local-scan-selfheal   beginWCLScan 入口自愈：清理超龄的 scanPending
#                             （>18s+4s）与残留的 F_BGSCAN，并记录拒绝原因
#                             （修「点网络撞上扫描后，列表永久空白直到断电重启」）
#   P8  local-associd-guard   ASSOC_REQ 补发前检查 ni_associd：关联已成功就停发。
#                             ic_state 滞后于 assoc_resp 处理，导致补发的 REQ 落在
#                             4-way 期间，把 AP 侧安全上下文重置、join 卡满 20s
#
# 用法:
#   ./install-airport.sh                # 备份 + 检查 + 安装
#   ./install-airport.sh --check        # 只看指纹，不动 EFI
#   ./install-airport.sh --list         # 列出备份
#   ./install-airport.sh --rollback [备份名]
set -u

EFI_KEXT=/Volumes/H/EFI/OC/Kexts/AirportMtkWlan.kext
EFI_CONFIG=/Volumes/H/EFI/OC/config.plist
SRC=/Users/jianghh/WorkBuddy/2026-11-04-09-27-43/mtk-fix/SimpleMtkWlan-lidiaoo/build/Release/tahoe/AirportMtkWlan.kext
BAKDIR=/Users/jianghh/WorkBuddy/2026-11-04-09-27-43/mtk-fix/wex-mtk-backup

ts() { date +%Y%m%d-%H%M%S; }

fingerprint() {
  local b="$1"
  echo "  ver: $(plutil -p "$(dirname "$(dirname "$b")")/Info.plist" 2>/dev/null | sed -n 's/.*CFBundleShortVersionString" => "\(.*\)"/\1/p')"
  echo "  size: $(stat -f%z "$b" 2>/dev/null)  arch: $(lipo -archs "$b" 2>/dev/null)"
  echo "  sha256: $(shasum -a 256 "$b" | cut -d' ' -f1)"
  for s in "retries exhausted" "WCL scan diag" "has5g=" "publishWCLCachedResults" "mtkrxamsdu" "paced resend" "txstart mgmt subtype" "scan self-heal" "scan reject" "resend stopped, associd" "skip resend, came from RUN" "duplicate assoc resp dropped" "clearing stale aid" "Disassoc while RUN, reason" "WCL leave: abort join request" "WCL leave: airportd leaveNetwork cmd" "WCL leave: new join preempts" "WCL join complete status" "dead state %d without mgt" "ESS has no PSK, keeping the joined key" "with no PMK in state" "WCL join failed, parking in S_INIT" "reached RUN %d times in" "cached node hit" "deferred behind running scan" "AUTH: aborting running scan" "ignoring stale scan completion in state" "cached node unusable" "echo of our pre-auth deauth"; do
    printf "  %-18s %s\n" "$s" "$(strings -a "$b" 2>/dev/null | grep -c -- "$s")"
  done
}

do_list() {
  ls -1 "$BAKDIR" 2>/dev/null | grep '^AirportMtkWlan.kext.bak-' || echo "(无备份)"
}

do_check() {
  echo "== 编译产物 ==" ; [ -d "$SRC" ] && fingerprint "$SRC/Contents/MacOS/AirportMtkWlan" || echo "  (未编译)"
  echo "== EFI 上现有 =="
  [ -d "$EFI_KEXT" ] && fingerprint "$EFI_KEXT/Contents/MacOS/AirportMtkWlan" || echo "  (不存在)"
}

do_rollback() {
  local name="${1:-}"
  if [ -z "$name" ]; then
    name=$(ls -1t "$BAKDIR" 2>/dev/null | grep '^AirportMtkWlan.kext.bak-' | head -1)
  fi
  if [ -z "$name" ] || [ ! -d "$BAKDIR/$name" ]; then
    echo "找不到备份: ${name:-<空>}"; do_list; exit 1
  fi
  echo ">> 回滚到 $name"
  rm -rf "$EFI_KEXT"
  cp -R "$BAKDIR/$name" "$EFI_KEXT"
  echo ">> 完成，当前："
  fingerprint "$EFI_KEXT/Contents/MacOS/AirportMtkWlan"
  echo ">> 重启生效。"
}

case "${1:-}" in
  --list) do_list; exit 0 ;;
  --check) do_check; exit 0 ;;
  --rollback) do_rollback "${2:-}"; exit 0 ;;
esac

[ -d "$SRC" ] || { echo "新产物不存在: $SRC（先 ./build-macos.sh Release AirportMtkWlan）"; exit 1; }
[ -d "$EFI_KEXT" ] || { echo "EFI 上找不到 AirportMtkWlan.kext，确认 /Volumes/H 已挂载"; exit 1; }

echo ">> 预链接检查（kxld）"
kextutil -n "$SRC" 2>&1 | sed 's/^/   /' || true

mkdir -p "$BAKDIR"
NEWBAK="$BAKDIR/AirportMtkWlan.kext.bak-$(ts)"
echo ">> 备份旧版 -> $NEWBAK"
cp -R "$EFI_KEXT" "$NEWBAK"

echo ">> 旧版："; fingerprint "$NEWBAK/Contents/MacOS/AirportMtkWlan"

echo ">> 安装新版"
rm -rf "$EFI_KEXT"
cp -R "$SRC" "$EFI_KEXT"

echo ">> 新版："; fingerprint "$EFI_KEXT/Contents/MacOS/AirportMtkWlan"

echo ">> config.plist 校验"
plutil -lint "$EFI_CONFIG"
python3 - "$EFI_CONFIG" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
for i, e in enumerate(d['Kernel']['Add']):
    bp = e.get('BundlePath', '')
    if 'tk' in bp.lower():
        print(f'   [{i}] {bp}  Enabled={e.get("Enabled")}')
print('   DisableIoMapper =', d['Kernel']['Quirks'].get('DisableIoMapper'))
PY

echo
echo ">> 完成。重启后验证："
echo "   kextstat | grep AirportMtkWlan"
echo "   ioreg -l -w0 -r -c IOPCIDevice | grep -o '\"MtkDebugLog\" = \"[^\"]*' | head -c 400"
echo "   日志关键字：retries exhausted / paced resend / txstart mgmt subtype / has5g="
