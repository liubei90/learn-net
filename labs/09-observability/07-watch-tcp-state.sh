#!/usr/bin/env bash
# =============================================================================
# 07-watch-tcp-state.sh —— 轮询 /proc/net/tcp，观察 TCP 状态的随时间变化
#
# 对应文档: docs/09-内核网络观测与性能调优.md 第 3 节（连接生命周期）
#
# 为什么有用:
#   单次 `ss` 只能看到一个瞬间。很多问题（连接泄漏、短连接风暴、accept 慢）
#   只有在**时间维度**上才看得出来：某个状态的数量是不是在单调增长？
#   本脚本用最朴素的轮询 + 差分，画出一个小小的状态时间序列表。
#
# 局限（很重要，必须知道）:
#   /proc/net/tcp 是**轮询快照**，不是事件流。两次采样之间发生的
#   『建立又关闭』的短连接会被完全漏掉。所以：
#     · 状态数量持续增长 -> 可靠结论（泄漏）
#     · 状态数量看起来正常 -> **不能**据此断定没有短连接风暴，
#       需要用 tcp:inet_sock_set_state 这类 tracepoint 做事件级观测
#       （见 06-tracepoint-templates.md，本机因无权限/无工具不可运行）
#
# 权限: 不需要 root。
# 用法: ./07-watch-tcp-state.sh [采样次数] [采样间隔秒]
# =============================================================================

set -euo pipefail

SECS=${1:-6}      # 采样次数
INTERVAL=${2:-2}  # 间隔秒
TCP=/proc/net/tcp
TCP6=/proc/net/tcp6

# 尊重 NO_COLOR 约定（见 https://no-color.org/）
if [[ -n "${NO_COLOR:-}" ]]; then
    C_TITLE=""; C_NOTE=""; C_RESET=""
else
    C_TITLE=$'\033[1;36m'; C_NOTE=$'\033[0;33m'; C_RESET=$'\033[0m'
fi

sec()  { printf '\n%s===== %s =====%s\n' "$C_TITLE" "$*" "$C_RESET"; }
note() { printf '%s[解读]%s %s\n' "$C_NOTE" "$C_RESET" "$*"; }

# /proc/net/tcp 的 st 列是十六进制状态码。
# 映射关系来自 include/net/tcp_states.h 的 enum（TCP_ESTABLISHED=1, ...）。
state_name() {
    case "$1" in
        01) echo "ESTABLISHED" ;;
        02) echo "SYN_SENT" ;;
        03) echo "SYN_RECV" ;;
        04) echo "FIN_WAIT1" ;;
        05) echo "FIN_WAIT2" ;;
        06) echo "TIME_WAIT" ;;
        07) echo "CLOSE" ;;
        08) echo "CLOSE_WAIT" ;;
        09) echo "LAST_ACK" ;;
        0A|0a) echo "LISTEN" ;;
        0B|0b) echo "CLOSING" ;;
        0C|0c) echo "NEW_SYN_RECV" ;;
        *) echo "UNKNOWN($1)" ;;
    esac
}

if [[ ! -r $TCP ]]; then
    echo "SKIP: $TCP 不可读（需要 CONFIG_PROC_FS 且未被 hidepid 之类机制屏蔽）。"
    exit 0
fi

# 采样次数必须是正整数，否则 awk 循环会失控
case "$SECS" in
    ''|*[!0-9]*) echo "用法: $0 [采样次数>=1] [间隔秒>=1]"; exit 2 ;;
esac
if [[ "$SECS" -lt 1 ]]; then echo "采样次数必须 >= 1"; exit 2; fi

echo "==================================================================="
echo " TCP 状态时间序列 —— 内核: $(uname -r)"
echo " 采样 $SECS 次，间隔 ${INTERVAL}s，数据源: $TCP $TCP6"
echo "==================================================================="

# 所有可能出现的状态（顺序固定，方便表格对齐）
STATES=(ESTABLISHED SYN_SENT SYN_RECV FIN_WAIT1 FIN_WAIT2 TIME_WAIT CLOSE CLOSE_WAIT LAST_ACK LISTEN CLOSING NEW_SYN_RECV)

# 用关联数组累积每个采样点的计数
declare -A SNAP=()

# 采集一个采样点：统计 /proc/net/tcp 与 /proc/net/tcp6 中每个状态的条目数
# 输出格式: "STATE COUNT" 每行一个
sample_states() {
    {
        # 跳过表头（第 1 行），取第 4 列 st（状态码）
        awk 'NR > 1 { print $4 }' "$TCP"  2>/dev/null || true
        [[ -r $TCP6 ]] && { awk 'NR > 1 { print $4 }' "$TCP6" 2>/dev/null || true; }
    } | while read -r hexstate; do
            state_name "$hexstate"
        done | sort | uniq -c | awk '{print $2, $1}'
}

i=1
while [[ $i -le $SECS ]]; do
    ts=$(date '+%H:%M:%S')
    # 读取该时刻的状态分布
    while read -r name count; do
        SNAP["$i:$name"]=$count
    done < <(sample_states)

    total=0
    for s in "${STATES[@]}"; do
        total=$(( total + ${SNAP["$i:$s"]:-0} ))
    done
    SNAP["$i:TOTAL"]=$total

    printf '  [%d/%d] %s 采样完成，共 %d 条 TCP socket 记录\n' "$i" "$SECS" "$ts" "$total"
    if [[ $i -lt $SECS ]]; then
        sleep "$INTERVAL"
    fi
    i=$(( i + 1 ))
done

