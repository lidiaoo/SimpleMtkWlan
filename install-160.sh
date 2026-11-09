#!/bin/bash
# 安装 lidiaoo/SimpleMtkWlan HEAD 编译出的 SimpleMtkWlan.kext（虚拟有线网卡版）
# 用法:
#   ./install-160.sh            # 安装新版本（自动备份旧版）
#   ./install-160.sh --list     # 列出备份
#   ./install-160.sh --rollback [备份名]  # 回滚（默认最近一次）
set -u

EFI_KEXT=/Volumes/H/EFI/OC/Kexts/SimpleMtkWlan.kext
EFI_CONFIG=/Volumes/H/EFI/OC/config.plist
SRC=/Users/jianghh/WorkBuddy/2026-11-04-09-27-43/mtk-fix/SimpleMtkWlan-lidiaoo/build/Build/Products/Release/SimpleMtkWlan.kext
BAKDIR=/Users/jianghh/WorkBuddy/2026-11-04-09-27-43/mtk-fix/wex-mtk-backup

ts() { date +%Y%m%d-%H%M%S; }

do_list() {
  ls -1 "$BAKDIR" 2>/dev/null | grep '^SimpleMtkWlan.kext.bak-' || echo "(无备份)"
}

do_rollback() {
  local name="${1:-}"
  if [ -z "$name" ]; then
    name=$(ls -1t "$BAKDIR" 2>/dev/null | grep '^SimpleMtkWlan.kext.bak-' | head -1)
  fi
  if [ -z "$name" ] || [ ! -d "$BAKDIR/$name" ]; then
    echo "找不到备份: ${name:-<空>}"; do_list; exit 1
  fi
  echo ">> 回滚到 $name"
  rm -rf "$EFI_KEXT"
  cp -R "$BAKDIR/$name" "$EFI_KEXT"
  echo ">> 完成。当前版本:"
  plutil -p "$EFI_KEXT/Contents/Info.plist" | grep -E 'CFBundleShortVersionString'
  shasum -a 256 "$EFI_KEXT/Contents/MacOS/SimpleMtkWlan"
  echo ">> 需要重启生效。"
}

case "${1:-}" in
  --list) do_list; exit 0 ;;
  --rollback) do_rollback "${2:-}"; exit 0 ;;
esac

[ -d "$SRC" ] || { echo "新产物不存在: $SRC（先编译）"; exit 1; }
[ -d "$EFI_KEXT" ] || { echo "EFI 上找不到 SimpleMtkWlan.kext，确认 /Volumes/H 已挂载"; exit 1; }

mkdir -p "$BAKDIR"
NEWBAK="$BAKDIR/SimpleMtkWlan.kext.bak-$(ts)"
echo ">> 备份旧版 -> $NEWBAK"
cp -R "$EFI_KEXT" "$NEWBAK"

echo ">> 旧版信息:"
plutil -p "$NEWBAK/Contents/Info.plist" | grep -E 'CFBundleShortVersionString'
shasum -a 256 "$NEWBAK/Contents/MacOS/SimpleMtkWlan" | sed 's/^/   /'

echo ">> 安装新版"
rm -rf "$EFI_KEXT"
cp -R "$SRC" "$EFI_KEXT"

echo ">> 新版信息:"
plutil -p "$EFI_KEXT/Contents/Info.plist" | grep -E 'CFBundleShortVersionString|CFBundleVersion|IOPCIPrimaryMatch'
shasum -a 256 "$EFI_KEXT/Contents/MacOS/SimpleMtkWlan" | sed 's/^/   /'
echo -n "   arch: "; lipo -archs "$EFI_KEXT/Contents/MacOS/SimpleMtkWlan"

echo ">> config.plist 校验"
plutil -lint "$EFI_CONFIG"
python3 - "$EFI_CONFIG" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
for e in d['Kernel']['Add']:
    if 'SimpleMtkWlan' in e.get('BundlePath', ''):
        print('   entry:', e.get('BundlePath'), 'Enabled=', e.get('Enabled'),
              'Exec=', e.get('ExecutablePath'), 'Min=', repr(e.get('MinKernel')),
              'Max=', repr(e.get('MaxKernel')))
q = d['Kernel']['Quirks']
print('   DisableIoMapper =', q.get('DisableIoMapper'))
PY

echo
echo ">> 完成。重启后验证："
echo "   kextstat | grep SimpleMtkWlan        # 应显示 1.6.0（新构建）"
echo "   bash mtk-fix/SimpleMtkWlan-1.1.0/mtklog.sh | grep -E 'RX A-MSDU|rxamsdu'"
