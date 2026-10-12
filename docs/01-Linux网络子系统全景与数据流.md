# 01 · Linux 网络子系统全景与数据流

> 本章是整个教程的「地图」。后面每一章都是在放大这张图上的某一段。读完本章你应该能回答：
> - 一个 `send()` 的数据包，从用户态到网线，中间到底经过了哪些函数？
> - 一个到达网卡的帧，怎么变成 `recv()` 返回的字节？
> - `sk_buff` 是什么，为什么它是理解一切的关键？
> - 每一层有哪些「可观测点」，出问题时该去哪一层看？

**定位**：横切全栈的总览章。前置知识：会写 socket 程序、了解 TCP/IP 分层。本章不引入新 API，
只建立**结构感**和**索引能力**。

---

## 0. 一张图先看全貌

Linux 网络子系统不是「一个模块」，而是**一组按层次协作的内核子系统 + 一张由函数指针串起来的调用图**。
它的分层不像教科书那么干净，真实情况是：**协议栈是「注册 + 回调」驱动的**，每层向上一层注册自己的处理函数。

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  用户态进程                                                                    │
│    socket() bind() listen() connect() send() recv() sendfile() epoll_wait()   │
└───────────────────────────────┬──────────────────────────────────────────────┘
                                │ 系统调用（glibc 封装 svc）
