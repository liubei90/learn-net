#!/usr/bin/env bash
# =============================================================================
# 02-proc-net-snapshot.sh —— 快照 /proc/net 下的核心文件
#
# 对应文档: docs/09-内核网络观测与性能调优.md 第 2 节（/proc/net 逐项解释）
#
# 为什么要有这个脚本:
#   /proc/net/* 是内核网络栈最"素"的对外接口。在容器里、在没有 ss/ethtool
#   的精简镜像里，它往往是唯一还能用的观测手段。而且它全部是只读的，
#   非特权用户也能读 —— 本机 CapEff=0 实测完全可读。
#
# 本脚本抓取关键片段并逐段解释。完整内容请自行 cat。
# 权限: 不需要 root。
# =============================================================================

set -euo pipefail

# 尊重 NO_COLOR 约定（见 https://no-color.org/）
if [[ -n "${NO_COLOR:-}" ]]; then
    C_TITLE=""; C_NOTE=""; C_RESET=""
else
    C_TITLE=$'\033[1;36m'; C_NOTE=$'\033[0;33m'; C_RESET=$'\033[0m'
fi

sec()  { printf '\n%s===== %s =====%s\n' "$C_TITLE" "$*" "$C_RESET"; }
note() { printf '%s[解读]%s %s\n' "$C_NOTE" "$C_RESET" "$*"; }
head_n() { head -n "$1" "$2"; }

echo "==================================================================="
echo " /proc/net 快照 —— 内核: $(uname -r)  时间: $(date '+%F %T %Z')"
echo "==================================================================="

# ---------------------------------------------------------------------------
# /proc/net/dev —— 每张网卡的收发字节/包/错误/丢包计数
# ---------------------------------------------------------------------------
sec "① /proc/net/dev（每张网卡的收发统计）"
cat /proc/net/dev || true
note "列的顺序（内核固定）："
note "  face | bytes packets errs drop fifo frame compressed multicast | (以上为接收)"
note "       | bytes packets errs drop fifo colls carrier compressed   (以上为发送)"
note "从第 3 行开始每行一张网卡，第 1 列是 '接口名:'。"
note "drop 计数增加说明内核在**协议栈层面**丢包（缓冲区不足），"
note "通常对应 /proc/net/softnet_stat 的第 2 列，或 socket 队列溢出。"

# ---------------------------------------------------------------------------
# /proc/net/snmp —— IP/TCP/UDP 等协议的 MIB 计数器（RFC 1213 风格）
# ---------------------------------------------------------------------------
sec "② /proc/net/snmp（协议级 MIB 计数器）"
note "格式很特殊：**奇数行是字段名，偶数行是对应的值**，两行一组。"
head_n 6 /proc/net/snmp || true
echo "  ... （完整内容请自行 cat /proc/net/snmp）"
note "Tcp: 那一组里最常看的字段："
note "  ActiveOpens / PassiveOpens —— 主动/被动建连次数（判断是客户端还是服务端角色）"
note "  AttemptFails               —— 建连失败次数（SYN 被拒/超时）"
note "  EstabResets                —— 已建立的连接被 RST 掉的次数"
note "  RetransSegs                —— 重传报文段数（对外网络质量最直接的指标）"
note "  InErrs / InCsumErrors      —— 收到错误段 / 校验和错误（后者常指向网卡/线路问题）"
note "  OutRsts                    —— 本机发出的 RST 数"

# ---------------------------------------------------------------------------
# /proc/net/netstat —— 扩展 MIB（TcpExt / IpExt / MPTcpExt）
# ---------------------------------------------------------------------------
sec "③ /proc/net/netstat（扩展 MIB，关键是 TcpExt 组）"
note "同样是『一行名字 + 一行值』成对出现，共 3 对：TcpExt / IpExt / MPTcpExt"
awk 'NR<=2' /proc/net/netstat | cut -c1-200 || true
echo "  ... （只截取了 TcpExt 的前 200 字符）"
note "TcpExt 是丢包排查的金矿，本机实测存在的关键计数器包括："
note "  ListenOverflows / ListenDrops —— accept 队列溢出 / 因队列满而丢弃的 SYN"
note "  TCPTimeouts                   —— RTO 超时次数（重传的根因之一）"
note "  TCPLostRetransmit             —— 重传的包又丢了（网络严重拥塞的信号）"
note "  TCPOFOQueue                   —— 乱序队列（Out-Of-Order）入队次数"
note "  TCPBacklogDrop / TCPRcvQDrop  —— 收包 backlog 满 / 接收队列满而丢包"
note "  TCPAbortOnMemory              —— 因内存压力主动断开连接"
note "  TCPTimeWaitOverflow           —— TIME-WAIT 表溢出"
note "用 labs/09-observability/04-snmp-counters.sh 可以看到这些值的当前读数。"

