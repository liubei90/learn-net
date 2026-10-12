# Lab 04：AF_PACKET 原始套接字与 ARP

> 对应文档：[`docs/04-网络层-IP路由与邻居子系统.md`](../../docs/04-网络层-IP路由与邻居子系统.md)

用 `AF_PACKET`/`SOCK_RAW` 直接收发以太网帧：抓帧、解析 ARP、
构造并发送 ARP 请求与免费 ARP，以及从零构造 IPv4+UDP 报文并计算首部校验和。

**本 Lab 在具备 `CAP_NET_RAW` 时才能运行。** 本机（uid=1000，`CapEff=0`）
用常规方式**无法**获得该能力，但**可以在非特权 user+net namespace 里真实运行** ——
下面的所有输出都是在本机 `unshare -Urn` 环境中**实测捕获**的，不是示意。

---

## 0. 本章定位

- **协议栈层次**：链路层（L2）。`AF_PACKET` 位于网络栈**最下方**，
  它在 `netif_receive_skb` 的分发路径上旁路注册 `packet_type`，
  因此能拿到「刚出网卡驱动、还没进 IP 层」的原始帧，也能直接注入帧绕过整个协议栈。
- **前置知识**：`docs/04`（邻居子系统 / ARP 状态机）、以太网帧格式、
  字节序（网络序 vs 主机序）。
- **学完能回答**：
  1. ARP 请求/应答的每个字段是什么？「who-has ... tell ...」对应哪两个字段？
  2. 为什么 `AF_PACKET` 需要 `CAP_NET_RAW`？它和普通 `AF_INET` socket 有什么本质区别？
  3. 为什么用原始套接字发 ARP 后，**本机内核的邻居缓存不会更新**？
  4. 免费 ARP（gratuitous ARP）有什么用，为什么它有安全风险？
  5. IPv4 首部校验和怎么算？为什么 `checksum(含校验和的头) == 0`？

---

## 1. 权限：本机的真实情况（重要）

### 1.1 实测的环境限制

```
$ id
uid=1000(user) gid=1000(user) ...

$ grep Cap /proc/self/status
CapInh: 0000000000000000
CapPrm: 0000000000000000
CapEff: 0000000000000000     <-- 无 CAP_NET_RAW
CapBnd: 0000000000000000     <-- 注意：连「边界集」都是空的
CapAmb: 0000000000000000

$ grep NoNewPrivs /proc/self/status
NoNewPrivs: 1                <-- 阻止通过 setuid/setcap 提权
```

这带来两个后果，必须说清楚：

1. **`sudo` 不可用**（实测报错）：
   ```
   $ sudo ./arp_who_has lo 127.0.0.1
   sudo: 已设置"no new privileges"标志，它阻止 sudo 以 root 身份运行。
   ```
2. **`setcap` 也救不了**：`CapBnd=0`（capability bounding set 为空）意味着
   即使给二进制文件打上 file capability，内核在 `execve` 时也会把它与
   边界集求交，结果仍是空集。所以本机上
   `sudo setcap cap_net_raw+ep ./arp_who_has` **不会**让程序获得 `CAP_NET_RAW`。

### 1.2 但是：非特权 user+net namespace 可用（实测）

```
$ unshare -Urn sh -c 'grep CapEff /proc/self/status; ip link add dummy0 type dummy'
CapEff: 000001ffffffffff     <-- namespace 内是 full caps
（dummy0 创建成功）
```

也就是说：在这个 namespace **内部**我们是「root」，`CAP_NET_RAW` 与
`CAP_NET_ADMIN` 都具备，而这些能力的作用域被限制在这个 netns 里，
**不会**影响宿主机的网络。这正是本 Lab 能在本机真跑的原因。

```console
$ unshare -Urn --map-root-user ./arp_who_has veth0 10.9.9.2
```

### 1.3 常规机器上的两种提权方式（供参考）

在**没有** `NoNewPrivs`/空边界集限制的普通机器上，标准做法是：

```console
# 方式一：给二进制最小授权（推荐，避免整个程序以 root 运行）
$ sudo setcap cap_net_raw+ep ./arp_who_has
$ getcap ./arp_who_has
./arp_who_has cap_net_raw=ep
$ ./arp_who_has eth0 10.0.1.1

# 方式二：直接用 root 运行
$ sudo ./arp_who_has eth0 10.0.1.1
```

> 本机上这两种方式都不可行，原因见 §1.1。

---

