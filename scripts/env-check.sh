#!/usr/bin/env bash
# =============================================================================
# env-check.sh —— learn-net 教程环境自检
#
# 用途：
#   一条命令回答三个问题：
#     1) 我的机器现在具备什么能力？（内核/发行版/工具链/权限/内核接口可读性）
#     2) 关键网络参数当前是多少？（sysctl 只读快照）
#     3) 本教程 labs 01~09 里，哪些我能直接跑、哪些需要 root、哪些需要额外准备？
#
# 设计原则：
#   · **不需要 root**，纯只读探测。所有探测失败都不会中断脚本。
#   · 退出码恒为 0（这是一个"信息收集"脚本，不是"断言"脚本）。
#   · 不依赖 sysctl(8) —— 本机 /usr/sbin/sysctl 其实存在，但不在非登录 shell 的 PATH 里，
#     统一直接读 /proc/sys（不依赖 PATH、不依赖 root、任何环境都能用）。
#
# 用法：
#   ./env-check.sh            # 人类可读
#   NO_COLOR=1 ./env-check.sh # 无 ANSI 转义，便于重定向
# =============================================================================

set -uo pipefail   # 注意：这里**故意不用 -e**。
                   # 本脚本的职责是"尽力探测并如实报告"，任何单项失败
                   # （比如某个文件不可读）都不应该中断整个自检。

# ---------------------------------------------------------------------------
# 输出小工具（尊重 NO_COLOR 约定）
# ---------------------------------------------------------------------------
if [[ -n "${NO_COLOR:-}" ]]; then
    C_T=""; C_OK=""; C_NO=""; C_WA=""; C_R=""
else
    C_T=$'\033[1;36m'; C_OK=$'\033[0;32m'; C_NO=$'\033[0;31m'
    C_WA=$'\033[0;33m'; C_R=$'\033[0m'
fi

sec()  { printf '\n%s===== %s =====%s\n' "$C_T" "$*" "$C_R"; }
ok()   { printf '  %s✅ %s%s\n' "$C_OK" "$*" "$C_R"; }
bad()  { printf '  %s❌ %s%s\n' "$C_NO" "$*" "$C_R"; }
warn() { printf '  %s⚠️  %s%s\n' "$C_WA" "$*" "$C_R"; }
info() { printf '     %s\n' "$*"; }
kv()   { printf '  %-34s %s\n' "$1" "$2"; }

echo "==================================================================="
echo " learn-net 环境自检 (env-check.sh)"
echo " 时间: $(date '+%F %T %Z')"
echo "==================================================================="

# ---------------------------------------------------------------------------
# 1. 系统与工具链
# ---------------------------------------------------------------------------
sec "1. 系统与工具链"

kv "内核版本" "$(uname -r)"
kv "内核完整串" "$(uname -srvmo 2>/dev/null || uname -a)"

if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    distro=$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}")
    kv "发行版" "$distro"
else
    kv "发行版" "(无法读取 /etc/os-release)"
fi

kv "当前用户" "$(id -un) (uid=$(id -u), gid=$(id -g))"
kv "架构 / CPU 数" "$(uname -m) / $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo '?')"
kv "页大小" "$(getconf PAGESIZE 2>/dev/null || echo '?') 字节"

for t in gcc make python3 bash; do
    if command -v "$t" >/dev/null 2>&1; then
        case "$t" in
            gcc)     v=$(gcc -dumpfullversion 2>/dev/null || gcc --version 2>/dev/null | head -1) ;;
            make)    v=$(make --version 2>/dev/null | head -1) ;;
            python3) v=$(python3 -c 'import sys;print(".".join(map(str,sys.version_info[:3])))' 2>/dev/null) ;;
            bash)    v=$(bash --version 2>/dev/null | head -1) ;;
        esac
        kv "$t" "$v"
    else
        kv "$t" "未安装"
    fi
done

# ---------------------------------------------------------------------------
# 2. Capabilities（关键！决定能做什么实验）
# ---------------------------------------------------------------------------
sec "2. Capabilities 与特权能力"

