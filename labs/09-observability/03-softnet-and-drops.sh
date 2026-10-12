#!/usr/bin/env bash
# =============================================================================
# 03-softnet-and-drops.sh —— 解析 softnet_stat 每一列 + 网卡层丢包计数
#
# 对应文档: docs/09-内核网络观测与性能调优.md 第 3 节 / 第 6 节 case 1-2
#
# 这个脚本解决一个非常具体的问题:
#   "包到底丢在哪一层？" —— 网卡 ring buffer？softnet backlog？socket 队列？
#   这三个位置分别对应三组计数器，本脚本把前两组拉出来对比。
#
# 权限: 不需要 root。
# =============================================================================

set -euo pipefail

# 尊重 NO_COLOR 约定（见 https://no-color.org/）
if [[ -n "${NO_COLOR:-}" ]]; then
    C_TITLE=""; C_NOTE=""; C_WARN=""; C_RESET=""
else
    C_TITLE=$'\033[1;36m'; C_NOTE=$'\033[0;33m'
    C_WARN=$'\033[0;31m'; C_RESET=$'\033[0m'
fi

sec()  { printf '\n%s===== %s =====%s\n' "$C_TITLE" "$*" "$C_RESET"; }
note() { printf '%s[解读]%s %s\n' "$C_NOTE" "$C_RESET" "$*"; }
warn() { printf '%s[注意]%s %s\n' "$C_WARN" "$C_RESET" "$*"; }

SOFTNET=/proc/net/softnet_stat
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)

echo "==================================================================="
echo " softnet_stat / 网卡丢包 解析 —— 内核: $(uname -r)"
echo " 在线 CPU: $NCPU  时间: $(date '+%F %T %Z')"
echo "==================================================================="

if [[ ! -r $SOFTNET ]]; then
    echo "SKIP: $SOFTNET 不可读，本机无法运行该实验。"
    exit 0
fi

# ---------------------------------------------------------------------------
# 1. 先确认列数：列数会随内核版本变化，不能写死
# ---------------------------------------------------------------------------
NCOL=$(awk 'NR==1{print NF}' "$SOFTNET")
NLINE=$(wc -l < "$SOFTNET")

sec "① 文件形态自检"
echo "  每行字段数（列数）: $NCOL"
echo "  行数（应等于可能的 CPU 数）: $NLINE"
echo "  真机在线 CPU 数    : $NCPU"

cat <<'EOF'

  格式说明（重要）：
    每一行对应一个 CPU，每一列都是宽度 8 的**十六进制**（内核用 %08x 输出）。
    所以 "000a5e9f" 要读成 0x000a5e9f = 679583，而不是十进制的一千万。
    行数通常等于内核配置的 possible CPU 数，可能大于在线 CPU 数。

EOF

if [[ "$NCOL" -ne 15 ]]; then
    warn "本机列数为 $NCOL，而作者在 6.12.107 上实测为 15 列。"
    warn "不同内核版本列数不同，下面的列名标注可能与你看到的错位！"
    warn "请以本机内核源码 net/core/net-procfs.c 的 softnet_seq_show() 为准。"
fi

# ---------------------------------------------------------------------------
# 2. 逐列释义表
# ---------------------------------------------------------------------------
sec "② 列含义对照表（本机 $NCOL 列）"
cat <<'EOF'
  ⚠️ 关于准确性的说明：
     作者在 6.12.107 上做了两件事来确认这张表，而不是凭记忆：
       (a) 从运行中的内核镜像里把 softnet_seq_show() 的格式串抠出来，
           数出确实是 15 个 %08x（复现方法见 README 的「如何验证」一节）；
       (b) 对照 /proc/net/softnet_stat 的实际行宽确认 15 列。
     但**第 4 列及之后的具体语义在历史上变动过**（早期的 fastroute、
     cpu_collision 被移除后保留为恒 0 的占位列），作者无法在本机完全确认
     每一列的现代语义。因此下面用 [已确认] / [待核对] 明确区分。

  列号  名称                 置信度      说明
  ----  -------------------  ----------  --------------------------------------
   1    processed            [已确认]    该 CPU 从队列中取走并处理的报文总数
   2    dropped              [已确认]    因 backlog 队列已满而丢弃的报文数 ★关键
   3    time_squeeze         [已确认]    软中断一次 poll 用完预算(netdev_budget)
                                        仍没处理完、被迫让出 CPU 的次数 ★关键
   4~8  reserved             [待核对]    历史上是 fastroute 相关计数，现恒为 0
   9    reserved             [待核对]    历史上是 cpu_collision，现恒为 0
  10    received_rps         [待核对]    RPS(Receive Packet Steering) 触发的次数
  11    flow_limit_count     [待核对]    RFS flow limit 触发次数
  12~15 reserved             [待核对]    保留位

  ★ 排查丢包时只需要盯住 2 列：第 2 列（dropped）和第 3 列（time_squeeze）。
  ★ 其余列在当前内核上基本恒为 0；如果你看到它们非 0，说明你的内核
    版本与 6.12 不同，必须去读该版本 net/core/net-procfs.c 的格式串。

  权威核对地址（以 6.12 为准）：
    https://elixir.bootlin.com/linux/v6.12/source/net/core/net-procfs.c
    函数 softnet_seq_show()  —— 里面只有一条 seq_printf，参数顺序即列顺序
