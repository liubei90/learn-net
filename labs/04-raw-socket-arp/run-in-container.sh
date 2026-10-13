#!/usr/bin/env bash
#
# run-in-container.sh —— 在特权容器里以「真 root」复现 labs/04 的 AF_PACKET / ARP 实验
#
# 为什么需要它：
#   本机宿主 shell 处于 bwrap 沙箱内（CapEff=0、NoNewPrivs=1、/run 只读），
#   直接跑需要能力的实验要么靠 userns 绕过，要么就得换一个"真 root + 独立 netns"
#   的环境。本脚本用 Docker 提供后者：容器里 uid=0、CapEff=000001ffffffffff、
#   /run 可写（可以直接 `ip netns add`）。
#
# 实测结论（2026-10-13，Debian 13 宿主 + debian:trixie 容器）：
#   · ARP 请求/应答完整捕获
#   · ns1 邻居表 0 条 / ns2 1 条 —— AF_PACKET 绕过邻居子系统的直接证据
#   · 免费 ARP（sender IP == target IP）
#   · 手写原始 IP/UDP 报文（校验和 0x8115）被 ns2 的 nc 收到，Udp InDatagrams 0→1
#
# 边界：容器用的是**宿主内核**（uname -r 仍是宿主值），所以只能解决"权限"问题，
#       解决不了 labs/06 那种模块签名/内核模块的问题。
#
# 用法：
#   bash run-in-container.sh           # 自动拉镜像（若无）
#   IMAGE=debian:trixie bash run-in-container.sh
set -euo pipefail

IMAGE="${IMAGE:-debian:trixie}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

if ! command -v docker >/dev/null 2>&1; then
    echo "SKIP: 没有 docker。可以改用 README 2.2 的 unshare -Urn 方案。"
    exit 0
fi
if ! docker info >/dev/null 2>&1; then
    echo "SKIP: 当前用户无权访问 docker 守护进程（把用户加入 docker 组或 sudo）。"
    exit 0
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "本地没有 $IMAGE，正在拉取..."
    docker pull "$IMAGE"
fi

# ip / nc 直接从宿主挂进去（本机容器无外网、装不了 apt）
docker run --rm --privileged \
    -v "$REPO":/repo:ro \
    -v /usr/bin/ip:/usr/bin/ip:ro \
    -v /usr/bin/nc:/usr/bin/nc:ro \
    -v /usr/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:ro \
    "$IMAGE" sh -c '
set -u
L=/repo/labs/04-raw-socket-arp
echo "=== 环境 ==="
echo "  uid=$(id -u)  CapEff=$(grep CapEff /proc/self/status | awk "{print \$2}")"
echo "  本容器 netns 的网卡: $(ls /sys/class/net | tr "\n" " ")"
echo

echo "=== 拓扑：ns1(10.9.9.1) <--veth--> ns2(10.9.9.2) ==="
ip netns add ns1; ip netns add ns2
ip link add veth0 type veth peer name veth1
ip link set veth1 netns ns2
ip addr add 10.9.9.1/24 dev veth0; ip link set veth0 up; ip link set lo up
ip netns exec ns2 ip addr add 10.9.9.2/24 dev veth1
ip netns exec ns2 ip link set veth1 up; ip netns exec ns2 ip link set lo up
echo "  ns1.veth0=$(ip -4 -o addr show veth0 | awk "{print \$4}")  ns2.veth1=$(ip netns exec ns2 ip -4 -o addr show veth1 | awk "{print \$4}")"
echo

echo "=== [1] ARP 请求/应答（ns2 抓原始帧，ns1 发 who-has）==="
ip netns exec ns2 "$L/arp_sniff" veth1 2 > /tmp/sniff.log 2>&1 &
SN=$!; sleep 0.6
"$L/arp_who_has" veth0 10.9.9.2
sleep 0.8; wait $SN 2>/dev/null
cat /tmp/sniff.log
echo

echo "=== [2] 邻居表对比（AF_PACKET 绕过邻居子系统的证据）==="
echo "  ns1: $(ip neigh show | wc -l) 条"; ip neigh show | sed "s/^/      /"
echo "  ns2: $(ip netns exec ns2 ip neigh show | wc -l) 条"; ip netns exec ns2 ip neigh show | sed "s/^/      /"
echo

echo "=== [3] 免费 ARP（gratuitous）==="
ip netns exec ns2 "$L/arp_sniff" veth1 1 > /tmp/sniff2.log 2>&1 &
SN=$!; sleep 0.6
"$L/arp_gratuitous" veth0 10.9.9.99 --i-own-this-network
sleep 0.8; wait $SN 2>/dev/null
cat /tmp/sniff2.log
echo

echo "=== [4] 手写原始 IP/UDP 报文端到端 ==="
before=$(ip netns exec ns2 sh -c "awk \"/^Udp:/{print \\\$2}\" /proc/net/snmp | tail -1")
ip netns exec ns2 sh -c "timeout 6 nc -u -l -p 9999 > /tmp/udp.log 2>&1" &
sleep 1.0
"$L/raw_send_udp" 10.9.9.2 9999 "hello-from-raw-udp"
sleep 1.5
after=$(ip netns exec ns2 sh -c "awk \"/^Udp:/{print \\\$2}\" /proc/net/snmp | tail -1")
echo "  ns2 的 nc 收到: $(cat /tmp/udp.log)"
echo "  ns2 Udp InDatagrams: $before -> $after"
echo

echo "=== 清理 ==="
ip netns del ns1 2>/dev/null; ip netns del ns2 2>/dev/null
echo "  已删除两个 netns"
'