┌───────────────────────────────▼──────────────────────────────────────────────┐
│ ① 系统调用入口层            net/socket.c                                     │
│    __sys_sendto → sock_sendmsg → sock->ops->sendmsg                          │
│    结构体：struct socket（面向用户态的门面）                                   │
├──────────────────────────────────────────────────────────────────────────────┤
│ ② 协议族层（AF_INET）        net/ipv4/af_inet.c                              │
│    inet_sendmsg → sk->sk_prot->sendmsg                                       │
│    结构体：struct sock（内核协议栈的真正载体）、proto / proto_ops 注册表        │
├──────────────────────────────────────────────────────────────────────────────┤
│ ③ 传输层 TCP/UDP            net/ipv4/tcp*.c / udp.c                          │
│    tcp_sendmsg（组 skb）→ tcp_write_xmit → tcp_transmit_skb（加 TCP 头）       │
│    职责：可靠性、流量控制、拥塞控制、端口复用、状态机                          │
├──────────────────────────────────────────────────────────────────────────────┤
│ ④ 网络层 IP + 路由           net/ipv4/ip_output.c, route.c                    │
│    ip_queue_xmit → ip_local_out → nf_hook(LOCAL_OUT) → ip_output             │
│    → ip_finish_output → neigh_output（查邻居表拿下一跳 MAC）                   │
│    职责：寻址、路由选择、分片、PMTU、ICMP                                     │
├──────────────────────────────────────────────────────────────────────────────┤
│ ⑤ netfilter（横切）          net/netfilter/, net/ipv4/netfilter/             │
│    PRE_ROUTING / LOCAL_IN / FORWARD / LOCAL_OUT / POST_ROUTING               │
│    职责：过滤、NAT、连接跟踪（conntrack）                                      │
├──────────────────────────────────────────────────────────────────────────────┤
│ ⑥ 邻居子系统（L2 地址解析）  net/core/neighbour.c, net/ipv4/arp.c             │
│    职责：IPv4↔MAC 映射（ARP）、NUD 状态机、邻居表缓存与回收                     │
├──────────────────────────────────────────────────────────────────────────────┤
│ ⑦ 队列规则 qdisc / 流量控制  net/sched/                                      │
│    __dev_queue_xmit → qdisc enqueue → __qdisc_run → sch_direct_xmit          │
│    职责：排队、整形、限速、AF_PACKET 旁路                                     │
├──────────────────────────────────────────────────────────────────────────────┤
│ ⑧ 驱动与网卡                 drivers/net/ethernet/**                        │
│    dev_hard_start_xmit → ndo_start_xmit → 填充 TX ring → 网卡 DMA 发出         │
├──────────────────────────────────────────────────────────────────────────────┤
│ ⑨ 软中断 / NAPI（接收侧引擎） net/core/dev.c                                 │
│    硬中断 → napi_schedule → NET_RX_SOFTIRQ → net_rx_action → napi_poll        │
│    → napi_gro_receive → netif_receive_skb → __netif_receive_skb_core         │
├──────────────────────────────────────────────────────────────────────────────┤
│ ⑩ 协议分发（回到 ④ / ⑥）    ptype_base 哈希表 → ip_rcv / arp_rcv             │
│    ip_rcv → ip_rcv_core → nf_hook(PRE_ROUTING) → 路由判定                     │
│      ├─ 本机 → ip_local_deliver → nf_hook(LOCAL_IN) → tcp_v4_rcv             │
│      └─ 转发 → ip_forward → nf_hook(FORWARD) → ip_output → POST_ROUTING      │
├──────────────────────────────────────────────────────────────────────────────┤
│ ⑪ 回到传输层 → 唤醒 socket  net/ipv4/tcp_input.c, net/core/sock.c            │
│    tcp_v4_rcv → tcp_v4_do_rcv → tcp_rcv_established → tcp_data_queue         │
│    → sk->sk_data_ready = sock_def_readable → 唤醒等待队列 → epoll 就绪        │
│    → recvmsg 从 sk_receive_queue 摘 skb → 拷贝到用户缓冲区                    │
└──────────────────────────────────────────────────────────────────────────────┘
```

**记住三个事实**，它们能解释 90% 的「为什么这么设计」：

1. **控制路径是注册/查表，数据路径是函数指针直调。** `struct proto`、`struct proto_ops`、
   `struct packet_type`、`struct net_device_ops`、`struct napi_struct`、`struct nf_hook_ops`
   都是「注册表 + 回调」模式。查一次表，然后一路直调。
2. **`sk_buff` 是唯一的通货。** 所有层都在**同一个 skb** 上做「前移指针加头 / 后移指针剥头」，
   极少拷贝数据本身。理解 skb 的指针语义，就理解了性能特征。
3. **接收侧跑在软中断上下文，发送侧大概率跑在进程上下文。** 这直接决定了两侧
   **能不能睡眠**、**能用什么锁**、**为什么会丢包**。

---

## 1. 核心数据结构：先认脸，再认关系

### 1.1 `struct sk_buff`——协议栈的「数据包对象」

定义在 `include/linux/skbuff.h`，实现在 `net/core/skbuff.c`。它是「一个数据包在网络栈中的全部状态」。

```c
struct sk_buff {
    /* 双向链表：挂在各种队列上（socket 接收队列、qdisc、NAPI backlog…） */
    struct sk_buff      *next, *prev;

    /* 跨层通信的临时字段，当前使用者自己解释 */
    union { struct sock *sk; struct ... };

    /* 定时器，用于 TCP 重传/延迟 ACK 等 */
    /* 注意：skb_shared_info 不是 struct sk_buff 的成员，而是**宏**：
       skb_shinfo(skb) → (struct skb_shared_info *)skb_end_pointer(skb)
       它位于数据缓冲区的尾部（skb->end 之后）。这里为可读性单独列出。 */

    /* ── 指针四兄弟：skb 的核心语义 ── */
    unsigned char *head;   /* 缓冲区起始（分配得到的内存首地址） */
    unsigned char *data;   /* 当前协议头起始（每层加头/剥头时移动） */
    unsigned char *tail;   /* 有效数据末尾 */
    unsigned char *end;    /* 缓冲区末尾 */

    /* ── 长度三兄弟 ── */
    unsigned int len;      /* 当前有效数据长度（不含非线性部分） */
    unsigned int data_len; /* 分片（frag）部分长度，len = 线性 + data_len */
    unsigned int truesize; /* 这块 skb 实际占用的内存（用于 sk_rmem 记账） */

    /* ── 元数据 ── */
    __u16 protocol;        /* L3 协议：ETH_P_IP(0x0800) / ETH_P_ARP(0x0806) … */
    __u8  pkt_type;        /* PACKET_HOST/BROADCAST/MULTICAST/OTHERHOST/OUTGOING */
    __u8  ip_summed;       /* CHECKSUM_NONE/UNNECESSARY/COMPLETE/PARTIAL… */

    __u16 mac_len;         /* L2 头长度 */
    __u16 queue_mapping;   /* 多队列选择结果 */
    __u8  encapsulation;   /* 隧道封装标志 */
    __u8  remcsum_offload;

    struct net_device *dev;    /* 收包/发包的设备 */
    struct dst_entry  *_skb_refdst; /* 路由结果（有引用计数！） */
    void (*destructor)(struct sk_buff *skb);

    /* 控制块：各层私有的临时存储（TCP 用它存 seq/ack/flags） */
    char cb[48] __aligned(8);
    /* shinfo（在数据缓冲区尾部）：
       struct skb_shared_info {
           unsigned char nr_frags;         // 非线性分片数（发送侧是 payload 页）
           __u8 tx_flags;
           unsigned short gso_size;        // GSO/TSO 分段大小
           unsigned short gso_segs;
           struct sk_buff *frag_list;      // skb 链
           skb_frag_t frags[MAX_SKB_FRAGS];// 页分片，零拷贝的关键
           ...
       }; */
};
```

**必须理解的 4 件事**：

| 语义 | 含义 | 典型操作 |
|---|---|---|
| `head/data/tail/end` | 一个「滑动的窗口」 | `skb_push()` 加头（TCP→IP→以太网），`skb_pull()` 剥头（接收方向） |
| `len` vs `data_len` | 线性区长度 vs 页分片长度 | `skb->len` 是真实报文长度；`paged data` 让大包不占连续内存 |
| `truesize` | 真实内存占用 | 决定 `sk_rmem_alloc` / `sk_wmem_alloc` 记账，是「socket 缓冲区满了」的判据 |
| `shinfo->frags[]` | 页分片数组 | 零拷贝、`sendfile`、TSO 的物理基础 |

**skb 的获得与释放**（见 `net/core/skbuff.c`）：

```
分配：alloc_skb / __alloc_skb / netdev_alloc_skb / napi_alloc_skb（NAPI 上下文用，走 per-CPU cache）
克隆：skb_clone()      —— 复制 skb 描述符，共享数据（引用计数），用于多播/抓包
      pskb_copy()      —— 拷贝线性区，共享分片
      skb_copy()       —— 全量深拷贝（昂贵，尽量别用）