EOF

# ---------------------------------------------------------------------------
# 3. 把 softnet_stat 渲染成对齐的十进制表格
# ---------------------------------------------------------------------------
sec "③ softnet_stat 逐 CPU 渲染（十六进制 -> 十进制）"

printf '  %-4s %12s %10s %13s %12s %10s %16s\n' \
       "CPU" "processed" "dropped" "time_squeeze" "recv_rps*" "flow_lim*" "col12+*"
printf '  %-4s %12s %10s %13s %12s %10s %16s\n' \
       "----" "------------" "----------" "-------------" "------------" "----------" "----------------"

# 用 awk 一次性完成 hex->dec 和格式对齐。
# awk 的 strtonum() 是 gawk 扩展；为了避免依赖 gawk，这里用 "0x" 前缀
# 交给 awk 的隐式数值转换（POSIX awk 对 0x 前缀支持不一致），
# 因此改用 printf "%d" 的十六进制解析：借助 bash 的 $((16#..)) 更稳妥，
# 这里直接在 awk 里用自定义函数解析，保证可移植。
awk '
  function hex2dec(h,   i,c,n,v) {
      n = 0
      h = tolower(h)
      for (i = 1; i <= length(h); i++) {
          c = substr(h, i, 1)
          v = index("0123456789abcdef", c) - 1
          if (v < 0) return -1
          n = n * 16 + v
      }
      return n
  }
  {
      printf "  %-4d %12d %10d %13d %12d %10d %16d\n", \
             NR-1, hex2dec($1), hex2dec($2), hex2dec($3), hex2dec($10), hex2dec($11), hex2dec($12)
      tp += hex2dec($1); td += hex2dec($2); ts += hex2dec($3)
  }
  END {
      printf "  %-4s %12d %10d %13d\n", "合计", tp, td, ts
  }
' "$SOFTNET"
note "* 标记的列语义待核对（见上表），本机通常恒为 0。"

# ---------------------------------------------------------------------------
# 4. 结论判读
# ---------------------------------------------------------------------------
sec "④ 判读"
awk '
  function hex2dec(h,   i,c,n,v) {
      n = 0; h = tolower(h)
      for (i = 1; i <= length(h); i++) {
          c = substr(h, i, 1); v = index("0123456789abcdef", c) - 1
          if (v < 0) return -1
          n = n * 16 + v
      }
      return n
  }
  { d = hex2dec($2); s = hex2dec($3); if (d > 0) dn++; if (s > 0) sn++; t += hex2dec($1) }
  END {
      if (dn > 0) printf "  ⚠️  有 %d 个 CPU 出现过 backlog 丢包（第 2 列 > 0）\n", dn
      else        print  "  ✅ 没有任何 CPU 出现 softnet backlog 丢包（第 2 列全为 0）"
      if (sn > 0) printf "  ⚠️  有 %d 个 CPU 出现过 time_squeeze（预算耗尽）\n", sn
      else        print  "  ✅ 没有 CPU 报告 time_squeeze（收包压力未触及预算上限）"
      printf "  ℹ️  所有 CPU 累计处理报文 %d 个\n", t
  }
' "$SOFTNET"

cat <<'EOF'

  判读规则（记住这三条就够）：
    · 第 2 列 dropped > 0 且持续增长
        -> 内核软中断来不及消费驱动队列，backlog 队列(netdev_max_backlog)满。
           解决方向：调大 net.core.netdev_max_backlog；开启多队列/RPS 分摊；
           或提升单核处理能力（根本解法）。
    · 第 3 列 time_squeeze > 0 且增长
        -> 一次 NAPI poll 没跑完预算就被迫返回，通常是单 CPU 收包压力过大。
           解决方向：调大 net.core.netdev_budget / netdev_budget_usecs（治标），
           或做 RPS/RFS、多队列、XDP 分流（治本）。
    · 两列都是 0 但仍在丢包
        -> 丢包不在这一层！往上看 socket 队列（ss 的 Recv-Q）和
           /proc/net/netstat 的 TcpExt 计数器，或往下看网卡 ring buffer。