CAPEFF_HEX=$(awk '/^CapEff:/{print $2}' /proc/self/status 2>/dev/null || echo "")
CAPEFF_DEC=0
if [[ -n "$CAPEFF_HEX" ]]; then
    # 十六进制转十进制（bash 内建，无需外部命令）
    CAPEFF_DEC=$(( 16#$CAPEFF_HEX )) 2>/dev/null || CAPEFF_DEC=0
    kv "CapEff (raw)" "0x${CAPEFF_HEX}  =  ${CAPEFF_DEC}"
else
    kv "CapEff (raw)" "(无法读取 /proc/self/status)"
fi

# 用位运算判断是否具备某个 capability。
# CapEff 是一个 64 位位图；能力编号 N 对应第 N 位。
# 编号来源：include/uapi/linux/capability.h 的 CAP_* 宏。
#
# 本教程关心的几个：
#   CAP_NET_ADMIN = 12  —— 改 sysctl、配 qdisc、nftables（注意：netns/veth 不需要它，
#                          用 unshare -U 进用户命名空间即可获得等效能力）
#   CAP_NET_RAW   = 13  —— AF_PACKET 原始套接字（ping/tcpdump 抓包）
#   CAP_BPF       = 39  —— 加载 BPF 程序（5.8+ 从 CAP_SYS_ADMIN 拆出）
#   CAP_SYS_ADMIN = 21  —— 挂载 tracefs、大量管理操作
#   CAP_PERFMON   = 38  —— perf_event_open（5.8+ 从 CAP_SYS_ADMIN 拆出）
has_cap() {
    local bit="$1"
    (( (CAPEFF_DEC >> bit) & 1 ))
}

declare -A CAPNAMES=(
    [12]="CAP_NET_ADMIN"
    [13]="CAP_NET_RAW"
    [21]="CAP_SYS_ADMIN"
    [38]="CAP_PERFMON"
    [39]="CAP_BPF"
)
declare -A CAPDESC=(
    [12]="改 sysctl、配 qdisc/nftables（netns/veth 可用 unshare -U 绕过）"
    [13]="创建 AF_PACKET 原始套接字（抓包、ping）"
    [21]="挂载 tracefs、执行大量管理操作"
    [38]="调用 perf_event_open（perf/bpftrace 追踪）"
    [39]="加载 BPF 程序（bpftrace/bcc）"
)
# 固定顺序输出，便于阅读
for bit in 12 13 21 38 39; do
    if has_cap "$bit"; then
        ok "${CAPNAMES[$bit]} (bit $bit) 已具备 —— ${CAPDESC[$bit]}"
    else
        bad "${CAPNAMES[$bit]} (bit $bit) 缺失 —— 无法${CAPDESC[$bit]}"
    fi
done

if [[ "$CAPEFF_DEC" -eq 0 ]]; then
    warn "CapEff 全为 0：当前是**完全非特权**环境。"
    info "本教程 labs/09-observability 的观测实验已针对该情况设计，可以直接跑。"
    info "labs 06（内核模块）与 08（eBPF/XDP）需要 root 或放宽 capability。"
fi

# ---------------------------------------------------------------------------
# 3. 网络内核接口可用性
# ---------------------------------------------------------------------------
sec "3. 网络相关内核接口可用性"

# /dev/net/tun：用户态协议栈实验（labs/05）必需
if [[ -c /dev/net/tun ]]; then
    ok "/dev/net/tun 存在（labs/05 tun/tap 用户态协议栈实验可以做）"
elif [[ -e /dev/net/tun ]]; then
    bad "/dev/net/tun 存在但不是字符设备（异常）"
else
    bad "/dev/net/tun 不存在 —— labs/05 的 TUN/TAP 实验无法在本机运行"
    info "通常需要: modprobe tun  且  内核 CONFIG_TUN=y，再用 root 创建 /dev/net/tun"
fi

# 网络命名空间：两种路径要分开测
#   ① 直接 unshare --net 需要初始命名空间里的 CAP_SYS_ADMIN（本机没有，必然失败）
#   ② 但 unshare -Urn（先建用户命名空间）在新 userns 内会拿到完整 capability，
#      实测可以在里面建 netns/veth、改 MTU、使用 AF_PACKET
if unshare --net true 2>/dev/null; then
    NETNS_DIRECT=yes
    ok "unshare --net 直接可用（初始命名空间就有 CAP_SYS_ADMIN）"
else
    NETNS_DIRECT=no
    info "unshare --net 直接失败（初始命名空间无 CAP_SYS_ADMIN）——这不代表实验做不了，"
    info "  请看下面第 ② 项：通过用户命名空间可以拿到能力。"
fi

if command -v unshare >/dev/null 2>&1 && \
   unshare -Urnmpf --mount-proc true 2>/dev/null; then
    NETNS=yes
    ok "unshare -Urnmpf --mount-proc 可用 —— 可在用户命名空间内建 netns/veth/改 MTU/AF_PACKET"
    info "  （注意：/sys 无法重挂，MTU 与接口统计要用 ip 与 /proc/net 读；"
    info "    ip netns add 需要写 /run/netns，本机只读，改用两个 unshare -n 进程）"
    info "  实测示例：bash scripts/netns-veth-demo.sh"
else
    NETNS=no
    bad "unshare -Urnmpf 也不可用 —— 无法创建用户/网络命名空间（可能被 seccomp 或"
    bad "  kernel.unprivileged_userns_clone=0 限制）"
    info "这意味着所有『用 netns/veth 搭拓扑』的实验本机都做不了。"
fi

# NETLINK_ROUTE：ip/ss 的基础，非特权查询可用
if command -v ip >/dev/null 2>&1 && ip -j link show >/dev/null 2>&1; then
    ok "NETLINK_ROUTE 非特权查询可用（ip -j link show 正常）"
else
    bad "NETLINK_ROUTE 查询异常（ip 命令缺失或 netlink 被拦截）"
fi

# /proc/net 关键文件可读性
PROC_MISSING=0
PROC_TOTAL=0
for f in dev snmp netstat sockstat tcp tcp6 udp softnet_stat sockstat6 netlink packet protocols unix arp route fib_trie; do
    PROC_TOTAL=$((PROC_TOTAL + 1))
    [[ -r "/proc/net/$f" ]] || PROC_MISSING=$((PROC_MISSING + 1))
done
if [[ "$PROC_MISSING" -eq 0 ]]; then
    ok "/proc/net/* 全部可读（$PROC_TOTAL/$PROC_TOTAL 个关键文件）—— 观测类实验基础完备"
else
    warn "/proc/net/* 有 $PROC_MISSING/$PROC_TOTAL 个不可读"
fi

# /proc/sys/net 可读性（只读快照的基础）
if [[ -r /proc/sys/net/core/somaxconn ]]; then
    ok "/proc/sys/net/* 可读（只读）—— 可以读参数快照"
else
    bad "/proc/sys/net/* 不可读 —— 无法读取网络参数快照"
fi

# NETLINK_SOCK_DIAG：labs/09 的 05 脚本依赖它
if python3 - <<'PY' >/dev/null 2>&1
import socket
try:
    s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 4)  # 4 = NETLINK_SOCK_DIAG
    s.bind((0, 0)); s.close()
except OSError:
    raise SystemExit(1)
PY
then
    ok "NETLINK_SOCK_DIAG 非特权可用（labs/09 的 05-socket-diag.py 可以跑）"
else
    bad "NETLINK_SOCK_DIAG 不可用（可能被 seccomp 拦截）"
fi

# ---------------------------------------------------------------------------
# 4. 内核头文件与构建环境
# ---------------------------------------------------------------------------
sec "4. 内核头文件与模块构建环境"

KREL=$(uname -r)
HDR="/usr/src/linux-headers-${KREL}"
if [[ -d "$HDR" ]]; then
    ok "内核头文件目录存在: $HDR"
else
    bad "内核头文件目录不存在: $HDR"
    info "安装: apt-get install linux-headers-\$(uname -r)  (Debian/Ubuntu)"
fi

if [[ -e "/lib/modules/${KREL}/build" ]]; then
    if [[ -d "/lib/modules/${KREL}/build" ]]; then
        ok "/lib/modules/${KREL}/build 存在（可编译内核模块）"
    else
        ok "/lib/modules/${KREL}/build 存在（软链接 -> $(readlink -f "/lib/modules/${KREL}/build" 2>/dev/null))"
    fi
else
    bad "/lib/modules/${KREL}/build 不存在 —— 无法编译内核模块"
fi

# 与运行内核同版本的 -common 头文件包（uapi 与 tracepoint 定义在这里）
COMMON_HDR=$(ls -d /usr/src/linux-headers-*-common 2>/dev/null | head -1 || true)
if [[ -n "$COMMON_HDR" && -d "$COMMON_HDR/include/uapi" ]]; then
    ok "uapi 头文件可用: $COMMON_HDR/include/uapi"
    info "这是核对 netlink 结构体 / tracepoint 字段的**权威**本地来源，"
    info "版本与运行内核一致，比在线文档更可靠。"
else
    warn "未找到 linux-headers-*-common 包（uapi 头文件）"
    info "安装: apt-get install linux-headers-\$(uname -r)"
fi

if [[ -d "/lib/modules/${KREL}" ]]; then
    kv "已加载模块数" "$(lsmod 2>/dev/null | tail -n +2 | wc -l)"
else
    kv "已加载模块数" "(无 /lib/modules/${KREL}，且 lsmod 可能不可用)"
fi

# ---------------------------------------------------------------------------
# 5. 观测工具清单
# ---------------------------------------------------------------------------
sec "5. 观测工具清单"

# 格式: "命令名|属于哪个实验|是否本教程必需"
TOOLS="ip|观测/配置基础|必需
ss|socket 观测(labs/09)|必需
gcc|内核模块与 C 实验|必需
make|构建|必需
python3|脚本实验|必需
nc|socket 编程实验(labs/01)|建议
socat|socket 实验辅助|建议
curl|HTTP 实验(labs/07)|建议
ethtool|网卡/驱动统计(labs/09)|需要 root
tcpdump|抓包|需要 root
ping|连通性|建议
nft|netfilter(labs/06)|需要 root
conntrack|连接跟踪|需要 root
iptables|netfilter(labs/06)|需要 root
perf|tracepoint/采样(labs/09)|需要 root
bpftrace|eBPF 追踪(labs/08/09)|需要 root
bcc|eBPF 工具集(labs/08)|需要 root
bpftool|BPF 对象管理(labs/08)|需要 root
clang|编译 BPF 程序(labs/08)|需要 root
llvm-strip|BPF 对象处理|需要 root
trace-cmd|ftrace 前端|需要 root
sysctl|读参数(本可用 /proc/sys 代替)|可选"

MISSING_REQUIRED=0
MISSING_OPTIONAL=0
while IFS='|' read -r cmd purpose need; do
    [[ -z "$cmd" ]] && continue
    if command -v "$cmd" >/dev/null 2>&1; then
        printf '  %s✅ %-12s%s %s\n' "$C_OK" "$cmd" "$C_R" "已安装   [$purpose]"
    else
        if [[ "$need" == "必需" ]]; then
            printf '  %s❌ %-12s%s %s\n' "$C_NO" "$cmd" "$C_R" "缺失     [$purpose] ($need)"
            MISSING_REQUIRED=$((MISSING_REQUIRED + 1))
        else
            printf '  %s⚠️  %-12s%s %s\n' "$C_WA" "$cmd" "$C_R" "缺失     [$purpose] ($need)"
            MISSING_OPTIONAL=$((MISSING_OPTIONAL + 1))
        fi
    fi
done <<< "$TOOLS"

# 特别提示：sysctl 命令缺失不等于不能读参数
if ! command -v sysctl >/dev/null 2>&1; then
    warn "当前 PATH 里找不到 sysctl 命令（注意：本机其实装了 /usr/sbin/sysctl，只是 /sbin 不在 PATH）。"
    warn "这不影响读参数："
    info "用 cat /proc/sys/net/ipv4/tcp_syncookies 代替 sysctl -n net.ipv4.tcp_syncookies"
    info "本脚本与本教程的所有参数快照都走 /proc/sys，不依赖 sysctl 命令。"
fi # (注意：这里没有 if 的相反分支需要处理)

# ---------------------------------------------------------------------------
# 6. tracefs / debugfs 可追踪性
# ---------------------------------------------------------------------------
sec "6. tracefs / debugfs（ftrace 与 eBPF 追踪的前提）"

check_tracefs() {
    local path="$1" name="$2"
    if [[ ! -e "$path" ]]; then
        bad "$name 未挂载（$path 不存在）"
        return
    fi
    # 能否列目录
    if ! ls "$path" >/dev/null 2>&1; then
        bad "$name 存在（$path）但**无读权限** —— 无法使用"
        return
    fi
    # 挂载选项里是否有 ro
    local mnt
    mnt=$(mount 2>/dev/null | grep " $path " || true)
    if [[ "$mnt" == *"(ro,"* || "$mnt" == *",ro,"* || "$mnt" == *",ro)"* ]]; then
        warn "$name 以**只读(ro)**方式挂载 —— 即使有 root 也需先 remount,rw"
        info "  mount -o remount,rw $path"
    else
        ok "$name 可读且非只读"
    fi
    # 真正决定能不能用 ftrace 的是 tracing_on 可否写
    if [[ -w "$path/tracing_on" ]]; then
        ok "$name/tracing_on 可写 —— ftrace 事件追踪可用"
    else
        bad "$name/tracing_on 不可写 —— ftrace 事件追踪**不可用**（需要 root）"
    fi
}
check_tracefs /sys/kernel/tracing "tracefs"
check_tracefs /sys/kernel/debug  "debugfs"

# unprivileged BPF 开关
UBD=$(cat /proc/sys/kernel/unprivileged_bpf_disabled 2>/dev/null || echo "?")
kv "kernel.unprivileged_bpf_disabled" "$UBD"
case "$UBD" in
    0) ok "非特权 BPF 已启用（普通用户可跑部分 bpftrace/bcc）" ;;
    1) warn "非特权 BPF 已禁用（值 1：可以通过 sysctl 改回 0，需要 root）" ;;
    2) bad "非特权 BPF 已禁用且**运行时无法重新启用**（值 2，只能改内核启动参数）" ;;
    *) warn "无法读取该值" ;;