## 2. 安全实验配方：两个网络命名空间 + veth

### 2.1 为什么必须用「两个」netns（实测踩的坑）

如果 veth 两端都放在**同一个** netns：

```
ns A: veth0 = 10.9.9.1/24   veth1 = 10.9.9.2/24
```

那么当 ARP 请求（who-has 10.9.9.2 tell 10.9.9.1）从 veth0 发出、到达 veth1 后，
内核要为「10.9.9.1」选路来发送 ARP 应答。但 10.9.9.1 在**本 netns 内是本地地址**，
路由类型是 `RTN_LOCAL` → 出口设备是 `lo`。结果：

- 观察到的是**完全没有 ARP 应答**：

  ```console
  [*] 发送 ARP 请求: who-has 10.9.9.2 tell 10.9.9.1 (广播)
  [*] 已发送 42 字节（第 3 次）
  [*] 等待应答（5 秒）...
  [!] 超时：没有收到 ARP 应答
  ```

这就是「同 netns 双 veth 做 ARP 实验」的经典陷阱。
**正确做法：veth 两端分处两个 netns**，这样对端地址不是本地的，
应答才能正常从 veth1 发回。

### 2.2 可用配方（本机实测通过）

本机 `/run` 是只读的，`ip netns add` 会失败：

```console
$ unshare -Urnm ip netns add a
mkdir /run/netns failed: Read-only file system
```

因此改用「再开一个 `unshare -n` 进程 + `nsenter`」的等价做法（同一个
user namespace 下，所以可以跨 netns 移动设备）：

```bash
L=/path/to/labs/04-raw-socket-arp
unshare -Urn bash -c '
set -e
unshare -n sleep 120 & BPID=$!        # 在同一个 userns 下开第二个 netns（B）
sleep 0.5
ip link add veth0 type veth peer name veth1
ip link set veth1 netns $BPID         # 把 veth1 移入 ns B
ip addr add 10.9.9.1/24 dev veth0
ip link set veth0 up; ip link set lo up
nsenter -t $BPID -n ip addr add 10.9.9.2/24 dev veth1
nsenter -t $BPID -n ip link set veth1 up
nsenter -t $BPID -n ip link set lo up
# 在 ns B 抓包，在 ns A 发 ARP 请求
nsenter -t $BPID -n '"$L"'/arp_sniff veth1 2 > /tmp/sniff.log 2>&1 &
sleep 1
'"$L"'/arp_who_has veth0 10.9.9.2
sleep 1
grep -A3 ethertype=0x0806 /tmp/sniff.log
'
```

---

## 3. 实测输出（真实捕获）

### 3.1 发送 ARP 请求并收到应答（ns A 视角）

```console
[*] 接口=veth0 本机=10.9.9.1/00:11:22:33:44:06
[*] 发送 ARP 请求: who-has 10.9.9.2 tell 10.9.9.1 (广播)
[*] 已发送 42 字节（第 1 次）
[*] 已发送 42 字节（第 2 次）
[*] 已发送 42 字节（第 3 次）
[*] 等待应答（5 秒）...
[+] 收到 ARP 应答！
    ARP: htype=1 ptype=0x0800 hlen=6 plen=4 oper=2(REPLY)
         sender: mac=00:11:22:33:44:09 ip=10.9.9.2
         target: mac=00:11:22:33:44:06 ip=10.9.9.1
[+] 结论: 10.9.9.2 is-at 00:11:22:33:44:09
```

### 3.2 `arp_sniff` 在 ns B 抓到的完整一问一答

```console
[帧] len=42   ifindex=2 00:11:22:33:44:06 -> ff:ff:ff:ff:ff:ff ethertype=0x0806
    ARP: htype=1 ptype=0x0800 hlen=6 plen=4 oper=1(REQUEST)
         sender: mac=00:11:22:33:44:06 ip=10.9.9.1
         target: mac=00:00:00:00:00:00 ip=10.9.9.2
[帧] len=42   ifindex=2 00:11:22:33:44:09 -> 00:11:22:33:44:06 ethertype=0x0806
    ARP: htype=1 ptype=0x0800 hlen=6 plen=4 oper=2(REPLY)
         sender: mac=00:11:22:33:44:09 ip=10.9.9.2
         target: mac=00:11:22:33:44:06 ip=10.9.9.1
```

对照记忆：
- **请求**：以太网目的地址是广播 `ff:ff:ff:ff:ff:ff`；
  ARP 的 `target mac` 全 0（因为还不知道）；`oper=1`。
