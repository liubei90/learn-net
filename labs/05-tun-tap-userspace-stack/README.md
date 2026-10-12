# Lab 05：TUN/TAP 与最小用户态协议栈

> 对应文档：[`docs/05-链路层-网卡驱动-NAPI与中断.md`](../../docs/05-链路层-网卡驱动-NAPI与中断.md)

打开 `/dev/net/tun`，在**用户态**实现一个最小协议栈：
读 IP 包 → 解析 IPv4 → 对 ICMP Echo Request 回 Echo Reply → 写回 TUN，
目标是让 `ping -I tun0 <addr>` 能通。

**本机无法运行**：`/dev/net/tun` 不存在，且内核 tun 驱动未加载（见 §5 的实测证据）。
因此本 Lab 的交付标准是：**编译零警告通过 + 协议逻辑经单元自检验证 + 给出 root 运行步骤**。
所有无法运行的结论都有实测命令与真实报错支撑，**没有任何伪造的成功输出**。

---

## 0. 本章定位

- **协议栈层次**：TUN 是**三层**虚拟网络设备（L3，点对点）。
  它挂在网络栈的最下游：`ip_finish_output2` 之后的 `ndo_start_xmit`
  不再是真实网卡，而是把 skb 交给用户态进程。
- **前置知识**：`docs/05`（`net_device`、`ndo_start_xmit`、NAPI 收包路径）、
  `docs/04`（IP 首部、路由）、`labs/04`（手工构造报文与校验和）。
- **学完能回答**：
  1. TUN 和 TAP 有什么区别？各自典型用途是什么？
  2. 内核为什么要把 skb「拷给」用户态？收发两个方向分别走哪条路径？
  3. 为什么 `IFF_NO_PI` 很重要？不加会怎样？
  4. VPN（WireGuard/OpenVPN）、容器网络、QEMU 网络分别在用 TUN 还是 TAP？
  5. 一个 ICMP Echo Reply 需要改哪些字段？校验和范围是什么？

---

## 1. 构建

```console
$ make clean all
cc -std=c11 -O2 -g -Wall -Wextra -c -o main.o main.c
cc -std=c11 -O2 -g -Wall -Wextra -c -o tun.o tun.c
cc -std=c11 -O2 -g -Wall -Wextra -c -o ip.o ip.c
cc -std=c11 -O2 -g -Wall -Wextra -c -o icmp.o icmp.c
cc -std=c11 -O2 -g -Wall -Wextra -c -o checksum.o checksum.c
cc -std=c11 -O2 -g -Wall -Wextra -o tun_stack main.o tun.o ip.o icmp.o checksum.o
```

`all` 退出码 0，**零警告**（已用 `-Werror` 验证：`STRICT_EXIT=0`）。

**编译不需要** root、不需要 `/dev/net/tun`、也不需要 tun 驱动 —— 
这是刻意设计的：代码里的 TUN 打开失败路径只影响运行期。

---

## 2. 代码分层

```
main.c       事件循环：read(tun) -> ipv4_parse -> icmp_build_echo_reply -> write(tun)
  ├── tun.c      /dev/net/tun 打开（TUNSETIFF）与接口配置（SIOCSIFADDR/SIOCSIFFLAGS）
  ├── ip.c       IPv4 首部校验与解析（版本/IHL/长度/校验和）
  ├── icmp.c     ICMP Echo Reply 构造
  └── checksum.c RFC 1071 反码求和（IPv4 与 ICMP 共用）
```

对应内核实现（以 6.12 为准）：

| 用户态函数 | 内核对应 | 说明 |
|---|---|---|
| `tun_alloc` + `ioctl(TUNSETIFF)` | `drivers/net/tun.c`：`tun_set_iff` | 创建/绑定 tun 网络设备 |
| `read(fd, ...)` | `tun_chr_read_iter` → `tun_do_read` | 从 socket 队列取出 skb 并拷到用户态 |
| `write(fd, ...)` | `tun_chr_write_iter` → `tun_get_user` | 用户态数据包装成 skb |
| 内核发往 tun0 的包 | `tun_net_xmit` | 入队并唤醒用户态 |
| 写回后的注入 | `tun_get_user` 末尾调用 `netif_rx()` | 重新进入协议栈**接收**路径（`netif_rx_ni()` 在 5.18 已删除） |
| 我们的 `ipv4_parse` | `net/ipv4/ip_input.c`：`ip_rcv_core` 的长度/校验和检查 | 大幅简化 |
| 我们的 `icmp_build_echo_reply` | `net/ipv4/icmp.c`：`icmp_echo`/`icmp_reply` | 只做 Echo |

