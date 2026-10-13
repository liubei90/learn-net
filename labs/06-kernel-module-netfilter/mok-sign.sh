#!/usr/bin/env bash
#
# mok-sign.sh —— 给 netlab.ko 签名，以便在开启 Secure Boot 的机器上加载
#
# 背景（本机实测 2026-10-13）：
#   本机 UEFI Secure Boot 开启（SecureBoot=1），内核进入 lockdown=integrity，
#   运行时 /sys/module/module/parameters/sig_enforce = Y。
#   于是**未签名模块一律被拒**，即便你是 root：
#       insmod: ERROR: could not insert module netlab.ko: Key was rejected by service
#   这是安全策略，不是权限问题。
#
# 本脚本只做**安全的两步**：生成 MOK 密钥对 + 给模块附签名。
# 它**故意不做** mokutil --import，也不碰固件/EFI 变量 —— 那一步会改变
# 你机器的 Secure Boot 信任链，必须由你本人确认。脚本末尾会打印手动步骤。
#
# 用法：
#   bash mok-sign.sh                 # 生成 ~/mok 下的密钥并签名 netlab.ko
#   MOKDIR=/path bash mok-sign.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
KO="$HERE/netlab.ko"
MOKDIR="${MOKDIR:-$HOME/mok}"
KVER="$(uname -r)"
SIGN_FILE="/usr/src/linux-headers-$KVER/scripts/sign-file"

echo "=== 环境检查 ==="
printf '  内核           : %s\n' "$KVER"
printf '  Secure Boot    : %s\n' "$(od -An -t u1 -j4 \
    /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c 2>/dev/null | tr -d ' ' || echo '非 UEFI')"
printf '  lockdown       : %s\n' "$(cat /sys/kernel/security/lockdown 2>/dev/null || echo '(不可读)')"
printf '  sig_enforce    : %s\n' "$(cat /sys/module/module/parameters/sig_enforce 2>/dev/null || echo '(不存在)')"
printf '  sign-file      : %s\n' "$([ -x "$SIGN_FILE" ] && echo "$SIGN_FILE" || echo '缺失（需 linux-headers）')"
printf '  openssl/mokutil: %s / %s\n' "$(command -v openssl || echo 缺失)" "$(command -v mokutil || echo 缺失)"

[ -f "$KO" ] || { echo; echo "先构建模块：make -C \"$HERE\" clean all"; exit 1; }
[ -x "$SIGN_FILE" ] || { echo; echo "缺少 $SIGN_FILE，装 linux-headers-$(uname -r) 后再试"; exit 1; }

echo
echo "=== 1) 生成 MOK 密钥对（$MOKDIR）==="
mkdir -p "$MOKDIR"
cd "$MOKDIR"
if [ -f MOK.key ] && [ -f MOK.crt ]; then
    echo "  已存在 MOK.key/MOK.crt，复用（要换新的就删掉它们重跑）"
else
    openssl req -new -x509 -newkey rsa:2048 -keyout MOK.key -out MOK.crt \
            -nodes -days 3650 -subj "/CN=learn-net module signing/" 2>/dev/null
    echo "  已生成 MOK.key / MOK.crt"
fi
openssl x509 -in MOK.crt -out MOK.cer -outform DER
echo "  已导出 DER 格式公钥 MOK.cer"

echo
echo "=== 2) 给模块附签名（sha256）==="
cp -f "$KO" "$KO.unsigned.bak"
"$SIGN_FILE" sha256 MOK.key MOK.crt "$KO"
echo "  已签名：$KO（原文件备份为 $(basename "$KO").unsigned.bak）"
echo "  --- 签名信息 ---"
/sbin/modinfo "$KO" 2>/dev/null | grep -iE 'signer|sig_id|sig_hashalgo' | sed 's/^/    /' || true

cat <<EOF

=== 3) 剩下两步需要你本人做（会改变 Secure Boot 信任链）===

  # 登记公钥（会让你设一个一次性密码，重启时在 MOK Manager 里输入）
  sudo mokutil --import $MOKDIR/MOK.cer

  # 重启 → 开机出现蓝色 MOK Manager → Enroll MOK → Continue → 输入刚才的密码
  sudo reboot

  # 重启之后验证并加载
  sudo mokutil --list-enrolled | grep -i learn-net      # 应能看到
  sudo insmod $KO
  cat /proc/netlab_stats
  sudo rmmod netlab

=== 想撤销 ===
  sudo mokutil --delete $MOKDIR/MOK.cer   # 然后重启，在 MOK Manager 里确认删除
  # 或者直接恢复未签名版本：mv $KO.unsigned.bak $KO

=== 如果你不想碰 Secure Boot ===
  换一台没有 Secure Boot 的机器，或在 QEMU 虚拟机里跑（推荐，见 README 8.3）。
EOF