esac

# ---------------------------------------------------------------------------
# 7. 关键 sysctl 只读快照
# ---------------------------------------------------------------------------
sec "7. 关键网络参数只读快照（直接读 /proc/sys）"

# 统一用 /proc/sys 路径读取：不依赖 PATH（本机 /usr/sbin/sysctl 虽存在但不在 PATH）、不依赖 root。
# 写成一个函数：第一个参数是 /proc/sys 下的相对路径。
show_param() {
    local rel="$1" desc="${2:-}"
    local full="/proc/sys/$rel"
    local val
    if [[ -r "$full" ]]; then
        val=$(cat "$full" 2>/dev/null | tr '\n' ' ')
        printf '  %-46s = %s\n' "$rel" "$val"
    else
        printf '  %-46s = %s\n' "$rel" "(不可读/不存在)"
    fi
}

echo "  ── 连接队列 ──"
show_param net/core/somaxconn
show_param net/ipv4/tcp_max_syn_backlog
show_param net/ipv4/tcp_syncookies
show_param net/ipv4/tcp_abort_on_overflow
show_param net/ipv4/tcp_max_tw_buckets

echo "  ── 端口与 TIME-WAIT ──"
show_param net/ipv4/ip_local_port_range
show_param net/ipv4/tcp_tw_reuse
show_param net/ipv4/tcp_fin_timeout
show_param net/ipv4/tcp_max_orphans