---

## 3. 内核-用户态数据流（为什么要把 skb 拷出来）

**发送方向（内核对 → 用户态）**：

```
应用 write() 到 tun0 网段内的地址
  -> 路由查找：出口设备 = tun0
  -> ip_finish_output2 -> neigh_output -> dev_hard_start_xmit
  -> ndo_start_xmit == tun_net_xmit()
       |  这里没有真实硬件！
       |  把 skb 挂到 tun 设备关联的 socket 接收队列
       `-> 唤醒阻塞在 read() 的用户态进程
  -> 用户态 read() 拿到「裸 IP 包」，自己做协议处理
```

**接收方向（用户态 → 内核）**：

```
用户态 write(fd, ip_packet)
  -> tun_chr_write_iter -> tun_get_user()
       |  把用户态缓冲包装成 skb（IFF_TUN 下按 IP 解析）
       `-> netif_rx() 重新注入协议栈接收路径
  -> 内核按正常收包流程处理（ip_rcv ...），
     最终 ping 的 socket 读到 Echo Reply
```

**代价**：每次跨越用户态/内核态边界都要 **一次内存拷贝 + 一次上下文切换**（唤醒）。
这就是 VPN 的固有性能成本，也是业界一直在优化
（多队列 TUN、`vhost`、`io_uring`、XDP 等）的原因。
本 Lab 是单线程阻塞 `read`，每条包都唤醒一次，性能最朴素。

---

## 4. TUN vs TAP

| | **TUN** | **TAP** |
|---|---|---|
| 层次 | L3（点对点，无 MAC） | L2（以太网，有 MAC） |
| 用户态读写 | 裸 **IP 包** | 完整**以太网帧** |
| 能否跑 ARP/DHCP/桥接 | 不能（无 L2） | 能 |
| 典型用途 | VPN（WireGuard、OpenVPN 的 tun 模式）、IP 隧道、本 Lab | 虚拟机网络（QEMU/KVM 的 `-netdev tap`）、容器 CNI 的桥接、用户态交换机 |

三者结合本教程其他部分的定位：
- **VPN**：本机是 TUN 一端，对端是远端；用户态程序负责加密封装成 UDP 发出去。
- **容器网络**：CNI 插件常用 TAP + bridge（L2 语义），让容器像接在交换机上。
- **QEMU 网络**：`-netdev tap` 把虚拟机网卡接到宿主机的 TAP 上，
  虚拟机发出的以太网帧直接进入宿主机协议栈（可再桥接出去）。

---

## 5. 本机为什么跑不了（实测证据）

```console
$ ./tun_stack tun0 10.0.0.1 255.255.255.0
open(/dev/net/tun): No such file or directory
提示：需要 1) 内核 tun 驱动已加载（modprobe tun）
      2) 设备节点存在（mknod /dev/net/tun c 10 200）
      3) CAP_NET_ADMIN
      本机实测：/dev/net/tun 不存在且 tun 驱动未加载，故无法运行（见 README）。
$ echo $?
1
```

环境证据（逐条实测）：

```console
$ ls /dev/net/tun
ls: 无法访问 '/dev/net/tun': 没有那个文件或目录

$ ls /sys/class/misc/tun          # 驱动没注册 misc 设备 => 未加载/未内建
ls: 无法访问 '/sys/class/misc/tun': 没有那个文件或目录

$ grep -c tun /proc/misc
0                                 # tun 也不是内建的

$ lsmod | grep tun
(no tun module loaded)

$ modprobe tun
bash: modprobe: 未找到命令        # 注意：这只是当前 shell 的 PATH 里没有 /sbin，
                                  # 并不代表机器上没装 modprobe——见下

$ /sbin/modprobe tun
modprobe: ERROR: could not insert 'tun': Operation not permitted   # 真正的原因是权限（EPERM）
# /sbin/modprobe 是指向 /bin/kmod 的软链，本机确实存在；失败原因是 CapEff=0 + CapBnd=0，
# 加载内核模块需要的是**初始用户命名空间**的 CAP_SYS_MODULE，这是无法用 userns 绕过的能力。

# 即使在非特权 userns 里有了 full caps，也无法创建设备节点：
$ unshare -Urnm sh -c 'mount -t tmpfs none /mnt; mknod /mnt/tun c 10 200'
mknod: /mnt/tun: 不允许的操作      # EPERM

$ unshare -Urn sh -c 'ip tuntap add mode tun dev tun0'
open: No such file or directory    # iproute2 也要打开 /dev/net/tun
ip tuntap add 失败
```

