#!/usr/bin/env bash
# =============================================================================
# 04-snmp-counters.sh —— 解析 /proc/net/snmp 与 /proc/net/netstat 的关键计数器
#
# 对应文档: docs/09-内核网络观测与性能调优.md 第 2 节 / 第 6 节 case 4-7
#
# 这两个文件的格式很有特色，也最容易读错:
#   /proc/net/snmp     奇数行 = 字段名，偶数行 = 对应值，按协议前缀成对出现
#   /proc/net/netstat  同样的『名字行 + 值行』成对结构，但按扩展组出现
#                      本机实测有 3 组：TcpExt / IpExt / MPTcpExt
#
# 为什么不用 sysctl:
#   本机直接敲 sysctl 会报「未找到命令」，但那只是 /usr/sbin 不在 PATH 里
#   （procps 确实装了 /usr/sbin/sysctl，用绝对路径就能用）。
#   这里统一用 cat /proc/... 是为了不依赖 PATH、不依赖工具包。
#   这些计数器本来也不归 sysctl 管；若要读 net.ipv4.* 参数，cat /proc/sys/... 等价于 sysctl -n。
#
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

SNMP=/proc/net/snmp
NETSTAT=/proc/net/netstat

echo "==================================================================="
echo " SNMP / TcpExt 计数器解析 —— 内核: $(uname -r)"
echo " 时间: $(date '+%F %T %Z')"
echo "==================================================================="
echo
echo "⚠️ 这些全是**累计计数器**（monotonic counter），单次读数意义有限。"
echo "   正确用法是间隔采样两次做差，看『增量』。本脚本先给出单次快照，"
echo "   并计算几个有意义的比值。"

# ---------------------------------------------------------------------------
# 通用解析函数：从『名字行 + 值行』结构里取出指定字段
#   $1 = 文件路径
#   $2 = 前缀（不含冒号），如 Tcp / TcpExt / Udp
#   $3.. = 想要查询的字段名
# ---------------------------------------------------------------------------
show_counters() {
    local file="$1" prefix="$2"; shift 2
    local want="$*"
    awk -v want="$want" -v pfx="$prefix" '
        BEGIN { n = split(want, W, " ") }
        # 只处理形如 "Tcp: ..." 的行
        substr($0, 1, length(pfx) + 1) == pfx ":" {
            line = substr($0, length(pfx) + 2)      # 去掉前缀和冒号
            if (!(pfx in seen)) {                   # 第一次出现 = 字段名行
                seen[pfx] = 1
                nf = split(line, H, " ")
                next
            }
            nv = split(line, V, " ")                # 第二次出现 = 值行
            for (i = 1; i <= nf && i <= nv; i++) val[H[i]] = V[i]
        }
        END {
            for (i = 1; i <= n; i++) {
                if (W[i] in val) printf "  %-30s %s\n", W[i], val[W[i]]
                else             printf "  %-30s %s\n", W[i], "(本机不存在该计数器)"
            }
        }
    ' "$file"
}

# ---------------------------------------------------------------------------
# 1. TCP 基础 MIB（/proc/net/snmp 的 Tcp: 组）
# ---------------------------------------------------------------------------
sec "① TCP 基础计数器（/proc/net/snmp 的 Tcp: 组）"
if [[ -r $SNMP ]]; then
    show_counters "$SNMP" Tcp \
        ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab \
        InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors
else
    echo "SKIP: $SNMP 不可读"
fi
note "怎么读："
note "  ActiveOpens  >> PassiveOpens  —— 本机主要作为客户端在发起连接"
note "  PassiveOpens >> ActiveOpens   —— 本机主要作为服务端在被连接"
note "  AttemptFails                  —— 建连失败；配合 TcpExt 的 ListenOverflows 判断"
note "                                   是否是 accept 队列溢出导致的"
note "  RetransSegs / OutSegs         —— 重传率，是网络质量最直接的指标（下面会算）"
note "  InCsumErrors > 0              —— 收到的包校验和错误，优先怀疑网卡 offload 或线路"
note "  CurrEstab                     —— 当前 ESTABLISHED 连接数（瞬时值，不是累计）"

# ---------------------------------------------------------------------------
# 2. UDP 基础 MIB
# ---------------------------------------------------------------------------
sec "② UDP 基础计数器（/proc/net/snmp 的 Udp: 组）"
if [[ -r $SNMP ]]; then
    show_counters "$SNMP" Udp \
        InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors
else
    echo "SKIP: $SNMP 不可读"
fi
note "  RcvbufErrors / SndbufErrors —— UDP 的经典丢包点。UDP 无重传，缓冲区满就丢，"
note "  并且**不会**通知应用。这是 UDP 业务『静默丢包』的头号原因。"
note "  NoPorts —— 收到目标端口没人在听的 UDP 包（内核回 ICMP port unreachable）。"

# ---------------------------------------------------------------------------
# 3. TcpExt 扩展计数器（丢包排查的主战场）
# ---------------------------------------------------------------------------
sec "③ TcpExt 扩展计数器（/proc/net/netstat 的 TcpExt: 组）"
if [[ -r $NETSTAT ]]; then
    echo "  ── 连接建立/队列相关 ──"
    show_counters "$NETSTAT" TcpExt ListenOverflows ListenDrops TCPReqQFullDrop TCPReqQFullDoCookies
    echo
    echo "  ── 重传/超时相关 ──"
    show_counters "$NETSTAT" TcpExt TCPTimeouts TCPLostRetransmit TCPTimeWaitOverflow
    echo
    echo "  ── 缓冲区/内存压力相关 ──"
    show_counters "$NETSTAT" TcpExt TCPBacklogDrop TCPRcvQDrop TCPAbortOnMemory TCPMemoryPressures
    echo
    echo "  ── 乱序/快速路径相关 ──"
    show_counters "$NETSTAT" TcpExt TCPOFOQueue TCPHPHits TCPPureAcks TCPHPAcks DelayedACKs TW TWRecycled
