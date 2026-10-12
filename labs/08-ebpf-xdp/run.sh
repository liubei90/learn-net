#!/usr/bin/env bash
#
# run.sh —— labs/08 一键实验
#
# 设计要点：
#   · 本机无法加载 eBPF（CapEff=0 + unprivileged_bpf_disabled=2），
#     所以脚本的「成功」标准是：**编译零警告 + 诚实地报告为什么跑不了**，
#     而不是伪造成功输出。
#   · 工具链齐全的机器上，同一脚本会自动多跑一段 XDP 构建。
set -uo pipefail

cd "$(dirname "$0")"

hr() { printf '\n\033[1;36m━━━ %s ━━━\033[0m\n' "$1"; }

hr "1. 工具链与权限自检"
make --no-print-directory check-toolchain
printf '  CapEff                          : %s\n' "$(awk '/CapEff/{print $2}' /proc/self/status)"
printf '  /sys/kernel/tracing 可读        : %s\n' \
    "$(ls /sys/kernel/tracing >/dev/null 2>&1 && echo yes || echo 'no（权限不够）')"
printf '  bpftrace / bpftool              : %s / %s\n' \
    "$(command -v bpftrace || echo 缺失)" "$(command -v bpftool || echo 缺失)"

hr "2. 构建（必须零警告；缺 clang/libbpf 时 XDP 示例打印 SKIP 且退出码 0）"
make clean all
echo "  构建退出码：$?"

hr "3. 运行手写指令示例（BPF_MAP_CREATE → BPF_PROG_LOAD → SO_ATTACH_BPF → 读 map）"
if [[ -x ./bpf_socket_filter ]]; then
    ./bpf_socket_filter
    rc=$?
    echo
    echo "  程序退出码：$rc（0 = 正常，包含「环境不支持因此 SKIP」这种情况）"
else
    echo "  ✗ 没有生成 bpf_socket_filter，说明构建失败"
    exit 1
fi

hr "4. 当前网卡上是否已挂 XDP 程序"
if command -v ip >/dev/null 2>&1; then
    ip -o link show | grep -o 'xdp' | head -1 >/dev/null 2>&1 \
        && echo "  有网卡挂了 xdp" || echo "  没有任何网卡挂 XDP 程序（符合预期，本机无法加载）"
fi

hr "5. 小结"
cat <<'TXT'
  本实验在本机能验证什么、不能验证什么：

  ✅ 能验证：
     · 手写 eBPF 指令的编码是正确的（编译通过，且每条指令的 code 值都来自 UAPI 头文件）
     · 加载/挂载所需的 syscall 序列与参数结构（bpf_attr 字段名全部来自本机 bpf.h）
     · 失败时的 errno 与原因（EPERM ← CapEff=0 + unprivileged_bpf_disabled=2）

  ❌ 不能验证（需要 root 或 CAP_BPF）：
     · 程序能否通过验证器
     · SO_ATTACH_BPF 挂载后统计是否真的递增
     · XDP 程序的编译（需要 clang -target bpf）与挂载（需要 CAP_NET_ADMIN）

  在有 root 的机器上，把第 3 步换成：
     sudo ./bpf_socket_filter
  并把第 2 步的 XDP 部分补上依赖后执行：
     sudo apt-get install -y clang llvm libbpf-dev
     make xdp && sudo ./xdp_drop lo
TXT