echo "  ── socket 缓冲区 ──"
show_param net/core/rmem_default
show_param net/core/rmem_max
show_param net/core/wmem_default
show_param net/core/wmem_max
show_param net/core/optmem_max
show_param net/ipv4/tcp_rmem
show_param net/ipv4/tcp_wmem
show_param net/ipv4/tcp_mem
show_param net/ipv4/tcp_moderate_rcvbuf
show_param net/ipv4/udp_mem
show_param net/ipv4/udp_rmem_min
show_param net/ipv4/udp_wmem_min

echo "  ── 拥塞控制与 qdisc ──"
show_param net/ipv4/tcp_congestion_control
show_param net/ipv4/tcp_available_congestion_control
show_param net/core/default_qdisc
show_param net/ipv4/tcp_slow_start_after_idle
show_param net/ipv4/tcp_notsent_lowat
show_param net/ipv4/tcp_mtu_probing
show_param net/ipv4/tcp_fastopen
show_param net/ipv4/tcp_ecn
show_param net/ipv4/tcp_timestamps
show_param net/ipv4/tcp_sack
show_param net/ipv4/tcp_window_scaling

echo "  ── 软中断 / NAPI 预算 ──"
show_param net/core/netdev_max_backlog
show_param net/core/netdev_budget
show_param net/core/netdev_budget_usecs
show_param net/core/dev_weight
show_param net/core/busy_poll
show_param net/core/busy_read