else
    echo "SKIP: $NETSTAT 不可读"
fi

cat <<'EOF'

  ── 每个计数器对应的真实故障场景（这是本节最该记住的部分）──

  ListenOverflows     accept 队列（全连接队列）溢出：三次握手已完成，但应用
                      迟迟不调用 accept()，队列满了之后内核丢弃新建的连接。
                      与 net.core.somaxconn 直接相关。
  ListenDrops         因 listen 队列满而丢弃的 SYN，服务端表现为客户端连不上。
  TCPReqQFullDrop     半连接队列（SYN queue）满且 tcp_syncookies=0 时丢弃 SYN。
  TCPReqQFullDoCookies 半连接队列满但启用了 syncookies，内核用 cookie 顶住没丢。
                      看到这个在涨说明队列已经压力很大了，只是被 syncookie 救了。
  TCPTimeouts         RTO（重传超时）触发的次数。TCP 认为包丢了在重传。
  TCPLostRetransmit   重传出去的包**又**丢了。这是网络严重拥塞/丢包的强信号。
  TCPTimeWaitOverflow TIME-WAIT 表溢出，新连接无法进入 TIME-WAIT。
  TCPBacklogDrop      收包 backlog 满导致丢包（和 softnet_stat 第 2 列同源）。
  TCPRcvQDrop         数据已进入 socket 接收队列但队列满而丢包，
                      **典型症状是 ss 里该连接 Recv-Q 持续很高** —— 应用读得太慢。
  TCPAbortOnMemory    内核因内存压力主动 abort 连接，通常是 socket 缓冲区占用过多。
  TCPOFOQueue         乱序（Out-Of-First）包进入乱序队列的次数。
                      轻微乱序属正常；持续大量增长说明网络路径有多路径/抖动。
  TCPMemoryPressures  进入 TCP 内存压力的次数，和 tcp_mem 三个阈值相关。
EOF

# ---------------------------------------------------------------------------
# 4. 派生指标：重传率（比绝对值更有信息量）
# ---------------------------------------------------------------------------
sec "④ 派生指标：重传率"
if [[ -r $SNMP ]]; then
    awk -v want="OutSegs RetransSegs InSegs InErrs" '
        BEGIN { n = split(want, W, " ") }
        substr($0, 1, 4) == "Tcp:" {
            line = substr($0, 6)
            if (!seen) { seen = 1; nf = split(line, H, " "); next }
            nv = split(line, V, " ")
            for (i = 1; i <= nf && i <= nv; i++) val[H[i]] = V[i]
        }
        END {
            out = val["OutSegs"] + 0
            rtx = val["RetransSegs"] + 0
            ins = val["InSegs"] + 0
            err = val["InErrs"] + 0
            printf "  发出报文段总数 OutSegs      = %d\n", out
            printf "  重传报文段数   RetransSegs  = %d\n", rtx
            if (out > 0)
                printf "  重传率 RetransSegs/OutSegs  = %.4f%%\n", rtx * 100.0 / out
            else
                printf "  重传率                       = 不可计算（OutSegs=0）\n"
            if (ins > 0)
                printf "  入向错误率 InErrs/InSegs    = %.6f%%\n", err * 100.0 / ins
            printf "\n"
            if (out > 0 && rtx * 100.0 / out > 1.0)
                print "  ⚠️  重传率 > 1%，网络路径很可能存在真实丢包，建议继续看 TCPTimeouts / TCPLostRetransmit"
            else if (out > 0)
                print "  ✅ 重传率 < 1%，属于正常范围（公网通常在 0.1%~1% 之间波动）"
        }
    ' "$SNMP"
else
    echo "SKIP: $SNMP 不可读"
fi
note "注意：OutSegs 包含重传段本身，所以这个比值是『重传段占发送总量的比例』，"
note "是一个工程上够用的近似，和 RFC 里的定义不完全等价。"

# ---------------------------------------------------------------------------
# 5. 内存与队列的当前水位
# ---------------------------------------------------------------------------
sec "⑤ 相关内存水位（/proc/net/sockstat 与 /proc/net/protocols）"
if [[ -r /proc/net/sockstat ]]; then
    cat /proc/net/sockstat || true
fi
echo
note "TCP: mem 的单位是**页**（本机页大小 $(getconf PAGESIZE) 字节），要乘出来才是字节。"
note "tw = TIME-WAIT 数量；orphan = 应用已关闭但内核还在用的 socket 数。"

echo
echo "==================================================================="
echo " 本实验对应的内核源码入口（以 6.12 为准，可在"
echo " elixir.bootlin.com/linux/v6.12/source/ 核对）："
echo "   net/ipv4/proc.c            —— /proc/net/snmp 与 netstat 的字段名表"
echo "                                 (snmp4_ipstats_list / tcp4_seq_show 等)"
echo "   include/linux/snmp.h       —— LINUX_MIB_* 与 LINUX_MIB_TCPEXT_* 枚举"
echo "   include/net/tcp.h          —— TCP_MIB_* / TCP_MIB_MAX 定义"
echo "   net/ipv4/tcp_input.c       —— 各计数器 ++ 的发生位置"
echo "==================================================================="
