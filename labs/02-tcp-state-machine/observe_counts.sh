#!/usr/bin/env bash
# labs/02-tcp-state-machine/observe_counts.sh
# 读 /proc/net/snmp 与 /proc/net/netstat 的 TCP 计数，并逐项解释。
# 只读操作，非 root 可用；需要写 sysctl 的步骤在输出中显式标注为不可运行。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="$HERE/parse_proc_tcp.py"

snapshot() {
    echo "== /proc/net/snmp 的 Tcp 行（RFC 1213 标准 MIB）=="
    awk '/^Tcp:/{n++; print (n==1 ? "[names] " : "[values] ")$0}' /proc/net/snmp
    echo
    echo "== /proc/net/sockstat =="
    cat /proc/net/sockstat
    echo
    echo "== /proc/net/sockstat6 =="
    cat /proc/net/sockstat6
}

explain_snmp() {
    cat <<'EOF'
/proc/net/snmp 的 Tcp 行字段逐个解释（源码：net/ipv4/tcp_ipv4.c 的 tcp4_proc_show 系列
以及 net/ipv4/proc.c: tcp4_seq_show 使用的 snmp4_tcp_list）：
  RtoAlgorithm  1 = RFC 793 恒定 RTO 算法（Linux 固定为 1）
  RtoMin        最小 RTO，毫秒。来自 net.ipv4.tcp_rto_min（内核内部 200ms 起步）
  RtoMax        最大 RTO，毫秒。来自 net.ipv4.tcp_rto_max（默认 120000）
  MaxConn       -1 表示不限制，来自 net.ipv4.ip_local_port_range 的能力上限
  ActiveOpens   主动发起连接次数（connect() 触发，tcp_v4_connect 成功后计数）
  PassiveOpens  被动接受连接次数（收到 SYN 并最终建立，tcp_v4_syn_recv_sock 路径）
  AttemptFails  connect 失败 + 同时打开失败的次数（如 SYN 重传耗尽）
  EstabResets   从 ESTABLISHED/CLOSE_WAIT 直接进入 CLOSED 的次数（多为收到 RST）
  CurrEstab     当前 ESTABLISHED 连接数（快照值，等于同一瞬间 /proc/net/tcp 中 01 的条数）
  InSegs        收到的 TCP 报文（含错误）总数
  OutSegs       发出的 TCP 报文总数（含重传）
  RetransSegs   重传报文数。RetransSegs/OutSegs 就是重传率
  InErrs        收到的错误 TCP 报文数
  OutRsts       发出的 RST 报文数
  InCsumErrors  校验和错误的 TCP 报文数

/proc/net/netstat 的 TcpExt 行则更细，见 parse_proc_tcp.py --netstat KEY 的输出。
EOF
}

echo "###### TCP/UDP 内核计数观测 ######"
echo "date   : $(date -Is)"
echo "kernel : $(uname -r)"
echo "uid    : $(id -u)（非 root：/proc/net/* 可读，sysctl 只读）"
echo

snapshot
echo
explain_snmp
echo

echo "== TcpExt 关键计数（带含义说明，脚本自动抓值）=="
python3 "$PY" --netstat TW TWRecycled TWKilled ListenOverflows ListenDrops TCPTimeouts \
    TCPFastRetrans TCPSackRecovery TCPLossProbes DelayedACKs TCPOFOQueue TCPBacklogDrop \
    SyncookiesSent TCPAbortOnClose TCPAbortOnLinger
echo

echo "== UDP 计数解释（/proc/net/snmp 的 Udp 行）=="
cat <<'EOF'
  InDatagrams   正常投递给应用的 UDP 数据报数
  NoPorts       收到的目标端口无监听者（回 ICMP port unreachable 并累加）
  InErrors      投递失败（如 sk_receive_queue 满、内存不足）
  OutDatagrams  发出的 UDP 数据报数
  RcvbufErrors  因接收缓冲区满而丢弃的数据报数（对应 sk_rmem_alloc 超限）
  SndbufErrors  因发送缓冲区满而丢包（udp_send_skb 返回 ENOMEM/EAGAIN 时累加）
  InCsumErrors  校验和错误
  IgnoredMulti  被忽略的多播数据报
  MemErrors     内存分配失败导致丢弃的数据报数
EOF
echo

echo "== 需要 root 才能做的对照实验（本机不可运行，仅给出做法）=="
cat <<'EOF'
  # 1) 调整全连接队列上限（会同时影响所有 LISTEN socket）
  sysctl -w net.core.somaxconn=128          # 或: echo 128 > /proc/sys/net/core/somaxconn
  # 2) 调整 SYN 队列上限与溢出策略
  sysctl -w net.ipv4.tcp_max_syn_backlog=256
  sysctl -w net.ipv4.tcp_abort_on_overflow=1   # 溢出时回 RST 而不是静默丢弃 ACK
  # 3) 观察 TIME_WAIT 回收策略
  sysctl -w net.ipv4.tcp_tw_reuse=1            # 本机已是 2（0=禁止,1=对客户端连接,2=对所有连接）
  sysctl -w net.ipv4.tcp_max_tw_buckets=1000   # 超过后立即销毁 TIME_WAIT 并累加 TWKilled
  # 4) 触发 SYN 队列溢出以观察 syncookie
  sysctl -w net.ipv4.tcp_syncookies=1          # 本机已是 1
  以上写操作本机返回 "Permission denied"（CapEff=0，无 CAP_NET_ADMIN），
  但 /proc/sys/net/... 的读操作全部可用。
EOF