释放：kfree_skb() / dev_kfree_skb() / consume_skb()
      → skb_release_all → skb_release_data → skb_release_head_state
      → destructor（TCP 用它做 sk_wmem 归还、unlock socket）
```

> **可观测点**：`kfree_skb` tracepoint（`skb:kfree_skb`）+ `perf`/`bpftrace`，带上 `caller` 字段就能
> 直接看到「谁把包丢了、丢在哪一行」。这是定位丢包最锋利的一把刀，见
> [docs/09-内核网络观测与性能调优.md](09-内核网络观测与性能调优.md)。

### 1.2 `struct socket` 与 `struct sock`——两层门面

- `struct socket`（`include/linux/net.h`）：给 VFS/用户态看的**门面**，持有 `state`、`type`、`flags`、
  `ops`（`proto_ops`，如 `inet_stream_ops`）、`file`、`wq`、指向 `struct sock` 的 `sk`。
- `struct sock`（`include/net/sock.h`）：协议栈**真正的工作对象**。协议族给它套「子类」：
  - `struct inet_sock`（`include/net/inet_sock.h`）：加 inet 地址、端口、TTL 等
  - `struct tcp_sock`（`include/linux/tcp.h`）：加 seq/ack、窗口、拥塞状态、RTT —— 这是全栈最大的结构体之一
  - `struct udp_sock`、`struct raw_sock`、`struct inet_timewait_sock`

```
用户态 fd ──> struct file ──> struct socket ──ops──> proto_ops (inet_stream_ops)
                                    │
                                    └──sk──> struct sock ──> (as tcp_sock) ──sk_prot──> struct proto (tcp_prot)
```

`socket()` 系统调用链：`__sys_socket` → `sock_create` → `__sock_create` → `pf->create` =`inet_create` →
`sock_alloc` + `sk_alloc`（`net/core/sock.c`）→ `sock_init_data` → `sk->sk_prot->init` = `tcp_v4_init_sock`。

> 详细展开见 [docs/02-套接字层与用户态接口.md](02-套接字层与用户态接口.md)。

### 1.3 `struct net_device`——网卡的抽象

`include/linux/netdevice.h`。一个 `net_device` 就是一个网络接口（物理网卡、veth、bridge、tun、lo…）。
关键成员：`name`、`ifindex`、`mtu`、`flags`、`dev_addr`、`netdev_ops`（`ndo_start_xmit` 等）、
`napi_list`、`qdisc`（发送队列规则）、`stats`（`rtnl_link_stats64`）、`features`（GSO/TSO/校验和卸载能力位）。

`lo`、`veth`、`tun` 之所以能当「网卡」用，是因为**它们都实现了 `net_device_ops`**。
这是 Linux 网络虚拟化的根基。

### 1.4 `struct napi_struct`——接收侧的中断缓解

一个 NAPI 实例 = 一个「可以用轮询方式批量收包的实体」。网卡收到第一个包时关中断 + `napi_schedule()`，
软中断里 `napi_poll()` 批量收割 ring 直到 budget 用完，再开中断。

### 1.5 `struct dst_entry`——路由结果

`include/net/dst.h`。`skb->_skb_refdst` 指向它。IPv4 下就是 `struct rtable`（含 `rt_gateway`、`rt_flags`）。
发送时 `dst_output()` 通过它决定走 `ip_output` 还是 `ip_forward`；接收时通过 `RTN_LOCAL` 决定「是不是给我的」。

---

## 2. 发送路径：`send()` 到网线（逐函数）

以「TCP 客户端调用 `write(fd, buf, n)`」为例，忽略中间的分支细节。**左侧是函数，右侧是它做了什么。**

```
①  __sys_sendto / __sys_write            net/socket.c
     └─ 从 fd 找到 struct file → struct socket