- **应答**：以太网目的地址是请求方的单播 MAC；
  ARP 的 `target mac` 填请求方 MAC；`oper=2`。

### 3.3 「谁学到了邻居？」—— AF_PACKET 旁路的实证

```console
=== ns A 的邻居缓存（发请求的一方）===
（空）

=== ns B 的邻居缓存（应答的一方）===
10.9.9.1 lladdr 00:11:22:33:44:06 STALE
```

**这是一个关键观察**：
- ns B 的内核**处理**了我们发的 ARP 请求（它经过了 B 的协议栈），
  所以 B 学到了 `10.9.9.1` 的映射；
- ns A 的内核**完全不知道**这件事 —— 因为应答是 `AF_PACKET` 直接注入的，
  走的不是 `arp_rcv` 那条正常路径，所以 A 的 `neigh` 表不会被更新。

结论：**用 `AF_PACKET` 手工发 ARP，内核的邻居子系统不参与**。
想验证内核自己的 ARP，请用 `ip neigh` 或直接 `ping`（由 `arp_solicit` 发起）。

### 3.4 免费 ARP 与合规闸门

不带授权标志会被拒绝（实测 `exit=3`）：

```console
$ ./arp_gratuitous lo 10.9.9.1 --wrong-flag
拒绝执行：缺少 --i-own-this-network。
免费 ARP 会改写同网段主机的邻居缓存，滥用属违法行为。
确认这是你自己的网络后，请显式加上该参数。
$ echo $?
3
```

带标志后正常发送：

```console
$ ./arp_gratuitous veth0 10.9.9.1 --i-own-this-network
[!] 已确认授权。接口=veth0 通告 10.9.9.1 is-at 00:11:22:33:44:08
[*] gratuitous ARP #1 已发送 (42 字节)
[*] gratuitous ARP #2 已发送 (42 字节)
[*] gratuitous ARP #3 已发送 (42 字节)
[*] 完成。可用 `ip neigh show dev veth0` 在邻居侧观察缓存变化。

$ nsenter -t $BPID -n ip neigh show dev veth1
10.9.9.1 lladdr 00:11:22:33:44:08 STALE
```

> ⚠️ **合规警告**：免费 ARP / ARP 应答可以改写他人的邻居缓存，
> 是 ARP 欺骗与中间人攻击的基础。**只能**在你自己的实验网络或
> 获得明确书面授权的网络中使用；建议始终在 netns 中做。

### 3.5 原始 IPv4+UDP 与校验和

```console
$ ./raw_send_udp 10.9.9.2 9999 "hello-from-raw-udp"
[*] IPv4 首部 20 字节，校验和 = 0x8115
[*] 目标 10.9.9.2:9999，UDP 长度 26，IP 总长 46
[*] 已发送 46 字节

# 对端（ns B）真的收到了：
$ cat /tmp/udp_recv.log
hello-from-raw-udp
```

校验和算法做了**双重数学验证**（RFC 1071 测试向量 + 自洽性不变量）：

```console
RFC1071 向量: got=0x220d expect=0x220d  PASS
IPv4 首部: 校验和=0x54ab, 回填后整段校验和=0x0000  PASS
```

- **测试向量**：`00 01 f2 03 f4 f5 f6 f7` 的校验和应为 `0x220d`（RFC 1071 附录）。
- **不变量**：把算出的校验和填回首部后，对**整段首部**（含校验和字段）
  再算一次，结果必须是 `0x0000`。这是反码求和的核心性质，
  也是接收端校验的做法。

---

## 4. ARP 与邻居子系统要点

### 4.1 ARP 报文结构（以太网 + IPv4，28 字节）

| 偏移 | 字段 | 值 | 说明 |
|---|---|---|---|
| 0 | `htype` | 1 | 硬件类型，1 = 以太网 |
| 2 | `ptype` | 0x0800 | 上层协议，IPv4 |
| 4 | `hlen` | 6 | 硬件地址长度 |
| 5 | `plen` | 4 | 协议地址长度 |
| 6 | `oper` | 1/2 | 1 = request，2 = reply |
| 8 | `sha` | MAC | sender hardware address |
| 14 | `spa` | IP | sender protocol address |
| 18 | `tha` | MAC | target hardware address（请求时全 0） |
| 24 | `tpa` | IP | target protocol address |

「who-has **10.9.9.2** tell **10.9.9.1**」对应：`tpa=10.9.9.2`，`spa=10.9.9.1`。