结论（三重障碍，缺一不可）：
1. **没有设备节点** `/dev/net/tun`；
2. **tun 驱动未加载**（既不是模块也不是内建），即使有节点，`open()` 也会因
   没有注册该 major 而失败（`ENODEV`）；
3. **没有权限**：uid=1000、`CapEff=0`、`CapBnd=0`、`NoNewPrivs=1`，
   `sudo` 不可用、`setcap` 无效，`mknod` 在 userns 里也被拒。

> 与 `labs/04` 的对比很有意思：`labs/04` 只依赖 `CAP_NET_RAW`，
> 而该能力在 user namespace 里是**被命名空间化**的，所以能在 `unshare -Urn` 里真跑；
> **加载内核模块**（tun）不是命名空间化的能力，任何非 root 手段都做不到。
> 这就是两者的本质差别。

---

## 6. 在正常机器上运行（root 步骤）

### 6.1 准备 tun 设备

```console
# 加载驱动
$ sudo modprobe tun
$ ls /sys/class/misc/tun          # 出现即成功

# 如果没有 /dev/net/tun，手工创建（主设备号 10，次设备号 200）
$ sudo mkdir -p /dev/net
$ sudo mknod /dev/net/tun c 10 200
$ sudo chmod 600 /dev/net/tun     # 只给 root；需要非 root 用可 666（不推荐）

# 现代发行版通常有 udev 规则自动创建，一般无需手工 mknod
```

### 6.2 运行本 Lab

方式一：让程序自己配置地址（需要 `CAP_NET_ADMIN`）

```console
$ sudo ./tun_stack tun0 10.0.0.1 255.255.255.0
[*] TUN 设备就绪: tun0 (fd=3)
[*] 已配置 tun0 = 10.0.0.1/255.255.255.0
[*] 已置 tun0 UP
[*] 用户态协议栈就绪，等待 tun0 上的 IP 包... (Ctrl-C 退出)
```

方式二：程序只做读写，地址用 `ip` 命令配（等价命令）

```console
# 终端 1
$ sudo ./tun_stack tun0        # tun_configure 失败会提示，但不致命
# 终端 2
$ sudo ip addr add 10.0.0.1/24 dev tun0
$ sudo ip link set tun0 up
```

### 6.3 验证

```console
# 终端 3
$ ping -I tun0 10.0.0.1
PING 10.0.0.1 (10.0.0.1) from 10.0.0.1 tun0: 56(84) bytes of data.
64 bytes from 10.0.0.1: icmp_seq=1 ttl=64 time=0.1 ms
...
```

同时在终端 1 应看到：

```
[<] 收到 84 字节
    IPv4 10.0.0.1 -> 10.0.0.1  proto=1 ttl=64 id=... len=84
    ICMP type=8 code=0
[>] 已回送 84 字节 ICMP Echo Reply（累计 1）
```

> 注意：`ping -I tun0 10.0.0.1` 时源和目的都是 10.0.0.1，
> 包会被路由到 tun0（因为该地址属于 tun0），于是我们的用户态栈收到它。
> 若要模拟「远端」地址，用 `10.0.0.2` 并加一条经 tun0 的路由：
> `sudo ip route add 10.0.0.2/32 dev tun0`，然后 `ping 10.0.0.2`。

### 6.4 可选：UDP 回显

把 `main.c` 里 `IPPROTO_ICMP` 的分支扩展成也处理 `IPPROTO_UDP`
（解析 `struct udphdr`，交换端口，把 payload 原样回送，重算 UDP 校验和），
即可用 `nc -u 10.0.0.2 9999` 测试。UDP 校验和需要 12 字节**伪首部**
（源 IP、目的 IP、0、协议号、UDP 长度），这点务必和 ICMP 区分开。

---

## 7. 在 netns 里安全实验

```bash
# 需要 /dev/net/tun 存在且驱动已加载（这两点是 userns 无法提供的）
sudo unshare -Urn --map-root-user bash -c '
  ip tuntap add mode tun dev tun0     # 或直接让 tun_stack 创建
  ./tun_stack tun0 10.0.0.1 255.255.255.0 &
  sleep 0.5
  ip addr add 10.0.0.1/24 dev tun0 2>/dev/null
  ip link set tun0 up
  ping -I tun0 -c 2 10.0.0.1
  kill %1
'
```

好处：地址、路由、接口全部被限制在 netns 内，**不会影响宿主机网络**，
是调试 VPN/隧道类程序的标准做法。

> 本机实测：`unshare -Urn` 本身可用（见 `labs/04`），但**没有 tun 设备节点与驱动**，
> 所以上面的脚本在本机依然跑不起来 —— 缺的是驱动，不是 namespace 权限。