②  sock_sendmsg(sock, msg)               net/socket.c
     └─ 安全检查 + sock_sendmsg_nosec
③  sock->ops->sendmsg = inet_sendmsg     net/ipv4/af_inet.c
④  sk->sk_prot->sendmsg = tcp_sendmsg    net/ipv4/tcp.c
     ├─ lock_sock(sk)                     ← 进程上下文，可以睡眠
     ├─ tcp_sendmsg_locked:
     │    ├─ 若 sk_stream_memory_free() 不足 → sk_stream_wait_memory() 睡眠等内存
     │    ├─ 把用户数据分组装进 skb：
     │    │    · 小数据 → 线性区（skb_put 拷进 head 缓冲区）
     │    │    · 大数据 → 页分片（skb_fill_page_desc，零拷贝，MSGMORE 时更明显）
     │    ├─ skb_entail：挂到 sk->sk_write_queue（TCP 的发送队列）
     │    └─ sk_mem_charge / sk_wmem_alloc_set（内存记账，truesize 在这里起作用）
     └─ tcp_push → __tcp_push_pending_frames
⑤  tcp_write_xmit(sk, ...)               net/ipv4/tcp_output.c
     ├─ 发送窗口检查：min(snd_wnd, cwnd, rwnd) 之内才能发
     ├─ Nagle 判定：小包 + 有未确认数据 → 等（除非 TCP_NODELAY）
     ├─ tso_segs = tcp_init_tso_segs()：决定是否需要 GSO 分段
     └─ 循环 tcp_transmit_skb()
⑥  tcp_transmit_skb(sk, skb, ...)        net/ipv4/tcp_output.c
     ├─ skb_clone + tcp_header_size 预留
     ├─ 填 TCP 头：源/目的端口、seq（skb->cb 里带）、ack_seq、窗口、
     │   flags（ACK/PSH/SYN/FIN…）、首部选项（MSS、SACK、时间戳）
     ├─ 写 skb->ip_summed：若网卡支持校验和卸载 → CHECKSUM_PARTIAL
     │                   否则 tcp_v4_send_check 自己算
     └─ icsk->icsk_af_ops->queue_xmit(sk, skb, &inet->cork.fl)
⑦  ip_queue_xmit / __ip_queue_xmit      net/ipv4/ip_output.c
     ├─ 查路由（若没有缓存）：ip_route_output_ports → rt = (struct rtable *)
     ├─ 填 IP 头：版本/IHL、tos、tot_len(typo: tot_len)、id、frag_off(DF)、
     │            ttl、protocol=IPPROTO_TCP、校验和、源/目的地址
     └─ ip_local_out(net, sk, skb)
⑧  __ip_local_out → nf_hook(NF_INET_LOCAL_OUT)   net/ipv4/ip_output.c
     └─ netfilter 的 LOCAL_OUT 钩子（iptables OUTPUT 链就挂这里）
⑨  dst_output → ip_output                net/ipv4/ip_output.c
     └─ 接上 L2 头（若 dst 已有）、POST_ROUTING 钩子、ip_finish_output
⑩  ip_finish_output
     ├─ 超过 MTU 且需要 → ip_fragment()（或交给 GSO 让驱动/网卡分段）
     └─ ip_finish_output2
⑪  ip_finish_output2                     net/ipv4/ip_output.c
     ├─ 有邻居（MAC）→ neigh_output → dev_queue_xmit
     └─ 没有 → 走 neigh_resolve_output：发 ARP 请求，skb 挂进邻居的 arp_queue 等待
⑫  __dev_queue_xmit                      net/core/dev.c
     ├─ 选 txq（多队列）→ qdisc enqueue（默认 pfifo_fast / fq_codel）
     ├─ __qdisc_run → sch_direct_xmit → dev_hard_start_xmit
     └─ 若 qdisc 停用/直发 → 直接 dev_hard_start_xmit
⑬  ndo_start_xmit（驱动实现）             drivers/net/ethernet/**/*.c
     ├─ DMA map skb 数据（dma_map_single / dma_map_page）
     ├─ 填 TX descriptor，写网卡寄存器 tail
     ├─ 队列满 → 返回 NETDEV_TX_BUSY 并 netif_stop_queue()（等 TX 完成中断再唤醒）
     └─ 返回 NETDEV_TX_OK