### 4.2 内核里对应的实现

| 用户态看到的 | 内核实现 |
|---|---|
| 收到 ARP 帧 | `arp_rcv` → `arp_process`（`net/ipv4/arp.c`） |
| 发出 ARP 请求 | `arp_solicit` → `arp_send`（`net/ipv4/arp.c`） |
| 邻居表项更新 | `neigh_update`（`net/core/neighbour.c`） |
| 邻居缓存查看 | `ip neigh`；内核态 `struct neighbour` |

配置旋钮（`sysctl`，定义在 `net/ipv4/devinet.c`，用法宏在
`include/linux/inetdevice.h`）：
`arp_ignore`（收到请求时在什么条件下应答）、
`arp_announce`（作为请求源时如何选择源地址）、
`proxy_arp`（是否代理其他地址的 ARP）。

### 4.3 AF_PACKET 的旁路位置

```
驱动 RX 中断 → napi → netif_receive_skb
                          ├─► AF_PACKET 的 packet_type（packet_rcv/tpacket_rcv）  ← 我们的程序在这里
                          └─► ip_rcv → ...（正常协议栈）
```

- 收方向：`AF_PACKET` 注册的 `packet_type` 在 `__netif_receive_skb_core`
  里被遍历调用，因此**先于** IP 层拿到帧；
- 发方向：`sendto` 最终走到 `dev_queue_xmit`，**跳过** IP 层与路由选择
  （我们自己拼好了以太网头）。
- 正因为它越过协议栈、能看到/改写任意 L2 帧，内核才要求 `CAP_NET_RAW`。

---

## 5. 常见陷阱

1. **同 netns 双 veth 收不到 ARP 应答** —— 对端地址是本地地址，
   应答会从 `lo` 出去。必须分两个 netns（§2.1，实测）。
2. **字节序** —— `htype/ptype/oper` 都要 `htons`；`sha/tha/spa/tpa` 是
   字节数组，**不要**再转。忘了 `htons` 会发出 `oper=0x0100` 这种报文。
3. **奇数长度不补零** —— 校验和函数遇到奇数长度必须补一个 0 字节，
   否则结果错误（本 Lab 的 `checksum()` 已处理）。
4. **`ip_off` 是网络序且含标志位** —— `IP_DF` 等标志在高 3 位，
   位运算前必须先 `ntohs`，写完再 `htons`，否则标志位会错位。
5. **`ifr_name` 溢出** —— `struct ifreq.ifr_name` 只有 `IFNAMSIZ`(16) 字节，
   务必用 `strncpy(..., IFNAMSIZ-1)` 并保证 NUL 结尾。
6. **packed 结构体取地址** —— `__attribute__((packed))` 的成员可能未对齐，
   编译器禁止 `&pk->spa`（`-Waddress-of-packed-member`）。
   用 `memcpy` 或逐字段赋值。
7. **`SO_BINDTODEVICE` 需要 `CAP_NET_RAW`** —— 在非特权环境会直接失败，
   不要误以为是接口名写错。
8. **`_GNU_SOURCE` 重复定义** —— 若 Makefile 里已 `-D_GNU_SOURCE`，
   源文件再 `#define` 会触发 `redefined` 警告（`-Werror` 下直接失败）。
   本 Lab 只在源文件里定义，Makefile 刻意不重复定义。

---

## 6. 构建

```console
$ make clean all
cc -std=c11 -O2 -g -Wall -Wextra -c -o arp.o arp.c
cc -std=c11 -O2 -g -Wall -Wextra -o arp_sniff arp_sniff.c arp.o
cc -std=c11 -O2 -g -Wall -Wextra -o arp_who_has arp_who_has.c arp.o
cc -std=c11 -O2 -g -Wall -Wextra -o arp_gratuitous arp_gratuitous.c arp.o
cc -std=c11 -O2 -g -Wall -Wextra -o raw_send_udp raw_send_udp.c
```

`all` 退出码 0，**零警告**（已用 `-Werror` 验证：`STRICT_EXIT=0`）。
编译**不需要**任何特权，也不需要接口存在。

---

## 7. 文件清单

| 文件 | 作用 |
|---|---|
| `arp.h` / `arp.c` | 报文结构（packed）、接口信息查询、`AF_PACKET` 打开、格式化打印 |
| `arp_sniff.c` | 抓帧 + 以太网头 + ARP 逐字段解析 |
| `arp_who_has.c` | 构造/发送 ARP 请求并等待应答 |
| `arp_gratuitous.c` | 免费 ARP（带合规闸门） |
| `raw_send_udp.c` | 手工 IPv4+UDP 与首部校验和 |
| `Makefile` | `all` / `clean` |

