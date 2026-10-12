#!/usr/bin/env bash
# labs/01-socket-api-basics/run.sh
# 在 127.0.0.1 上把四个二进制全部真跑一遍，并把 ss / proc 观测一并输出。
# 设计要点：端口用内核临时分配（--port 0）避免冲突；所有后台进程用 trap EXIT 清理；
#          每一步都有超时，脚本不会挂死；无需 root。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

SRV_PID=""
UDP_PID=""
CLIENT_PID=""
TIMEOUT_BIN="$(command -v timeout || true)"
rc=0

log()  { printf '\n===== %s =====\n' "$*"; }
note() { printf '[run.sh] %s\n' "$*"; }

cleanup() {
    local ec=$?
    for p in "$CLIENT_PID" "$SRV_PID" "$UDP_PID"; do
        if [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null; then
            kill -TERM "$p" 2>/dev/null
            for _ in $(seq 1 20); do
                kill -0 "$p" 2>/dev/null || break
                sleep 0.1
            done
            kill -KILL "$p" 2>/dev/null
        fi
    done
    wait 2>/dev/null
    return $ec
}
trap cleanup EXIT INT TERM

run_to() { # run_to <seconds> <cmd...>
    local secs="$1"; shift
    if [[ -n "$TIMEOUT_BIN" ]]; then
        "$TIMEOUT_BIN" "$secs" "$@"
    else
        "$@"
    fi
}

for b in tcp_echo_server tcp_client udp_echo sockopt_probe; do
    if [[ ! -x "./$b" ]]; then
        note "缺少 ./$b，请先 make"
        exit 1
    fi
done


echo "###### lab 01-socket-api-basics 实跑记录 ######"
echo "date      : $(date -Is)"
echo "kernel    : $(uname -r)"
echo "gcc       : $(gcc --version | head -1)"
echo "uid/caps  : uid=$(id -u) CapEff=$(awk '/CapEff/{print $2}' /proc/self/status)"
echo "port range: $(cat /proc/sys/net/ipv4/ip_local_port_range | tr '\t' ' ')"
echo

########################################################################
log "0. TCP 服务端启动（epoll LT，端口由内核临时分配）"
########################################################################
mkdir -p .out
: > .out/server_lt.log
./tcp_echo_server --port 0 --trigger lt --verbose > .out/server_lt.log 2>&1 &
SRV_PID=$!
for _ in $(seq 1 50); do
    grep -q LISTENING .out/server_lt.log 2>/dev/null && break
    sleep 0.1
done
cat .out/server_lt.log
TCP_PORT="$(awk '/LISTENING/{split($2,a,":"); print a[2]}' .out/server_lt.log)"
if [[ -z "${TCP_PORT:-}" ]]; then
    note "服务端未启动成功，跳过 TCP 部分"
    rc=1
else
    note "TCP 监听端口 = $TCP_PORT"
    echo
    echo "--- 观测 1: ss -lnt（LISTEN 状态，Recv-Q 是全连接队列当前长度，Send-Q 是 backlog 上限）---"
    ss -lnt "sport = :$TCP_PORT" || true

    echo
    echo "--- 观测 2: ss -lntp / 内核 backlog 上限 ---"
    ss -lntp "sport = :$TCP_PORT" || true
    echo "/proc/sys/net/core/somaxconn = $(cat /proc/sys/net/core/somaxconn)"

    ####################################################################
    log "1. TCP 客户端：4 次 x 4096B，nodelay 关闭（可观察 Nagle 与延迟 ACK 交互）"
    ####################################################################
    run_to 30 ./tcp_client --host 127.0.0.1 --port "$TCP_PORT" --count 4 --size 4096
    echo "client exit=$?"

    ####################################################################
    log "2. TCP 客户端：8 次 x 65536B，开启 TCP_NODELAY"
    ####################################################################
    run_to 30 ./tcp_client --host 127.0.0.1 --port "$TCP_PORT" --count 8 --size 65536 --nodelay
    echo "client exit=$?"

    ####################################################################
    log "3. 客户端显式设置缓冲区：观察内核加倍语义 + 连接存续期的 ss -tni"
    ####################################################################
    # --sleep-after 让客户端在 ESTABLISHED 状态停住 3 秒，外部才有机会抓快照
    ( ./tcp_client --host 127.0.0.1 --port "$TCP_PORT" --count 200 --size 4096 \
        --sndbuf 65536 --rcvbuf 65536 --sleep-after 3 > .out/client_buf.log 2>&1 ) &
    CLIENT_PID=$!
    sleep 1.2
    echo "--- 观测 3: ss -tnie 连接级 TCP_INFO（-e 打印 skmem 记账、-i 打印 cwnd/rtt/retrans）---"
    ss -tnie | grep -A1 -E "127.0.0.1:$TCP_PORT" || true
    echo
    echo "--- 观测 3b: ss -tm（socket memory，对应 sk_mem_charge/sk_forward_alloc 记账）---"
    ss -tm | grep -B1 -A1 -E "127.0.0.1:$TCP_PORT" || true
    echo
    echo "--- 观测 3c: ss -tln（LISTEN 的 Recv-Q/Send-Q 与 backlog）---"
    ss -tln "sport = :$TCP_PORT" || true
    wait "$CLIENT_PID" 2>/dev/null
    CLIENT_PID=""
    echo "--- 客户端自述（getsockopt 实测值，已过滤长 payload）---"
    grep -v '^echo check' .out/client_buf.log | sed -n '1,20p'
    grep -E 'round-trips|tcpi_(state|snd_cwnd|rtt |retransmits|total_retrans|sacked|snd_wnd|rcv_space)' .out/client_buf.log | head -40

    ####################################################################
    log "4. 客户端直连后立即关闭：观察 TIME_WAIT（主动关闭方）"
    ####################################################################
    run_to 20 ./tcp_client --host 127.0.0.1 --port "$TCP_PORT" --count 1 --size 64
    echo "--- 观测 4: ss -tan state time-wait ---"
    ss -tan state time-wait | head -20 || true
    echo "--- 观测 4b: /proc/net/netstat TcpExt 的 TW/TWRecycled/TWKilled ---"
    python3 "$HERE/../02-tcp-state-machine/parse_proc_tcp.py" --netstat TW TWRecycled TWKilled 2>/dev/null \
        || awk '/^TcpExt:/{n++; if(n==1){for(i=1;i<=NF;i++) if($i=="TW") c=i} else if(n==2 && c){print "  TW(TCPTimeWait) =", $c; exit}}' /proc/net/netstat

    echo
    echo "--- 观测 5: /proc/net/tcp（十六进制，只取前 10 行）---"
    head -10 /proc/net/tcp

    echo
    echo "--- 观测 6: /proc/net/snmp 的 Tcp 行 ---"
    grep -A1 '^Tcp:' /proc/net/snmp | head -4

    note "关闭 TCP 服务端"
    kill -TERM "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
    echo "--- 服务端退出时打印的连接 TCP_INFO ---"
    tail -40 .out/server_lt.log
fi

########################################################################
log "5. UDP 回显：无连接 sendto/recvfrom + connected UDP"
########################################################################
: > .out/udp.log
./udp_echo server --port 0 > .out/udp.log 2>&1 &
UDP_PID=$!
for _ in $(seq 1 50); do
    grep -q UDP_ECHO_LISTENING .out/udp.log 2>/dev/null && break
    sleep 0.1
done
cat .out/udp.log
UDP_PORT="$(awk '/UDP_ECHO_LISTENING/{split($2,a,":"); print a[2]}' .out/udp.log)"
if [[ -n "${UDP_PORT:-}" ]]; then
    note "UDP 端口 = $UDP_PORT"
    echo "--- 观测 7: ss -lun（UDP 监听，Recv-Q 为接收队列中排队的 skb 字节数）---"
    ss -lun "sport = :$UDP_PORT" || true
    echo
    run_to 20 ./udp_echo client --port "$UDP_PORT" --count 3 --size 512
    echo "udp client(no-connect) exit=$?"
    echo
    run_to 20 ./udp_echo client --port "$UDP_PORT" --count 2 --size 4096 --connect
    echo "udp client(connected) exit=$?"
    echo
    echo "--- 观测 8: UDP 统计 /proc/net/snmp 的 Udp 行 ---"
    grep -A1 '^Udp:' /proc/net/snmp | head -4
    echo
    echo "--- 观测 8b: 向未监听端口发包触发 ICMP port unreachable ---"
    run_to 10 ./udp_echo client --port 1 --count 1 --size 32 --connect || true
    echo "（connected UDP 收到 ICMP 后 recv 返回 ECONNREFUSED，行内含 NoPorts 统计增长）"
    grep -A1 '^Udp:' /proc/net/snmp | head -4
    kill -TERM "$UDP_PID" 2>/dev/null
    wait "$UDP_PID" 2>/dev/null
    UDP_PID=""
fi

########################################################################
log "6. 套接字选项与缓冲记账探针"
########################################################################
run_to 60 ./sockopt_probe
echo "sockopt_probe exit=$?"

########################################################################
log "7. 收尾：确认无遗留监听/连接"
########################################################################
ss -lntu | head -20 || true

echo
echo "###### run.sh 结束 (rc=$rc) ######"
exit "$rc"