⑭  网卡 DMA 读出数据 → 上线路；发完后硬中断 → napi 完成队列 → 释放 skb
```

**发送侧的三个关键「刹车点」**（性能问题几乎都在这三者之一）：

| 刹车点 | 判据 | 观测 |
|---|---|---|
| socket 发送缓冲区满 | `sk_wmem_alloc` ≥ `sk_sndbuf` | `ss -tmi` 的 `wmem`、`sk_stream_wait_memory` 栈 |
| TCP 流控/拥塞窗口 | `min(snd_wnd, cwnd)` 受限 | `ss -ti` 的 `cwnd`、`snd_wnd`、`rtt` |
| qdisc/驱动队列满 | `qdisc` backlog、`netif_stop_queue` | `tc -s qdisc`、`ip -s link` TX dropped |

---

## 3. 接收路径：网线到 `recv()`（逐函数）

```
①  网卡 DMA 把帧写进 RX ring → 硬中断（或中断合并后的第一次中断）
②  中断处理程序（上半部）：drivers/net/ethernet/**/*.c
     ├─ 关该队列中断（napi_disable 语义）
     ├─ napi_schedule(&adapter->napi) → __napi_schedule → ____napi_schedule
     │    └─ list_add_tail(&napi->poll_list, &sd->poll_list)
     │       __raise_softirq_irqoff(NET_RX_SOFTIRQ)
     └─ 返回（中断极短，这是 NAPI 的全部意义）
③  软中断 ksoftirqd / do_softirq → net_rx_action      net/core/dev.c
     ├─ budget = netdev_budget（默认 300）、time_limit = netdev_budget_usecs
     ├─ 循环：napi_poll(n, budget)
     │     └─ 驱动 poll 函数：批量取 RX descriptor
     │          ├─ napi_alloc_skb（走 per-CPU cache，比 alloc_skb 便宜）
     │          ├─ dma_unmap + 填 skb（len、data、dev、protocol）
     │          └─ napi_gro_receive(&napi->gro, skb)   ← 尝试合并同流小包
     └─ 预算用完 → 把 napi 重新挂回 poll_list 并触发下一轮（softirq 饥饿的根源）
        若软中断被长时间占用 → ksoftirqd 接手（可被调度，说明「软中断太重」）
④  napi_gro_receive → napi_skb_finish → netif_receive_skb
     （GRO 合并失败就逐个上报；6.x 还有 netif_receive_skb_list 的批量路径）
⑤  __netif_receive_skb → __netif_receive_skb_core      net/core/dev.c
     ├─ 处理 VLAN、ptype、bonding/bridge 等「特殊接收者」：
     │    rx_handler（如 br_handle_frame）优先于协议栈
     ├─ 遍历 ptype_all（AF_PACKET 抓包者，tcpdump 挂这里）
     └─ 按 skb->protocol 查 ptype_base 哈希 → deliver_skb
          → ip_rcv（ETH_P_IP）/ arp_rcv（ETH_P_ARP）/ ipv6_rcv
⑥  ip_rcv → ip_rcv_core                                  net/ipv4/ip_input.c
     ├─ 合法性检查：长度、版本、首部校验和、pkt_type
     └─ NF_HOOK(NF_INET_PRE_ROUTING) → nf_hook_slow
⑦  ip_rcv_finish → ip_rcv_finish_core
     ├─ 若 skb 还没有 dst：ip_route_input_noref / ip_route_input_rcu
     │    → 结果为 RTN_LOCAL（给我）/ RTN_UNICAST（要转发）/ RTN_BROADCAST…
     ├─ 早分流（early demux）：tcp_v4_early_demux / udp_v4_early_demux
     │    在有 listener 缓存时直接定位 sock，省掉一次哈希查找
     └─ dst_input(skb) → 由 dst 决定调 ip_local_deliver 还是 ip_forward
⑧  本地投递：ip_local_deliver → NF_HOOK(NF_INET_LOCAL_IN) → ip_local_deliver_finish
     ├─ 需要分片重组 → ip_defrag()（ip_frag_queue/ip_frag_reasm）
     ├─ 先给 RAW socket：raw_local_deliver
     └─ 查 inet_protos[protocol] → ipprot->handler = tcp_v4_rcv
⑨  tcp_v4_rcv(skb)                                       net/ipv4/tcp_ipv4.c
     ├─ __inet_lookup_skb(inet_hashinfo, skb) —— 五元组查 ehash（已建立）
     │    或 bhash/lhash（LISTEN 状态）
     ├─ 不是给我们的 → 发 RST（tcp_v4_send_reset）
     └─ tcp_v4_do_rcv(sk, skb)