---

## 8. 协议逻辑正确性：单元自检（本机实测通过）

因为无法端到端运行，我写了一个**一次性自检程序**（跑完即删除），
在不依赖 `/dev/net/tun` 的前提下验证全部协议逻辑。
它编译链接 `checksum.c` / `ip.c` / `icmp.c`，共 17 项断言：

```console
== 1. inet_checksum ==
  [PASS] RFC1071 向量 00 01 f2 03 f4 f5 f6 f7 -> 0x220d
== 2. 构造 IPv4 + ICMP Echo Request，自检其校验和 ==
  [PASS] IPv4 首部回填校验和后整段为 0
  [PASS] ICMP 报文回填校验和后整段为 0
== 3. ipv4_parse ==
  [PASS] 合法包解析成功
  [PASS] hdr_len == 20
  [PASS] payload_len == 16
  [PASS] protocol == IPPROTO_ICMP
  [PASS] 校验和被破坏 -> 解析失败
  [PASS] 截断包 -> 解析失败
== 4. icmp_build_echo_reply ==
  [PASS] 应答长度 == 请求长度
  [PASS] type 8 -> 0 (ECHOREPLY)
  [PASS] code == 0
  [PASS] id 保留 (ping 靠它匹配)
  [PASS] sequence 保留
  [PASS] 负载原样回显
  [PASS] 应答 ICMP 校验和自洽 (整段 == 0)
  [PASS] 应答校验和 != 请求校验和 (确实重算过)

全部通过 (fails=0)
```

其中两项是最有价值的：

1. **RFC 1071 测试向量**：`{00 01 f2 03 f4 f5 f6 f7}` 的校验和必须是 `0x220d`。
2. **自洽不变量**：把校验和回填后，对整段再算一次必须为 `0`。

### 8.1 自检抓到的一个真实 bug（字节序）

第一版自检**失败了**，暴露出一个很隐蔽的 bug：

```c
/* 错误写法：inet_checksum() 返回的是校验和的「数值」，
 * 而 wire 上该字段是大端。直接赋给 uint16_t 字段，在小端机器上
 * 会把两个字节写反，于是内核/对端一律判定校验和错误。 */
hdr->check = inet_checksum(buf, hdr_len);

/* 正确写法 */
hdr->check = htons(inet_checksum(buf, hdr_len));
```

- `inet_checksum()` 内部按**大端**解释缓冲区，返回普通 16 位整数 C；
- 写入报文的 16 位 `checksum` 字段时必须 `htons(C)`，才能让内存里的
  两个字节呈现为大端（= wire 顺序）。

修复后 17 项全部通过。**这个坑与 `labs/04` 的 `raw_send_udp` 是同一个道理**，
也是手写协议栈最常见的错误之一。详见 `checksum.h` 里的约定说明。

---

## 9. 常见陷阱

1. **忘了 `IFF_NO_PI`** —— 不加时 `read()` 得到的**前 4 字节**是内核附加的
   信息头（2 字节 flags + 2 字节 proto，即 `struct tun_pi`），
   你按 IP 首部解析就会全错。表现为「version 字段不是 4」。
2. **校验和字节序**（§8.1）—— 必须 `htons()`。
3. **ICMP 校验和范围** —— 只覆盖 ICMP 报文本身；
   TCP/UDP 还要加 12 字节伪首部，别混。
4. **`id`/`sequence` 必须原样保留** —— 改了 `ping` 会认为不是自己的应答而丢弃。
5. **`tot_len` 是不可信输入** —— 必须同时校验 `>= hdr_len` 和 `<= 实际长度`，
   否则会越界读（本 Lab 的 `ipv4_parse` 两条都查，并有自检覆盖）。
6. **回写时要用同一个 tun fd** —— TUN 是「一个设备一个 fd」；
   写别的 fd 到不了目标设备。
7. **必须重算 IP 首部校验和** —— 交换源/目的地址、改 TTL 后，
   原校验和立刻失效，不改会被内核丢弃。
8. **阻塞 `read` 的唤醒开销** —— 每包一次上下文切换。
   生产级实现会用多队列 TUN、`vhost-net`、`io_uring` 或 XDP 来摊薄这个成本。
9. **`IFF_TUN` vs `IFF_TAP`** —— 选错会导致你读到以太网帧（TAP）
   或裸 IP 包（TUN），解析全错；并且 TAP 才能跑 ARP。

---

## 10. 自测题

<details><summary>1. TUN 与 TAP 的本质区别？</summary>