# ---------------------------------------------------------------------------
# /proc/net/sockstat —— 各协议 socket 数量与内存占用（内核全局汇总）
# ---------------------------------------------------------------------------
sec "④ /proc/net/sockstat（socket 数量与内存）"
cat /proc/net/sockstat || true
note "  sockets: used <总 socket 数>"
note "  TCP: inuse <已用> orphan <无主(应用已关但内核还在用)> tw <TIME-WAIT> alloc <已分配> mem <页数>"
note "  UDP: inuse <已用> mem <页数>"
note "  FRAG / RAW 同理。"
note "mem 的单位是**页**（本机 4096 字节），不是字节 —— 这是最常见的误读。"
note "orphan 持续增长通常意味着应用 close() 了但内核还在发送缓冲区里排队（发送慢）。"

# ---------------------------------------------------------------------------
# /proc/net/tcp —— TCP socket 表（十六进制！）
# ---------------------------------------------------------------------------
sec "⑤ /proc/net/tcp（前 8 行）"
head_n 8 /proc/net/tcp || true
note "⚠️ 这个文件里地址和端口都是**十六进制**，最容易读错："
note "  local_address 格式为 'HEX_IP:HEX_PORT'，0100007F:1F90"
note "    -> IP 0100007F 按小端读 = 127.0.0.1，端口 0x1F90 = 8080"
note "  st 列是 TCP 状态码，用十六进制表示："
note "    01=ESTABLISHED 02=SYN_SENT 03=SYN_RECV 04=FIN_WAIT1 05=FIN_WAIT2"
note "    06=TIME_WAIT   07=CLOSE    08=CLOSE_WAIT 09=LAST_ACK 0A=LISTEN 0B=CLOSING"
note "  tx_queue:rx_queue 就是 ss 显示的 Send-Q:Recv-Q（同样是十六进制）。"
note "  inode 列可以和 /proc/<pid>/fd/ 里的 socket:[inode] 对上，用于定位进程。"

# ---------------------------------------------------------------------------
# /proc/net/udp —— 无连接协议
# ---------------------------------------------------------------------------
sec "⑥ /proc/net/udp（前 5 行）"
head_n 5 /proc/net/udp || true
note "UDP 没有连接状态机，st 几乎总是 07(CLOSE)，除非是 connect() 过的 UDP。"
note "排查 UDP 丢包主要看 drops 列（第 13 列附近）和 /proc/net/snmp 的 Udp: 组。"

# ---------------------------------------------------------------------------
# /proc/net/softnet_stat —— 每个 CPU 的收包统计（丢包排查第一现场）
# ---------------------------------------------------------------------------
sec "⑦ /proc/net/softnet_stat（每 CPU 一行，共 $(wc -l < /proc/net/softnet_stat) 行）"
note "本机实测：每行有 $(awk 'NR==1{print NF}' /proc/net/softnet_stat) 列，且每列都是 **8 位十六进制**（%08x），"
note "千万不要按十进制读！例如 000a5e9f 是十六进制的 679583。"
cat /proc/net/softnet_stat || true
note "第 1 列 = 该 CPU 处理的报文数；第 2 列 = 因 backlog 队列满而丢弃的报文数。"
note "第 2 列持续增长 = 软中断处理不过来，需要调 net.core.netdev_max_backlog 或做多队列/RPS。"
note "详细的逐列解释（含列数随版本变化的注意事项）见 03-softnet-and-drops.sh。"

# ---------------------------------------------------------------------------
# /proc/net/arp 与 route —— 转发表
# ---------------------------------------------------------------------------
sec "⑧ /proc/net/arp 和 /proc/net/route"
echo "--- arp ---"; cat /proc/net/arp || true
echo "--- route ---"; cat /proc/net/route || true
note "arp 表里的 Flags 0x2 表示条目有效（complete）。"
note "route 表字段同样是十六进制且**小端**：Destination 00000000 = default。"
note "IPv6 路由在 /proc/net/ipv6_route。"

# ---------------------------------------------------------------------------
# /proc/net/protocols 与 packet/netlink —— 更冷门但排障有用
# ---------------------------------------------------------------------------
sec "⑨ /proc/net/protocols（各传输层协议的内存用量与回调表，前 12 行）"
head_n 12 /proc/net/protocols || true
note "sysctl 下 mem 一列是该协议当前消耗的 socket 缓冲区内存，"
note "排查『谁把内存吃光了』时非常有用。"

sec "⑩ /proc/net/packet 与 /proc/net/netlink（前 5 行）"
echo "--- packet ---"; head_n 5 /proc/net/packet || true
echo "--- netlink ---"; head_n 5 /proc/net/netlink || true
note "packet 是 AF_PACKET 原始套接字的表（本机无 CAP_NET_RAW，通常为空）。"
note "netlink 列出内核 netlink socket；ss/ip 的通信就依赖它。"

echo
echo "==================================================================="
echo " 小结：/proc/net 的读取成本极低（就是内核里一个 seq_file 回调），"
echo " 但它是**累计计数器**，只有看『增量』才有意义。"
echo " 正确用法：间隔采样两次做差（period=1s 以上），或看它是否在持续增长。"
echo " 本目录的 07-watch-tcp-state.sh 演示了如何做带时间序列的采样。"
echo "==================================================================="