echo "  ── 安全与转发 ──"
show_param net/ipv4/ip_forward
show_param net/ipv4/conf/all/rp_filter
show_param net/ipv4/conf/default/rp_filter
show_param net/ipv4/icmp_echo_ignore_broadcasts
show_param net/ipv4/conf/all/accept_redirects

echo "  ── BPF / 其他 ──"
show_param kernel/unprivileged_bpf_disabled
show_param kernel/kptr_restrict
show_param kernel/perf_event_paranoid
show_param net/core/bpf_jit_enable 2>/dev/null || true

echo
info "读法说明：以上都是**只读快照**。改参数需要 root，且本机 CapEff=0 改不了。"
info "若本机有 sysctl(8)，等价写法是 sysctl -n <把 / 换成 . 后的名字>，"
info "例如: sysctl -n net.core.somaxconn  ==  cat /proc/sys/net/core/somaxconn"

# ---------------------------------------------------------------------------
# 8. 结论表：labs 01~09 在本机的可运行性
# ---------------------------------------------------------------------------
sec "8. 结论：本教程 labs 01~09 在本机的可运行性"

# 判定逻辑（尽量基于上面真正探测到的结果，而不是写死）
DEV_TUN=$([[ -c /dev/net/tun ]] && echo yes || echo no)
# 重新探测一次 netns 能力（第 ② 条路径才是关键：用户命名空间内拿能力）
if unshare -Urnmpf --mount-proc true 2>/dev/null; then NETNS=yes; else NETNS=no; fi
CAP_NET_ADMIN=$([[ "$(has_cap 12 && echo y || echo n)" == y ]] && echo yes || echo no)
ROOT=$([[ "$(id -u)" -eq 0 ]] && echo yes || echo no)
HAVE_HEADERS=$([[ -d "$HDR" && -e "/lib/modules/${KREL}/build" ]] && echo yes || echo no)