EOF

# ---------------------------------------------------------------------------
# 5. 网卡层统计：ring buffer 溢出与错误
# ---------------------------------------------------------------------------
sec "⑤ 网卡层统计（/sys/class/net/*/statistics/）"

printf '  %-16s %10s %10s %10s %14s %12s\n' \
       "接口" "rx_dropped" "tx_dropped" "rx_errors" "rx_missed_err" "rx_bytes"
printf '  %-16s %10s %10s %10s %14s %12s\n' \
       "----------------" "----------" "----------" "----------" "--------------" "------------"

shopt -s nullglob
for dev in /sys/class/net/*; do
    name=$(basename "$dev")
    rd=$(cat "$dev/statistics/rx_dropped"        2>/dev/null || echo 0)
    td=$(cat "$dev/statistics/tx_dropped"        2>/dev/null || echo 0)
    re=$(cat "$dev/statistics/rx_errors"         2>/dev/null || echo 0)
    # rx_missed_errors 只有部分驱动上报，缺失时显示 n/a
    if [[ -r "$dev/statistics/rx_missed_errors" ]]; then
        rm_=$(cat "$dev/statistics/rx_missed_errors")
    else
        rm_="n/a"
    fi
    rb=$(cat "$dev/statistics/rx_bytes"          2>/dev/null || echo 0)
    printf '  %-16s %10s %10s %10s %14s %12s\n' "$name" "$rd" "$td" "$re" "$rm_" "$rb"
done
shopt -u nullglob

note "这几个计数器的定位："
note "  rx_dropped        —— 内核在把包交给协议栈**之前**丢弃的（含 ring 满、无内存）"
note "  rx_missed_errors  —— 驱动报告『我看见了包但没来得及收』，**ring buffer 溢出的经典信号**"
note "                       注意：只有部分网卡驱动会填这个计数器，n/a 是正常的"
note "  rx_errors         —— 物理层/校验错误（网线、光模块、双工不匹配）"
note "  tx_dropped        —— 发送方向丢包（qdisc 队列满也计入这里）"

# ---------------------------------------------------------------------------
# 6. /proc/net/dev 的原始计数（与上面同源，换一个视角）
# ---------------------------------------------------------------------------
sec "⑥ /proc/net/dev 原始读数（列宽紧凑，注意列对齐）"
cat /proc/net/dev || true
note "第 1-2 行是表头说明；第 3 行起每行是 '接口名:' 开头。"
note "字段顺序: 接收 bytes packets errs drop fifo frame compressed multicast"
note "          发送 bytes packets errs drop fifo colls carrier compressed"
note "这里没有 rx_missed_errors，所以查 ring buffer 溢出要用上面的"
note "/sys/class/net/<iface>/statistics/rx_missed_errors，或 ethtool -S（需 root）。"

# ---------------------------------------------------------------------------
# 7. 三层丢包位置的对照总结
# ---------------------------------------------------------------------------
sec "⑦ 丢包定位三层对照"
cat <<'EOF'
  观测位置                                计数器                          本机读数
  --------------------------------------  ------------------------------  -----------
  ① 网卡 ring buffer                      rx_missed_errors / rx_dropped   见 ⑤
  ② 内核 softnet backlog                  softnet_stat 第 2 列 dropped    见 ③
  ③ 内核 → 应用（socket 接收队列）        TcpExt TCPRcvQDrop / ss Recv-Q  见 04 脚本

  排查顺序应当是**从下往上**：网卡 -> backlog -> socket -> 应用。
  因为下游的丢包往往会体现为上游的『看起来正常但吞吐上不去』。
EOF

echo
echo "==================================================================="
echo " 本实验对应的内核源码入口（以 6.12 为准，可在"
echo " elixir.bootlin.com/linux/v6.12/source/ 核对）："
echo "   net/core/net-procfs.c   softnet_seq_show()  —— softnet_stat 的格式"
echo "   include/linux/netdevice.h  struct softnet_data —— 各列的数据来源"
echo "   net/core/dev.c          enqueue_to_backlog() —— backlog 丢包发生地"
echo "   net/core/dev.c          net_rx_action()      —— 预算与 time_squeeze"
echo "==================================================================="