⑩  tcp_v4_do_rcv → tcp_rcv_established（或 tcp_rcv_state_process）  net/ipv4/tcp_input.c
     ├─ 慢路径头部预测失败时 tcp_validate_incoming（seq/ack 合法性、SACK、窗口更新）
     ├─ ACK 处理：tcp_ack → 滑动 snd_una、更新 rtt/cwnd、拥塞控制回调
     ├─ 数据：tcp_data_queue → 顺序数据并入 sk_receive_queue
     │                       乱序数据进 ofo 队列，并可能触发 SACK/快速重传
     └─ 最终 sk->sk_data_ready(sk)  = sock_def_readable     net/core/sock.c
          └─ wake_up_interruptible_sync_poll(&sk->sk_wq ...)
               └─ ep_poll_callback → 把 socket 挂进 epoll 就绪链表
⑪  进程被唤醒 → epoll_wait 返回 → recv() → tcp_recvmsg                net/ipv4/tcp.c
     ├─ 从 sk->sk_receive_queue 摘 skb
     ├─ 拷贝到用户缓冲区（skb_copy_datagram_msg → __skb_datagram_iter）
     ├─ 更新 rcv_nxt、可能发送窗口更新（tcp_send_ack）
     └─ 数据被消费 → sk_rmem_alloc 归还 → 唤醒发送方（sk_data_ready 的反向）
```

> **注意「接收栈在软中断里跑」的后果**：这里**不能睡眠**。所以任何「等一等」的需求都变成了
> 「丢包 / 重传 / 排队」。`/proc/net/softnet_stat` 的 dropped 列、`sk_receive_queue` 溢出丢包，
> 都是这个约束的直接产物。

---

## 4. 两张容易被忽略但极重要的路径

### 4.1 环回（loopback）不走向下再向上

`ping 127.0.0.1` 或本机两个进程互连，走的是 `lo` 设备，**不会真的经过网卡和 NAPI**：

```
ip_finish_output2 → neigh_output（lo 的邻居？无）→ dev_queue_xmit(lo)
  → loopback_xmit（drivers/net/loopback.c）
       skb_orphan(); 设 pkt_type/协议;
       __netif_rx(skb)  // 直接投进 backlog，由软中断继续
  → process_backlog → netif_receive_skb → 后续与真实设备完全一致
```

这对实验很友好：**不碰网卡也能完整走一遍协议栈**，本教程绝大多数实验都基于 `lo` 或 veth。
注意 `lo` 的 MTU 默认 65536，所以环回上**看不到分片**——这是很多「本机测试正常、上线就出问题」的原因。

### 4.2 转发路径

```
ip_rcv_finish_core（路由判定为 RTN_UNICAST 且非本机）
  → ip_forward → NF_HOOK(NF_INET_FORWARD)      ← iptables FORWARD 链
      → ip_forward_finish → dst_output → ip_output
          → NF_HOOK(NF_INET_POST_ROUTING)      ← SNAT/MASQUERADE 在这里生效
              → ip_finish_output → ... → dev_queue_xmit
```
注意：**TTL 递减、ICMP 超时、路径 MTU 的 ICMP 需要**都发生在 `ip_forward` 里。
这也是「装了 Docker/K8s 的机器必须开 `ip_forward`」的原因。

---

## 5. 分层职责对照表（打印出来贴在显示器旁）

| 层 | 入口函数（收） | 出口函数（发） | 核心结构 | 关键源文件 | 观测点 |
|---|---|---|---|---|---|
| 系统调用 | `__sys_recvfrom` | `__sys_sendto` | `socket`/`file` | `net/socket.c` | `strace`、`ss -p` |
| 协议族 | `inet_recvmsg` | `inet_sendmsg` | `sock`/`proto` | `net/ipv4/af_inet.c` | `/proc/net/protocols` |
| TCP | `tcp_v4_rcv` | `tcp_sendmsg` | `tcp_sock` | `net/ipv4/tcp*.c` | `ss -tni`、`/proc/net/tcp` |
| UDP | `udp_rcv` | `udp_sendmsg` | `udp_sock` | `net/ipv4/udp.c` | `ss -un` |
| IP | `ip_rcv` | `ip_queue_xmit` | `sk_buff` | `net/ipv4/ip_input.c`/`ip_output.c` | `netstat -s`、`/proc/net/snmp` |
| 路由 | `ip_route_input_*` | `ip_route_output_*` | `fib_table`/`rtable` | `net/ipv4/route.c` | `ip route`、`/proc/net/fib_trie` |
| 邻居/ARP | `arp_rcv` | `neigh_resolve_output` | `neighbour` | `net/core/neighbour.c`、`net/ipv4/arp.c` | `ip neigh`、`/proc/net/arp` |
| netfilter | `nf_hook_slow` | 同左（钩子） | `nf_hook_ops` | `net/netfilter/*` | `iptables-save`、`/proc/net/nf_conntrack` |
| qdisc | — | `__dev_queue_xmit` | `Qdisc` | `net/sched/*` | `tc -s qdisc` |
| 驱动/NAPI | `napi_poll` | `ndo_start_xmit` | `net_device`/`napi_struct` | `net/core/dev.c`、`drivers/net/**` | `/proc/interrupts`、`/sys/class/net/*/statistics` |
| 软中断 | `net_rx_action` | `net_tx_action` | `softnet_data` | `net/core/dev.c` | `/proc/net/softnet_stat` |

---

## 6. 数据包在各层的「变形」过程

发送方向（每层 `skb_push` 一个头，`data` 指针前移）：

```
应用数据 "GET / HTTP/1.1..."
  │  ← TCP 层：skb_push(20+12)  填 TCP 头（seq/ack/window/flags/options）
  ▼