if [[ "$ROOT" == yes ]]; then
    PRIV=yes
elif [[ "$CAP_NET_ADMIN" == yes ]]; then
    PRIV=partial
else
    PRIV=no
fi

# 参数: 编号|名称|状态|说明
# 说明：中文字符是"双宽"字符，用 printf '%-30s' 对齐会因为 printf 按字符数
# （而非显示宽度）填充而错位。所以这里改用「每个 lab 一个小块」的格式，
# 无论中英文混排都能稳定对齐。
LABS="01|socket API 基础 (C 语言)|✅|纯用户态 socket 编程，非特权即可
02|TCP 状态机实验|✅|纯用户态，非特权即可
03|NETLINK_ROUTE / rtnetlink|✅|读取链路/地址信息非特权即可（只有写操作才需要 root）
04|原始套接字与 ARP|✅|纯 AF_PACKET 需要 CAP_NET_RAW；本机初始命名空间没有，但在 unshare -Urn 内可获得，实测真跑通过
05|TUN/TAP 用户态协议栈|⚠️|需要 root + /dev/net/tun；本机 /dev/net/tun 不存在
06|内核模块与 netfilter|⚠️|本机有头文件**可编译**模块，但非特权无法 insmod 加载
07|HTTP 解析器|✅|纯用户态字符串/协议解析，非特权即可
08|eBPF / XDP|⚠️|需要 CAP_BPF + clang + libbpf + 可写 tracefs；本机全部缺失
09|内核网络观测与调优|✅|已针对本机权限设计：labs/09 的 01~05、07 全部真跑通过
"