---

## 8. 自测题

<details><summary>1. 为什么 AF_PACKET 需要 CAP_NET_RAW？</summary>

因为它允许用户态直接读取和注入链路层帧，绕过协议栈。
收方向能拿到本机所有流量（等价于抓包能力），
发方向能伪造任意源 MAC/IP 的帧（等价于欺骗能力）。
这两者都足以用于窃听或攻击，因此被 `CAP_NET_RAW` 保护。
</details>

<details><summary>2. 用 arp_who_has 发出请求并收到应答后，为什么 `ip neigh` 里没有这条记录？</summary>

因为应答帧是被 `AF_PACKET` 直接注入的，没有经过 `arp_rcv`/`arp_process`，
而邻居表项的建立/更新只发生在 `arp_process` → `neigh_update` 这条路径上。
实测中 ns A 的邻居表为空、ns B 的表里有条目，正好印证了这一点。
</details>

<details><summary>3. `checksum(含校验和的 IPv4 首部) == 0` 说明了什么？</summary>

反码求和的不变量：首部所有 16 位字的反码和再取反为 0。
发送端填好校验和后，接收端对整段首部（含校验和字段）做同样的
反码求和，结果应为 `0x0000` 或 `0xffff`（取决于实现是否取反），
否则首部在传输中受损，报文被丢弃。
本 Lab 实测该值为 `0x0000`。
</details>

<details><summary>4. 为什么 ARP 请求要用广播 MAC，而应答用单播？</summary>

请求方还不知道目标 MAC（`tha` 全 0），只能广播让同广播域所有主机看到，
由持有目标 IP 的主机认领。应答时请求方的 MAC 已从请求的 `sha` 字段获知，
因此可以单播，避免打扰其他主机。
</details>

<details><summary>5. 免费 ARP 的两种形态及其典型用途？</summary>

形态一：ARP **REQUEST** 且 `spa == tpa`（本机地址），广播 —— 这是 keepalived
等 HA 软件接管 VIP 后刷新的方式，同时具备地址冲突检测能力。
形态二：ARP **REPLY** 且目标 MAC 为广播。
用途：HA 故障切换时更新交换机与主机的 ARP 缓存、网卡/地址变更通告、
地址冲突探测。
</details>

<details><summary>6. 本机为什么不能用 `sudo setcap cap_net_raw+ep ./arp_who_has`？</summary>

两个独立原因：其一 `NoNewPrivs=1` 使 `sudo` 无法提权；
其二 `CapBnd=0`（边界集为空）使得即使设置了 file capability，
`execve` 时与边界集求交的结果仍是空集，程序依然拿不到 `CAP_NET_RAW`。
因此本机只能走非特权 user+net namespace 路线。
</details>

<details><summary>7. 抓包时看到 `ifindex=2` 而本机 `veth1` 是 2 号，说明了什么？</summary>

`ifindex` 是**每个 netns 独立编号**的，netns B 里的 2 号接口就是 B 的 `veth1`。
netlink/`AF_PACKET` 返回的索引必须放到对应 netns 里解释，跨 netns 比较索引没有意义。
</details>

<details><summary>8. 为什么 `recvfrom` 之前要检查 `n >= sizeof(eth_hdr) + sizeof(arp_msg)`？</summary>

收到的帧长度由网络/对端决定，属于不可信输入。若直接按
`struct arp_msg *` 解引用而缓冲区实际更短，就会越界读。
必须先验证长度，再解析。这是所有报文解析器的通用纪律。
</details>

---

## 9. 延伸阅读

- `man 7 packet`、`man 7 arp` —— `AF_PACKET` 与 ARP 的权威说明。
- `man 3 packet` —— `sockaddr_ll`、`PACKET_*` 选项。
- 内核：`net/ipv4/arp.c`（`arp_rcv`/`arp_process`/`arp_solicit`）、
  `net/core/neighbour.c`（`neigh_update`/状态机）、
  `net/packet/af_packet.c`（`packet_rcv`/`tpacket_rcv`）。
  以 6.12 为准，可在 <https://elixir.bootlin.com/linux/v6.12/source/> 核对。
- RFC 826（ARP）、RFC 1071（Internet 校验和）、RFC 5227（IPv4 地址冲突检测）。
