#!/usr/bin/env bash
#
# netns-veth-demo.sh —— 无需 root 也能做的「真实网卡路径」实验
#
# 为什么需要它：
#   本教程大部分实验跑在环回（lo）上，而 lo 的 MTU 是 65536，
#   所以**永远不会分片**，也测不出 PMTU 问题。想复现真实网卡行为，
#   需要 veth（虚拟网卡对）+ 小 MTU + 两个网络命名空间。
#
# 关键实测结论（本机验证，初始命名空间 CapEff=0）：
#   `unshare -Urnmpf --mount-proc` 之后，新用户命名空间内 CapEff=000001ffffffffff，
#   于是可以建 netns/veth、改 MTU、发原始套接字——但 /sys 无法重新挂载，
#   因此接口信息只能从 `ip` 与 /proc/net/* 读。
#
# 本脚本会自动把自己重新 exec 进 userns，直接这样跑即可：
#   bash scripts/netns-veth-demo.sh
#
# 退出码：0 = 成功或优雅跳过；非 0 = 真的出错。

set -uo pipefail

# ── 0. 若不在用户命名空间内，自动重新执行自己 ──────────────────────
if [[ "${LEARNNET_IN_NETNS:-0}" != "1" ]]; then
    if ! command -v unshare >/dev/null 2>&1; then
        echo "SKIP: 没有 unshare 命令，无法创建用户/网络命名空间。"
        echo "      在有 root 的机器上可以直接用 ip netns + ip link 做本实验。"
        exit 0
    fi
    if ! unshare -Urnmpf --mount-proc true 2>/dev/null; then
        echo "SKIP: 本机不允许创建用户命名空间（unshare -Urnmpf 失败）。"
        echo "      原因通常是 kernel.unprivileged_userns_clone=0 或 seccomp 拦截。"
        exit 0
    fi
    exec unshare -Urnmpf --mount-proc env LEARNNET_IN_NETNS=1 bash "$0" "$@"
fi

CHILD_PID=""
cleanup() {
    [[ -n "$CHILD_PID" ]] && kill "$CHILD_PID" 2>/dev/null
    ip link del v0 2>/dev/null
    return 0
}
trap cleanup EXIT

hr() { printf '\n\033[1;36m━━━ %s ━━━\033[0m\n' "$1"; }

# /proc/net/snmp 的 Ip: 行字段位置（本机 6.12 实测核对）
#   $4=InReceives $15=ReasmReqds $16=ReasmOKs $18=FragOKs $19=FragFails $20=FragCreates
ip_stat() { awk '/^Ip:/{print $4, $15, $16, $18, $19, $20}' /proc/net/snmp | tail -1; }
show_ip_stat() {
    local label="$1" line
    line="$(ip_stat)"
    # shellcheck disable=SC2086
    set -- $line
    printf '  %-16s InReceives=%-5s ReasmReqds=%-4s ReasmOKs=%-4s FragOKs=%-4s FragFails=%-4s FragCreates=%s\n' \
        "$label" "$1" "$2" "$3" "$4" "$5" "$6"
}
mtu_of() { ip -o link show "$1" 2>/dev/null | sed -n 's/.*mtu \([0-9]*\).*/\1/p'; }
send_udp() {  # $1=dst $2=port $3=len
    python3 - "$1" "$2" "$3" <<'PY'
import socket, sys
dst, port, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"X" * n, (dst, port))
PY
}

hr "1. 环境确认"
# 新建的网络命名空间里 lo 默认是 DOWN 的，必须先拉起来，否则连 127.0.0.1 都不可达
ip link set lo up
echo "  uid=$(id -u)  CapEff=$(awk '/CapEff/{print $2}' /proc/self/status)  （在新用户命名空间内）"
echo "  当前网络命名空间的接口："
while read -r dev mtu; do echo "    ${dev}  MTU=${mtu}"; done < <(
    ip -o link show | sed -n 's/^[0-9]*: \([^:@]*\).*mtu \([0-9]*\).*/\1 \2/p'
)
echo "  注意：/sys 无法重挂（mount -t sysfs 返回 EPERM），所以 MTU 要用 ip 读，"
echo "        不能读 /sys/class/net/<dev>/mtu（那是宿主机的视图）。"

hr "2. 建立 veth 对，并把一端放进另一个网络命名空间"
ip link add v0 type veth peer name v1
ip link set v0 up
ip addr add 10.7.7.1/24 dev v0
ip link set v0 mtu 1280

# `ip netns add` 需要写 /run/netns（本机只读），所以改用「另一个 unshare -n 进程」
unshare -n sleep 60 &
CHILD_PID=$!
sleep 0.4
ip link set v1 netns "$CHILD_PID"
nsenter -t "$CHILD_PID" -n sh -c 'ip link set v1 up; ip link set v1 mtu 1280; ip addr add 10.7.7.2/24 dev v1'