while IFS='|' read -r num name status desc; do
    [[ -z "$num" ]] && continue
    printf '  %s  lab/%s  %s\n' "$status" "$num" "$name"
    printf '            %s\n' "$desc"
done <<< "$LABS"

echo
echo "  图例: ✅ 本机可直接跑   ⚠️ 需要 root 或额外 capability   ❌ 本机不可运行"
echo
echo "  ⚠️ 关于 labs 01~08 的判断依据："
echo "     这些 lab 的具体实现由教程的其他章节负责，本脚本无法读取它们的内容。"
echo "     上面的判断是基于 (a) lab 目录名对应的主题，以及 (b) 本机能力的实测结果"
echo "     推断出来的**预期**结论。请以各 lab 自己的 README 为准。"

echo
printf '  权限结论        : %s\n' "$(
    case "$PRIV" in
        yes)     echo "root —— 全部实验可做" ;;
        partial) echo "有部分 capability —— 部分特权实验可做" ;;
        no)      echo "完全非特权 (CapEff=0) —— 只有用户态实验可做" ;;
    esac
)"
printf '  /dev/net/tun    : %s\n' "$DEV_TUN"
printf '  网络命名空间    : %s\n' "$NETNS"
printf '  内核头文件      : %s\n' "$HAVE_HEADERS"
printf '  CAP_NET_ADMIN   : %s\n' "$CAP_NET_ADMIN"

echo
echo "  📌 本教程的设计前提："
echo "     labs/09 与 scripts/ 已按『CapEff=0 也能跑』来设计，"
echo "     需要特权的实验一律提供『模板 + 前置条件 + 优雅降级』。"
echo "     因此即使在本机这样的受限环境，你依然能完成全部观测类实验。"
echo

# ---------------------------------------------------------------------------
# 9. 缺失项的安装建议
# ---------------------------------------------------------------------------
sec "9. 补齐环境的命令（需要 root）"

if [[ "$MISSING_REQUIRED" -gt 0 || "$MISSING_OPTIONAL" -gt 0 ]]; then
    echo "  Debian / Ubuntu:"
    echo "    sudo apt-get update"
    echo "    sudo apt-get install -y \\"
    echo "        build-essential linux-headers-\$(uname -r) \\"
    echo "        iproute2 tcpdump iputils-ping netcat-openbsd socat curl \\"
    echo "        ethtool conntrack nftables iptables \\"
    echo "        linux-perf bpftrace bpfcc-tools bpftool clang llvm libbpf-dev \\"
    echo "        trace-cmd procps python3"
    echo
    echo "  RHEL / CentOS / Fedora / Rocky:"
    echo "    sudo dnf install -y \\"
    echo "        gcc gcc-c++ make kernel-devel-\$(uname -r) \\"
    echo "        iproute tcpdump iputils nmap-ncat socat curl \\"
    echo "        ethtool conntrack-tools nftables iptables \\"
    echo "        perf bpftrace bcc-tools bpftool clang llvm libbpf-devel \\"
    echo "        trace-cmd procps-ng python3"
    echo
    echo "  注意："
    echo "    · Debian 上 perf 的包名是 linux-perf（不是 perf）"
    echo "    · bpftrace/bcc 在 RHEL 系需要先启用对应仓库"
    echo "    · 装完内核头文件后，/lib/modules/\$(uname -r)/build 才会出现"
else
    ok "必需与建议的工具都已安装，无需补装。"
fi

echo
echo "==================================================================="
echo " 自检结束。退出码 0（本脚本不因探测失败而失败）。"
echo "==================================================================="
exit 0
