#!/usr/bin/env bash
# =============================================================================
# 01-socket-overview.sh —— 观测漏斗的第一层：从 socket 视角俯瞰本机网络
#
# 对应文档: docs/09-内核网络观测与性能调优.md 第 1 节（观测方法论）
#
# 这个脚本做什么:
#   把 `ss` 的几种"看全局"的用法串起来，并对每一段输出做解读标注。
#   观测漏斗的第一步永远是"先看规模和分布"，而不是一上来就抓包。
#
# 权限: 不需要 root。非特权用户只能看到自己的进程名（ss -p），
#       但 socket 的队列/状态信息是全局可见的。
# =============================================================================

set -euo pipefail

# 尊重 NO_COLOR 约定（见 https://no-color.org/）：
# 设置 NO_COLOR=1 运行可得到不含 ANSI 转义的纯文本，方便重定向到文件。
if [[ -n "${NO_COLOR:-}" ]]; then
    C_TITLE=""; C_NOTE=""; C_WARN=""; C_RESET=""
else
    C_TITLE=$'\033[1;36m'; C_NOTE=$'\033[0;33m'
    C_WARN=$'\033[0;31m'; C_RESET=$'\033[0m'
fi

# 统一的输出小标题，方便阅读长输出
sec()  { printf '\n%s===== %s =====%s\n' "$C_TITLE" "$*" "$C_RESET"; }
note() { printf '%s[解读]%s %s\n' "$C_NOTE" "$C_RESET" "$*"; }

# ss 是 iproute2 的一部分；它在底层使用 NETLINK_SOCK_DIAG
if ! command -v ss >/dev/null 2>&1; then
    echo "SKIP: 未找到 ss 命令（iproute2 未安装）。本机需要 iproute2 才能运行该实验。"
    exit 0
fi

echo "==================================================================="
echo " socket 总览观测 —— 主机: $(hostname)  内核: $(uname -r)"
echo " 时间: $(date '+%F %T %Z')"
echo "==================================================================="

# ---------------------------------------------------------------------------
# 1. 汇总：本机到底有多少 socket
# ---------------------------------------------------------------------------
sec "① 汇总统计：ss -s"
ss -s || true
note "ss -s 给出各协议族的 socket 总量与内存占用。"
note "先看这里的原因：如果 total 数量异常（比如 TIME-WAIT 上万），"
note "排查方向就已经确定了（端口耗尽 / 短连接风暴），不需要看单条连接。"

# ---------------------------------------------------------------------------
# 2. 监听端口：本机对外提供了什么服务
# ---------------------------------------------------------------------------
sec "② 监听中的端口：ss -lntup"
note "l=listening n=不解析域名 t=TCP u=UDP p=显示进程"
ss -lntup || true
note "重点看两件事："
note "  1) Send-Q 列对 LISTEN socket 而言是**已完成三次握手、等待 accept() 的队列长度**"
note "     （内核里对应 struct sock 的全连接队列，受 net.core.somaxconn 与 listen(fd, backlog) 共同限制）。"
note "     这一列持续 >0 且波动，说明应用 accept() 太慢。"
note "  2) 0.0.0.0 与 127.0.0.1 的差别是暴露面（前者所有网卡，后者仅本机回环）。"

# ---------------------------------------------------------------------------
# 3. TCP 连接明细 + 内核内部指标
# ---------------------------------------------------------------------------
sec "③ TCP 连接明细（含内核状态）：ss -tni（只显示前 25 行）"
note "i=internal，展示内核 TCP 栈内部变量：拥塞算法、rtt、cwnd、retrans、定时器"
if ss -tni 2>/dev/null | head -n 25; then :; fi
echo "  ... （完整输出请自行运行: ss -tni）"
note "关键字段中文对照（这些字段来自内核 struct tcp_info，经 NETLINK_SOCK_DIAG 导出）："
note "  cubic        —— 拥塞控制算法（net.ipv4.tcp_congestion_control 的当前生效值）"
note "  rtt:X/Y      —— 平滑 RTT / RTT 抖动，单位毫秒"
note "  rto:X        —— 重传超时时间（Retransmission Timeout, RTO），单位毫秒"
note "  cwnd:X       —— 拥塞窗口（Congestion Window），单位是 MSS 的倍数"
note "  ssthresh:X   —— 慢启动阈值"
note "  retrans:X/Y  —— 当前重传计数 / 总重传次数"
note "  unacked:X    —— 已发出但未被确认的报文数（>0 且 cwnd 满 = 发送受阻）"

# ---------------------------------------------------------------------------
# 4. Unix domain socket：本机进程间通信
# ---------------------------------------------------------------------------
sec "④ Unix domain socket 汇总：ss -x（前 15 行）"
if ss -x 2>/dev/null | head -n 15; then :; fi
echo "  ... （完整输出请自行运行: ss -x）"
note "Unix socket 不走 TCP/IP 协议栈，但同样有 Send-Q/Recv-Q 队列。"
note "本地 RPC（如 systemd、Docker、数据库 socket）变慢时，这里是最直接的证据。"

sec "⑤ Unix socket 总数统计"
if ss -x 2>/dev/null | tail -n +2 | wc -l | xargs printf '  unix socket 条目数: %s\n'; then :; fi
note "注意：ss -x 会把处于监听和已连接状态的都列出来，所以这个数字 >= 实际 socket 数。"

# ---------------------------------------------------------------------------
# 6. 把 ssl 的队列信息提炼成"是否需要进一步排查"的结论
# ---------------------------------------------------------------------------
sec "⑥ 自动判读：有没有明显异常"
if ss -lnt 2>/dev/null | awk 'NR>1 && $2 > 0 {printf "  ⚠️  LISTEN %s 的 accept 队列积压 = %s\n", $4, $2; found=1} END{if(!found) print "  ✅ 未发现 LISTEN 队列积压（所有监听 socket 的 Send-Q = 0）"}'; then :; fi
if ss -tn 2>/dev/null | awk 'NR>1 && $2 > 0 {n++; if(n<=5) printf "  ⚠️  Recv-Q 积压 %s 字节 -> %s\n", $2, $4} END{if(n==0) print "  ✅ 未发现 TCP 接收队列积压（所有 Recv-Q = 0）"; else printf "  （共 %d 条连接 Recv-Q > 0；Recv-Q 持高说明应用读取速度跟不上）\n", n}'; then :; fi
note "Recv-Q 对已连接 socket 表示内核已收到、但应用还没 read() 的字节数。"
note "它=0 是好消息；它长期 >0 甚至增长到接近 rmem_max，就是典型的『应用读取慢』。"

echo
echo "==================================================================="
echo " 本实验对应的内核源码入口（以 6.12 为准，可在"
echo " elixir.bootlin.com/linux/v6.12/source/ 核对）："
echo "   net/core/sock_diag.c        —— NETLINK_SOCK_DIAG 的注册与分发"
echo "   net/ipv4/inet_diag.c        —— inet_diag_msg 的填充（inet_sk_diag_fill 等）"
echo "   net/ipv4/tcp_diag.c         —— TCP 的 diag handler"
echo "   include/net/tcp_states.h    —— TCPF_* 状态位掩码的定义"
echo "==================================================================="