TUN 是三层设备，用户态读写的是**裸 IP 包**，链路上没有 MAC/ARP；
TAP 是二层设备，读写的是**完整以太网帧**，因此可以做桥接、跑 ARP/DHCP。
TUN 适合点对点 IP 隧道/VPN，TAP 适合虚拟机与容器网络。
</details>

<details><summary>2. 内核发出的包是怎么到达用户态的？</summary>

路由指向 tun0 后，包经 `ip_finish_output2` → `neigh_output` →
`dev_hard_start_xmit` 调用该设备的 `ndo_start_xmit`，也就是
`drivers/net/tun.c` 的 `tun_net_xmit`。这里没有硬件，于是把 skb
挂到设备关联 socket 的接收队列并唤醒用户态，用户态 `read()` 取走。
</details>

<details><summary>3. 用户态 `write()` 到 tun fd 之后，包怎么回到内核协议栈？</summary>

`tun_chr_write_iter` → `tun_get_user()` 把用户态数据包装成 skb，
然后调用 `netif_rx()` 注入
**接收**路径，等价于「这个包刚从 tun0 上收到了」，于是继续走
`ip_rcv` 等正常流程。
</details>

<details><summary>4. 为什么要 `IFF_NO_PI`？不加会怎样？</summary>

不加时内核会在每个包前面附加 2 字节 flags + 2 字节 proto（`struct tun_pi`）。
用户态若不先剥掉这 4 字节，就会把 `tun_pi` 当成 IP 首部解析，
`version` 字段直接是错的。加了 `IFF_NO_PI` 则读写都是裸包。
</details>

<details><summary>5. ICMP Echo Reply 需要改哪些字段？</summary>

IP 首部：源/目的地址互换、TTL 重设（如 64）、重算首部校验和。
ICMP：`type` 由 8 改为 0、`code=0`、`id`/`sequence` 原样保留、
负载原样回显、重算 ICMP 校验和（范围 = ICMP 头 + 数据）。
</details>

<details><summary>6. 为什么 `labs/04` 能在本机跑，而 `labs/05` 不能？</summary>

`labs/04` 只需要 `CAP_NET_RAW`，而该能力在 user namespace 中是被
命名空间化的：`unshare -Urn` 内部能获得完整 capability，因此可以真跑。
`labs/05` 需要 **tun 驱动**和 `/dev/net/tun` 设备节点：
加载内核模块不受 namespace 影响，非 root 无法 `modprobe`，
设备节点也无法在非特权环境下 `mknod`（实测 EPERM）。
</details>

<details><summary>7. 为什么 VPN 用 TUN 而 QEMU 常用 TAP？</summary>

VPN 只需要转发 IP 包（点对点、无需 L2 广播域），TUN 更简单、
没有 ARP/广播开销。QEMU 要让虚拟机像一台真正的主机接入网络
（需要 ARP、DHCP、可桥接、可有自己的 MAC），因此需要 TAP 提供 L2 语义。
</details>

<details><summary>8. TUN 的性能瓶颈在哪？</summary>

每包一次用户态/内核态拷贝 + 一次唤醒（上下文切换）。
此外单队列 TUN 容易成为瓶颈。优化方向：多队列 TUN（`IFF_MULTI_QUEUE`）、
`vhost-net`（把数据面下沉到内核，减少切换）、`io_uring`、
XDP/`AF_XDP` 零拷贝路径。
</details>

---

## 11. 文件清单

| 文件 | 作用 |
|---|---|
| `tun.h` / `tun.c` | 打开 `/dev/net/tun`（`TUNSETIFF`）、配置地址/掩码/UP |
| `ip.h` / `ip.c` | IPv4 首部校验与解析 |
| `icmp.h` / `icmp.c` | ICMP Echo Reply 构造 |
| `checksum.h` / `checksum.c` | RFC 1071 反查和（含字节序约定说明） |
| `main.c` | 事件循环与 IP 首部改写 |
| `Makefile` | `all` / `clean` |

---

## 12. 延伸阅读

- `man 4 tun` —— TUN/TAP 设备与 `TUNSETIFF` 等 ioctl 的权威说明。
- `Documentation/networking/tuntap.rst`（内核文档）。
- 内核实现：`drivers/net/tun.c`（`tun_net_xmit`、`tun_get_user`、
  `tun_chr_read_iter`、`tun_do_read`、`tun_set_iff`）。
  以 6.12 为准，可在 <https://elixir.bootlin.com/linux/v6.12/source/> 核对。
- RFC 792（ICMP）、RFC 1071（Internet 校验和）。
- 对照阅读：WireGuard 的 `tun.c` 数据面、OpenVPN 的 tun/tap 模式、
  以及 `vhost-net` 如何绕过用户态拷贝。