# ---------------------------------------------------------------------------
# 渲染时间序列表
# ---------------------------------------------------------------------------
sec "① 时间序列表（每行一个状态，每列一个采样点）"

# 表头
printf '  %-14s' "STATE \\ 采样点"
for (( j = 1; j <= SECS; j++ )); do
    printf ' %8s' "#$j"
done
printf ' %10s\n' "变化"

printf '  %-14s' "--------------"
for (( j = 1; j <= SECS; j++ )); do
    printf ' %8s' "--------"
done
printf ' %10s\n' "----------"

for s in "${STATES[@]}" TOTAL; do
    # 如果这个状态在所有采样点都是 0 且不是 TOTAL，就跳过，避免表格过长
    if [[ "$s" != "TOTAL" ]]; then
        any=0
        for (( j = 1; j <= SECS; j++ )); do
            [[ ${SNAP["$j:$s"]:-0} -ne 0 ]] && any=1
        done
        [[ $any -eq 0 ]] && continue
    fi

    printf '  %-14s' "$s"
    for (( j = 1; j <= SECS; j++ )); do
        printf ' %8d' "${SNAP["$j:$s"]:-0}"
    done
    first=${SNAP["1:$s"]:-0}
    last=${SNAP["$SECS:$s"]:-0}
    delta=$(( last - first ))
    if [[ $delta -gt 0 ]]; then
        printf ' %10s\n' "+$delta"
    elif [[ $delta -lt 0 ]]; then
        printf ' %10s\n' "$delta"
    else
        printf ' %10s\n' "0"
    fi
done

# ---------------------------------------------------------------------------
# 自动判读
# ---------------------------------------------------------------------------
sec "② 自动判读"

first_total=${SNAP["1:TOTAL"]:-0}
last_total=${SNAP["$SECS:TOTAL"]:-0}
printf '  起始总 socket 记录数: %d\n' "$first_total"
printf '  结束总 socket 记录数: %d\n' "$last_total"

# CLOSE_WAIT 单调增长是最经典的『应用忘了 close()』bug
cw_first=${SNAP["1:CLOSE_WAIT"]:-0}
cw_last=${SNAP["$SECS:CLOSE_WAIT"]:-0}
if [[ $cw_last -gt $cw_first ]]; then
    printf '  ⚠️  CLOSE_WAIT 从 %d 增长到 %d\n' "$cw_first" "$cw_last"
    printf '      CLOSE_WAIT 增长几乎总是应用层 bug：对端已关闭连接，\n'
    printf '      本端应用没有调用 close()，socket 就一直挂着。\n'
else
    printf '  ✅ CLOSE_WAIT 没有增长（%d -> %d），未发现明显的连接未关闭泄漏\n' "$cw_first" "$cw_last"
fi

tw_first=${SNAP["1:TIME_WAIT"]:-0}
tw_last=${SNAP["$SECS:TIME_WAIT"]:-0}
if [[ $tw_last -gt $tw_first ]]; then
    printf '  ℹ️  TIME_WAIT 从 %d 增长到 %d（主动关闭方会留下 TIME_WAIT，属正常现象）\n' "$tw_first" "$tw_last"
    printf '      只有当数量达到数万、并且客户端报 "Cannot assign requested address" 时\n'
    printf '      才是问题，排查见 docs/09 第 6 节 case 5。\n'
fi

if [[ $last_total -gt $first_total ]]; then
    printf '  ⚠️  总记录数在增长（+%d）。持续单调增长通常意味着连接泄漏。\n' "$(( last_total - first_total ))"
    printf '      注意：也可能是这台机器正常在接新连接，需结合业务判断。\n'
elif [[ $last_total -eq $first_total ]]; then
    printf '  ✅ 总记录数稳定（%d），采样窗口内没有明显的连接堆积\n' "$last_total"
else
    printf '  ℹ️  总记录数下降（%d）。多为短连接自然回收，属正常。\n' "$(( last_total - first_total ))"
fi

sec "③ 关于采样快照的固有局限"
cat <<'EOF'
  /proc/net/tcp 是**轮询快照**，不是事件流。这意味着：
    · 两次采样之间『建立又立刻关闭』的短连接，会被完全漏掉；
    · 因此本表适合发现"堆积/泄漏"类问题，
      不适合发现"高频短连接"类问题（即使 QPS 上万，瞬时存量也可能很小）。
  要做事件级观测，必须用 tracepoint，例如：
      sock:inet_sock_set_state     —— 每次 TCP 状态迁移都触发一次
      tcp:tcp_retransmit_skb       —— 每次重传触发
  模板与前置条件见 06-tracepoint-templates.md。
  ⚠️ 本机（CapEff=0、无 tracefs 读权限、未安装 bpftrace/perf）无法运行这些
     tracepoint 工具，脚本会优雅降级为 SKIP。
EOF

echo
echo "==================================================================="
echo " 本实验对应的内核源码入口（以 6.12 为准，可在"
echo " elixir.bootlin.com/linux/v6.12/source/ 核对）："
echo "   include/net/tcp_states.h   —— TCP_* 状态枚举（本脚本状态码的依据）"
echo "   net/ipv4/tcp_ipv4.c        tcp4_seq_show() —— /proc/net/tcp 的渲染"
echo "   include/net/inet_sock.h    inet_sk_state_store() —— 更新 sk->sk_state 的入口"
echo "                              （sock:inet_sock_set_state 这个 tracepoint 就在"
echo "                                这个函数里触发；tcp_set_state() 调用它）"
echo "   net/ipv4/af_inet.c         inet_sk_state_store() 的实现"
echo "==================================================================="