[TCP][应用数据]
  │  ← IP 层：skb_push(20)      填 IP 头（src/dst/ttl/proto/checksum）
  ▼
[IP][TCP][应用数据]
  │  ← 邻居层确认 MAC；驱动层：
  ▼
[Eth][IP][TCP][应用数据][FCS←网卡硬件补]
```

接收方向完全相反：每层 `skb_pull` 剥头，把 `data` 后移，`skb->protocol`/`skb->pkt_type` 逐层被解释。

**性能含义**：如果所有头都在同一块连续内存里（线性区），加头/剥头就是**移动指针**，
O(1) 且无拷贝。这就是「零拷贝」在协议栈内部的含义——真正的拷贝发生在
**用户态↔内核态边界**（`copy_from_user`/`copy_to_user`）以及**非线性数据聚合**时。

---

## 7. 并发模型：谁在什么上下文里跑

| 路径 | 上下文 | 能睡眠吗 | 锁 |
|---|---|---|---|
| `send()` 入库、组 skb | 进程上下文 | 能（等内存/等窗口） | `lock_sock(sk)` |
| `tcp_transmit_skb` 以下 | 进程上下文（或软中断触发的重传） | 不能（可能已在软中断） | `bh_lock_sock` |
| 接收硬中断 | 硬中断 | 绝对不能 | 无（NAPI 调度） |
| `net_rx_action`/`napi_poll` | 软中断 | 绝对不能 | per-CPU（`sd`）、RCU |
| 路由/邻居查找 | 软中断或进程 | 不能（软中断侧） | RCU（`rcu_read_lock`） |
| conntrack 查表 | 软中断或进程 | 不能 | per-CPU + RCU |
| eBPF/XDP | 驱动 NAPI 上下文 | 不能 | 无锁，用 per-CPU map |

**经验法则**：接收路径上的任何操作必须**有界且短**；需要等待的事，一律「返回失败，让上层重试」。

---

## 8. 本章与后续章节的对应关系

| 你关心的路径段 | 去哪一章 |
|---|---|
| `socket()`/`send()`/`recv()`、epoll 唤醒、sk_buff 与 sock 的关系 | [02 套接字层](02-套接字层与用户态接口.md) |
| TCP 状态机、滑动窗口、拥塞控制、TIME_WAIT、UDP | [03 传输层](03-传输层-TCP与UDP实现.md) |
| IP 收发、路由查找、fib_trie、PMTU、ARP 邻居 | [04 网络层](04-网络层-IP路由与邻居子系统.md) |
| NAPI、softirq、ring buffer、GRO/GSO、qdisc、TUN | [05 链路层与驱动](05-链路层-网卡驱动-NAPI与中断.md) |
| 五个 hook 点、conntrack、NAT、k8s 网络 | [06 netfilter](06-netfilter-conntrack与NAT.md) |
| XDP/tc eBPF 在数据面插在哪、可编程数据面 | [07 eBPF 与 XDP](07-eBPF-XDP与可编程数据面.md) |
| 把 skb 变成 HTTP 请求：应用层解析器与内核的分界 | [08 HTTP 协议解析](08-HTTP协议解析与高性能服务实现.md) |
| 丢包/延迟怎么定位、sysctl 怎么调 | [09 观测与调优](09-内核网络观测与性能调优.md) |
| 想自己读源码从哪下手 | [10 源码阅读地图](10-源码阅读地图与实验手册.md) |

配套实验：`labs/01`（socket 与 TCP_INFO）→ `labs/02`（状态机与队列）→ `labs/03`（netlink 读内核网络配置）
→ `labs/04`（原始套接字/ARP）→ `labs/05`（TUN 与用户态协议栈）→ `labs/06`（内核模块 + netfilter 钩子）
→ `labs/07`（HTTP 解析器）→ `labs/08`（eBPF/XDP）→ `labs/09`（观测工具箱）。

---

## 9. 自测题（能答上再往下走）

1. `skb->data` 和 `skb->head` 的区别是什么？接收一个以太网帧后，`data` 指向哪里？
2. 为什么 `skb->len` 和 `data_len` 是两个字段？`skb->len` 包含 `data_len` 吗？
3. `skb_clone()` 和 `skb_copy()` 对内存的要求有什么本质区别？tcpdump 抓包用的是哪个？
4. 发送路径上第一次可能「睡眠」的地方在哪一层？接收路径上能不能睡眠，为什么？
5. `tcp_transmit_skb` 里调用的是 `ip_queue_xmit`，那 `ip_local_out` 是谁调的？
6. 一个包到达网卡后，`netif_receive_skb` 之前经历了哪些函数？为什么要有 NAPI？
7. `ptype_base` 哈希表的作用是什么？`tcpdump` 是怎么在协议栈之前拿到包的？
8. 环回通信为什么不会触发 NAPI？`lo` 的 MTU 有什么副作用？
9. 转发一个包时，netfilter 的哪几个 hook 会被触发？SNAT 在哪个 hook 生效？
10. `/proc/net/softnet_stat` 的 `dropped` 列涨了，说明丢在哪一层？下一步该看什么？

<details>
<summary>参考答案要点</summary>

1. `head` 是分配到的缓冲区起点且终生不变；`data` 是当前协议头起点，每加/剥一层头就移动。接收后 `data` 指向以太网头（`netif_receive_skb_core` 之后可能已 `skb_pull` 掉 L2）。
2. `len` = 线性区长度 + `data_len`（页分片长度）。分片存在是为了零拷贝与 TSO：应用大块数据可以直接用页引用，不必先拷进连续内存。
3. `skb_clone` 只复制描述符（共享数据，靠 `skb_shared_info` 引用计数），O(1)；`skb_copy` 深拷贝全部数据，O(n)。抓包用 clone。
4. 发送侧第一处睡眠在 `tcp_sendmsg` 的 `sk_stream_wait_memory()`（发送缓冲不足）或等连接建立时；接收侧全程软中断/硬中断，绝不能睡眠，只能丢弃并用 TCP 重传弥补。
5. `ip_queue_xmit()` 内部调 `__ip_queue_xmit()`，后者最后调 `ip_local_out()`；`ip_local_out` 再经 `__ip_local_out` 触发 LOCAL_OUT 钩子后由 `dst_output` 走到 `ip_output`。
6. 硬中断 → `napi_schedule` → 软中断 `net_rx_action` → `napi_poll` → `napi_gro_receive` → `netif_receive_skb`。NAPI 的意义：把「每包一次中断」变成「批量轮询」，避免高速率下的中断活锁（livelock）。
7. `ptype_base` 按 `skb->protocol` 分发到具体 L3 处理函数（`ip_rcv`/`arp_rcv`…）；抓包工具用 `AF_PACKET` 注册到 `ptype_all` 链表，因此在协议处理之前就能拿到 skb（并通常是 clone）。
8. `lo` 是纯软件设备，`loopback_xmit` 直接把 skb 交给 backlog（`netif_rx` 路径），不经过真实中断与驱动轮询。`lo` MTU 65536 意味环回不会分片，也测不出 PMTU 问题。
9. 转发触发 PRE_ROUTING、FORWARD、POST_ROUTING。SNAT/MASQUERADE 在 POST_ROUTING（conntrack 的 NAT 也在 `NF_IP_PRI_NAT_SRC` 优先级）；DNAT 在 PRE_ROUTING。
10. 丢在软中断的 backlog/protocol 处理阶段（`net_rx_action` 预算耗尽或 `input_pkt_queue` 满）。下一步看 `/proc/net/softirqs` 的 NET_RX 增长、`netdev_max_backlog`、以及 `skb:kfree_skb` tracepoint 的 caller。
</details>

---

## 10. 延伸阅读

- **书**：《Understanding Linux Network Internals》(Christian Benvenuti) —— 逐层讲清数据结构与设计动机，虽然版本老但概念不过时；《深入理解 Linux 网络：架构设计与内核实现》；《TCP/IP 详解 卷1》。
- **源码**：`Documentation/networking/` 下的驱动与协议文档（在 elixir.bootlin.com 上按 `v6.12/source/Documentation/networking/` 看）。
- **在线**：elixir.bootlin.com 交叉引用（点函数名看所有调用点，是读内核最快的路径）。
- **工具**：本教程 [labs/09-observability](../labs/09-observability/) 里的一组脚本，边读源码边观测。

> **版本提示**：本教程以 Linux 6.12（Debian 13）为基准。内核网络代码的函数名与文件路径在 6.x 内总体稳定，
> 但具体实现细节会演进（例如 IPv4 路由缓存早在 4.18 就被 FIB trie 直查取代）。
> 遇到对不上的地方，请在 `elixir.bootlin.com/linux/v6.12/source/` 下核对，**不要相信任何写死的行号**。
