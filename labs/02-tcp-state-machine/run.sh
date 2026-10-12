#!/usr/bin/env bash
# labs/02-tcp-state-machine/run.sh
# 全部实跑：backlog 溢出、TIME_WAIT、/proc 解析、内核计数观测。
# 非 root 可运行；无端口冲突（监听端口由内核临时分配）；所有后台进程由 trap 清理。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
mkdir -p .out

TIMEOUT_BIN="$(command -v timeout || true)"
rc=0

log()  { printf '\n===== %s =====\n' "$*"; }
note() { printf '[run.sh] %s\n' "$*"; }
run_to() { local s="$1"; shift; if [[ -n "$TIMEOUT_BIN" ]]; then "$TIMEOUT_BIN" "$s" "$@"; else "$@"; fi; }

for b in backlog_probe timewait_demo; do
    [[ -x "./$b" ]] || { note "缺少 ./$b，请先 make"; exit 1; }
done


echo "###### lab 02-tcp-state-machine 实跑记录 ######"
echo "date      : $(date -Is)"
echo "kernel    : $(uname -r)"
echo "gcc       : $(gcc --version | head -1)"
echo "python    : $(python3 --version)"
echo "uid/caps  : uid=$(id -u) CapEff=$(awk '/CapEff/{print $2}' /proc/self/status)"
echo

########################################################################
log "1. 基线：/proc/net/tcp 解析 + 状态分布（parse_proc_tcp.py）"
########################################################################
run_to 20 python3 parse_proc_tcp.py --summary | head -60
echo
echo "--- 只筛 TIME_WAIT / LISTEN（演示 --state 过滤）---"
run_to 20 python3 parse_proc_tcp.py --state TIME_WAIT | head -25
run_to 20 python3 parse_proc_tcp.py --state LISTEN | head -25

########################################################################
log "2. backlog 探针：listen(backlog=1) + 24 个并发连接 + 延迟 accept"
########################################################################
run_to 90 ./backlog_probe --port 0 --backlog 1 --clients 24 --delay 3 --connect-timeout 800
echo "backlog_probe exit=$?"
echo
echo "--- 解析本机 backlog 相关监听项（注意 Recv-Q 表示全连接队列长度）---"
run_to 20 python3 parse_proc_tcp.py --state LISTEN --summary 2>/dev/null | head -30

########################################################################
log "3. backlog 探针对照：backlog=16 + 立即 accept（不溢出）"
########################################################################
run_to 90 ./backlog_probe --port 0 --backlog 16 --clients 24 --delay 1 --accept-all --connect-timeout 1500
echo "backlog_probe(no-overflow) exit=$?"

########################################################################
log "4. TIME_WAIT 演示：主动关闭 / SO_LINGER(RST) / 状态机三场景"
########################################################################
run_to 60 ./timewait_demo --scenario 1
run_to 60 ./timewait_demo --scenario 2
run_to 60 ./timewait_demo --scenario 3

########################################################################
log "5. 固定四元组的 TIME_WAIT 与 SO_REUSEADDR（Python 版对照）"
########################################################################
run_to 30 python3 timewait_pair_demo.py --local-port 42000

########################################################################
log "6. 内核计数观测：/proc/net/snmp + /proc/net/netstat(TcpExt)"
########################################################################
mkdir -p .out
awk '/^Tcp:/{n++; print (n==1?"[names] ":"[values] ")$0}' /proc/net/snmp > .out/snmp_tcp.txt
awk '/^Udp:/{n++; print (n==1?"[names] ":"[values] ")$0}' /proc/net/snmp >> .out/snmp_tcp.txt
cat .out/snmp_tcp.txt
echo
cat /proc/net/sockstat
echo
run_to 30 python3 parse_proc_tcp.py --netstat TW TWRecycled TWKilled ListenOverflows ListenDrops \
    TCPTimeouts TCPFastRetrans TCPSackRecovery TCPLossProbes DelayedACKs TCPOFOQueue \
    TCPBacklogDrop SyncookiesSent TCPAbortOnClose
echo
echo "--- 采样 3 次状态分布（每次间隔 1s，观察 TIME_WAIT 堆积与消退）---"
run_to 30 python3 parse_proc_tcp.py --watch 3 --interval 1

########################################################################
log "7. 计数脚本 observe_counts.sh 的关键片段（完整输出见 .out/counts.log）"
########################################################################
run_to 60 bash observe_counts.sh > .out/counts.log 2>&1
echo "observe_counts.sh exit=$?"
head -40 .out/counts.log
echo "..."
grep -A 30 "TcpExt 关键计数" .out/counts.log | head -40

echo
echo "###### run.sh 结束 (rc=$rc) ######"
exit "$rc"