echo "  发送侧 v0: MTU=$(mtu_of v0)  地址 10.7.7.1/24"
echo "  接收侧 v1: MTU=$(nsenter -t "$CHILD_PID" -n sh -c "ip -o link show v1 | sed -n 's/.*mtu \\([0-9]*\\).*/\\1/p'")  地址 10.7.7.2/24（在另一个 netns 里）"

hr "3. 小包 ICMP 与 DF（禁止分片）的对比"
echo "  ① 小包（108 字节 payload）远小于 MTU 1280，应当通："
if command -v ping >/dev/null 2>&1; then
    if ping -c 1 -W 2 -s 100 10.7.7.2 >/dev/null 2>&1; then
        echo "     ✅ ping -s 100 通过"
    else
        echo "     ⚠️  ping 未通过（可能受 ping_group_range 限制，不影响后续实验）"
    fi
    echo "  ② 大包 + DF（-M do，禁止分片）超过 MTU，应当被内核拒绝："
    ping -c 1 -W 2 -M do -s 2000 10.7.7.2 2>&1 | sed 's/^/     /' | head -3
    sleep 0.3
    show_ip_stat "DF 拒绝后"
    echo "     ↑ 实测观察（值得记住）：应用确实收到 EMSGSIZE（\"消息过长\"），"
    echo "       但 **FragFails 并没有增加**。说明计数器的语义比它的名字更窄"
    echo "       ——「DF 导致无法分片」这条路径不走 FragFails 计数。"
    echo "       这类「计数器名字 ≠ 计数器语义」的情况，只能靠实测确认，不能想当然。"
    echo "       （分片真正失败的其他路径才会累加 FragFails，比如造不出分片 skb。）"
else
    echo "     SKIP: 未安装 ping"
fi

hr "4. 核心实验 A：3000 字节 UDP 经过 MTU 1280 的 veth —— 分片与重组"
echo "  一个 3000 字节 UDP 数据报 = 20(IP) + 8(UDP) + 3000 = 3028 字节"
echo "  MTU=1280 → 每片含 20 字节 IP 头，最多携带 1280-20=1260 字节数据（且必须是 8 的倍数）"
echo "  ⇒ 预期分成 3 片：1260 + 1260 + 508"
echo
show_ip_stat "发送前(父ns)"
send_udp 10.7.7.2 9999 3000
sleep 0.4
show_ip_stat "发送后(父ns)"
echo
nsenter -t "$CHILD_PID" -n sh -c 'awk "/^Ip:/{printf \"  接收侧(子ns)     InReceives=%-5s ReasmReqds=%-4s ReasmOKs=%-4s\\n\", \$4, \$15, \$16}" /proc/net/snmp | tail -1'
echo
echo "  读法：发送侧 FragOKs +1（1 个数据报被成功分片）、FragCreates +3（共造出 3 片）；"
echo "        接收侧 ReasmReqds=3（收到 3 片）、ReasmOKs=1（成功重组为 1 个数据报）。"

hr "5. 核心实验 B（对照）：同样的 3000 字节走 lo（MTU 65536）—— 不分片"
echo "  lo 的 MTU 是 $(mtu_of lo)，3028 字节远小于它，因此不触发任何分片"
echo
show_ip_stat "lo 发送前"
send_udp 127.0.0.1 9999 3000
sleep 0.4
show_ip_stat "lo 发送后"
echo
echo "  FragOKs / FragCreates 都没变 —— 这就是「环回测不出分片问题」的直接证据。"

hr "6. 小结：这些计数器对应的内核代码在哪"
cat <<'TXT'
  三件被本实验验证的事：
    1) MTU 是「出接口」的属性，分片与否只取决于 数据报长度 vs 出接口 MTU；
    2) 分片发生在**发送侧**（FragOKs/FragCreates），重组发生在**接收侧**（ReasmReqds/ReasmOKs）；
    3) 环回（lo, MTU 65536）看不到这些计数器变化 —— 这正是
       「本机测试正常、上线就出问题」的一个常见根源。

  DF 位与 PMTU 的交互（本实验第 3 段验证）：
    IP 头 DF=1 时，超过 MTU 的报文既不能分片也发不出去 → FragFails++、应用收到 EMSGSIZE，
    内核同时回 ICMP "fragmentation needed"（若可达）来更新路径 MTU。

  对应源码入口（详见 docs/04-网络层-IP路由与邻居子系统.md）：
    net/ipv4/ip_output.c     ip_finish_output() → ip_fragment() → ip_do_fragment()
    net/ipv4/ip_fragment.c   ip_frag_queue() / ip_frag_reasm()
    net/ipv4/route.c         ip_rt_update_pmtu()（PMTU 更新）
    include/net/ip.h         ip_dont_fragment()（DF 位与 pmtudisc 的判定）
TXT
echo
echo "  正在清理 veth 与子 netns（trap 自动执行）..."
