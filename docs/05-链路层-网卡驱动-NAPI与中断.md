# 第 5 章 链路层：网卡驱动、NAPI 与中断

## 0. 本章定位

**处于协议栈哪一层**：本章覆盖**链路层与其下**——即
网络设备抽象（`net_device`）、网卡驱动收发、**NAPI**、
中断与软中断（softirq）、GRO/GSO/TSO 卸载、RPS/RFS/XPS、
qdisc 发送队列，以及 TUN/TAP 这种"虚拟网卡"。
它是第 4 章（IP 层）的**下游**：`netif_receive_skb` 之后进入 IP 层，
`ip_finish_output2` 之后进入本章的发送路径。

**前置章节**：
- 第 4 章 `docs/04`（IP 路由、邻居子系统；出口设备与 MTU 从哪来）
- 第 1 章 socket API（发送最终如何落到 `ndo_start_xmit`）

**学完能回答什么问题**：
1. 一个包从网卡中断到进入 IP 层，中间经过哪些函数和软中断？
2. NAPI 为什么是"中断 + 轮询"的混合体？它解决了什么问题？
3. `napi_poll` 的 weight/budget 是什么？`time_squeeze` 说明什么？
4. `netif_rx` 与 `napi_gro_receive` 是两条不同的收包路径吗？
5. GRO/GSO/TSO/checksum offload 分别在哪一端、省了什么开销？
6. RPS/RFS/XPS 与多队列 RSS 有什么区别？如何查与调？
7. 发包为什么要经过 qdisc？`mq`/`noqueue`/`fq_codel`/HTB 各是什么？
8. TUN/TAP 在内核里处于什么位置？为什么它要跨越内核-用户态边界？

---

## 1. 核心概念与设计权衡

### 1.1 `net_device`：统一各种网卡的抽象

Linux 用 **`struct net_device`**（`include/linux/netdevice.h`）
统一表示一切网络接口：物理网卡、veth、bridge、bond、VLAN、
TUN/TAP、loopback……用户态看到的 `eth0`/`wlan0`/`tun0` 都是它。

驱动只需要做两件事：
1. 用 `alloc_netdev` / `alloc_etherdev_mqs` 分配并填充 `net_device`
   （其中大部分行为通过 **`struct net_device_ops`** 函数表提供）；
2. 用 `register_netdevice`（或 `register_netdev`）注册。

**关键回调（`struct net_device_ops`）**：

| 回调 | 何时调用 | 驱动要做什么 |
|---|---|---|
| `ndo_open` | `ip link set up` | 申请中断、初始化 DMA ring、开队列、启动 NAPI |
| `ndo_stop` | `ip link set down` | 关中断、停 NAPI、释放 ring |
| `ndo_start_xmit` | 有包要发 | 把 skb 映射成 DMA、填 TX descriptor、敲门铃（**必须尽快返回，不能睡眠**） |
| `ndo_set_rx_mode` | 组播/混杂模式变化 | 更新网卡的 MAC 过滤表 |
| `ndo_get_stats64` | 读统计 | 返回 64 位计数（`ip -s link` 就是它） |
| `ndo_change_mtu` | `ip link set mtu` | 调整 MTU 并重配缓冲 |

设备间的关系（如 veth 对端、bridge 成员、VLAN 的父设备）通过
`netdev_upper_dev_link` 建立"上层设备"链接，这也是 `ip link show`
里 `master`/`lower_up` 的信息来源。

### 1.2 设计权衡：为什么是 NAPI（中断 + 轮询）

**纯中断的问题**：高速网卡在高 PPS 下会产生**中断风暴**——
CPU 把所有时间花在"进中断、出中断"上（每次中断的上下文保存/恢复
成本固定），真正处理包的预算被耗尽，吞吐反而崩塌（livelock）。

**纯轮询的问题**：空闲时轮询会白白烧 CPU，功耗与延迟都差。

**NAPI（New API）的折中**：

```
第一个包到达  -> 硬件中断
                  └─► napi_schedule()  把该 NAPI 实例挂到当前 CPU 的 poll_list
                        并触发 NET_RX_SOFTIRQ
                             └─► net_rx_action() 在一个"预算"内循环
                                    └─► napi_poll()  一次处理**多个**包（批量）
中断随后被**关闭**（busy-poll 期），直到 poll 结束才重新打开
```

- **中断只用来"叫醒"一次**，剩下的包用轮询批量收 —— 中断次数从
  "每包一次"降到"每轮一次"。
- 每个 NAPI 实例有一个 **weight**（预算，通常 64）：一次 `napi_poll`
  最多处理这么多包，保证单个设备不会独占 CPU。
- 全局还有 `net.core.netdev_budget`（一轮软中断总预算）与
  `net.core.netdev_budget_usecs`（时间上限，防止软中断跑太久）。

### 1.3 设计权衡：backlog 队列与 `netif_rx` 路径

不是所有包都走 NAPI。**非 NAPI 驱动**（以及一些特殊路径）会用
`netif_rx()` 把 skb 放进**每 CPU 的 backlog 队列**
（`softnet_data.input_pkt_queue`），然后触发软中断由
`process_backlog()` 稍后处理。

为什么要排队而不是直接处理？因为 `netif_rx` 可能运行在
**中断上下文**（不能睡眠、不能长时间占用），排队后交给软中断处理
可以让出中断上下文，并起到"削峰"作用。

**代价**：队列满了就**丢包**。这就是 `net.core.netdev_max_backlog`
存在的意义，也是 `/proc/net/softnet_stat` 里 `dropped` 列的来源。

### 1.4 设计权衡：软中断、ksoftirqd 与"饥饿"

软中断（softirq）在两种时机执行：
1. **中断返回时**（`irq_exit` 路径）—— 最快，延迟最低；
2. **`ksoftirqd` 内核线程** —— 当软中断持续产生（负载过高）时，
   内核会让 `ksoftirqd` 接管，避免用户态被永久饿死。

`net_rx_action` 的预算机制：
- 处理满 `netdev_budget`(300) 个包，或
- 耗时超过 `netdev_budget_usecs`(8000 µs)，或
- 单个 NAPI 用完它的 weight

就会**主动让出**，剩余的留给下一次软中断/`ksoftirqd`。
此时 `softnet_data.time_squeeze` 计数 +1 —— 它表示
"这一轮预算用完了但还有活没干完"，是**收包侧过载的最直接信号**。

### 1.5 卸载（offload）：把工作交给硬件

| 技术 | 方向 | 做什么 | 省了什么 |
|---|---|---|---|
| **checksum offload** | 双向 | 网卡计算/校验 L3/L4 校验和 | CPU 逐字节求和 |
| **TSO**（TCP Segmentation Offload） | TX | 协议栈交一个"超大"的 TCP 段给网卡，网卡自己切成 MSS 大小 | 协议栈每包一次的头部构造与调用开销 |
| **GSO**（Generic Segmentation Offload） | TX | **软件**版 TSO：协议栈尽量晚切分；若网卡支持就交给硬件，否则在驱动前用软件切 | 同上，且对不支持 TSO 的设备也有效 |
| **GRO**（Generic Receive Offload） | RX | 收包时把同一流的多个小段**合并**成一个大 skb，再交给协议栈 | 上层处理次数与每包固定开销 |
| **LRO** | RX | 硬件版 GRO（较老，语义受限，多数场景已被 GRO 取代） | 同 GRO |

关键点：**GSO/TSO 发生在发送侧，GRO 发生在接收侧**，二者方向相反、
目的相同（减少每包固定开销）。

### 1.6 `skb->ip_summed` 与 `CHECKSUM_*` 语义

这是最容易混淆的一组状态。它们的含义是
"**这个 skb 的校验和现在处于什么状态**"：

| 值 | 含义 | 谁负责最终校验和 |
|---|---|---|
| `CHECKSUM_NONE` | 校验和已经由软件算好（或无需校验） | 无人，已完成 |
| `CHECKSUM_UNNECESSARY` | 网卡已校验通过，**无需再算** | 无人，已校验 |
| `CHECKSUM_COMPLETE` | 网卡把**整个包的校验和**放在了 `skb->csum` 里 | 上层用它做增量校验 |
| `CHECKSUM_PARTIAL` | **校验和还没算**，但已填好"伪首部"，等硬件算 | 网卡（发送侧 TSO/校验和卸载） |

`skb_shinfo(skb)` 里与分片相关的字段：
`gso_size`（每个分片的负载大小，即 MSS）、`gso_type`
（如 `SKB_GSO_TCPV4`）、`gso_segs`（段数）。
`gso_size != 0` 就表示这是一个"待分片的超大 skb"。

### 1.7 RPS / RFS / XPS / RSS：把收发放到多个 CPU

| 技术 | 层 | 解决的问题 | 手段 |
|---|---|---|---|
| **RSS** | 硬件 | 单队列中断压在一个 CPU 上 | 网卡多队列 + 每个队列独立中断（MSI-X）；按流哈希分发 |
| **RPS** | 软件（RX） | 网卡只有单队列 / 中断集中 | 在软中断里按**流哈希**把 skb 转发到其他 CPU 的 backlog 队列 |
| **RFS** | 软件（RX） | RPS 的哈希与"处理该流的应用所在 CPU"不一致，导致跨 CPU 缓存失效 | 记录"某流最近由哪个 CPU 的应用处理"，让 RPS 优先送到那个 CPU |
| **XPS** | 软件（TX） | 发送时多 CPU 争抢同一个 TX 队列锁 | 为每个 TX 队列指定可用 CPU 集合，让同一 CPU 的发送走同一队列 |

- 多队列的观测点在 `/proc/interrupts`：每个队列一条 `...-TxRx-N` 中断。
- RPS/XPS 的配置在 `/sys/class/net/<dev>/queues/{rx,tx}-N/rps_cpus`、`xps_cpus`。
- 中断亲和性在 `/proc/irq/<n>/smp_affinity`（位图）与
  `smp_affinity_list`（列表，更好读）。
- `irqbalance` 守护进程会自动调整这些亲和性；但它不理解 RPS/RFS 的
  "流-应用同 CPU"意图，高 PPS 场景常需要手工固定。

### 1.8 qdisc：发送侧的排队规则

发送**不是**直接从协议栈到驱动，中间有一层 **qdisc（queueing discipline）**：

```
__dev_queue_xmit
   └─► 选队列 -> qdisc enqueue（入队）
         └─► __qdisc_run -> qdisc_restart -> sch_direct_xmit
               └─► dev_hard_start_xmit
                     └─► ndo_start_xmit（驱动）
```

为什么要有这一层：
1. **流量控制**：HTB/TBF 等实现限速、分层令牌桶（`tc` 命令配置）；
2. **公平与延迟**：`fq_codel` 用 CoDel 算法对抗 bufferbloat，
   是现代发行版的有线默认 qdisc；
3. **批量化**：`sch_direct_xmit` 与 `qdisc_run` 一次可以发出多个包；
4. **设备忙时的缓冲**：网卡队列满时包先入 qdisc，而不是立刻丢弃。

常见 qdisc（`net/sched/`）：

| qdisc | 文件 | 特点 |
|---|---|---|
| `pfifo_fast` | `sch_generic.c` | 传统默认：三个优先级 FIFO 带，简单无状态 |
| `fq_codel` | `sch_fq_codel.c` | 公平队列 + CoDel，抗 bufferbloat，`fq_codel_enqueue`/`fq_codel_dequeue` |
| `mq` | 多队列设备的默认 | 每个硬件 TX 队列一个子 qdisc，**不做整形**（本机 `eth0` 实测为 `mq`） |
| `noqueue` | `sch_generic.c`(`noop_qdisc`) | 表示"不需要排队"，直接发（本机 `wlan0` 与 `lo` 实测为 `noqueue`） |
| HTB | `sch_htb.c` | 分层令牌桶，做带宽保证/限制（`htb_enqueue`/`htb_dequeue`） |

TPACKET/AF_PACKET 与本章的关系：`AF_PACKET` 在**接收**侧的
`__netif_receive_skb_core` 里被分发（`packet_rcv`/`tpacket_rcv`），
在**发送**侧走 `packet_snd` 最终仍进入 `dev_queue_xmit`（见 `labs/04`）。

### 1.9 TUN/TAP 在分层中的位置

TUN/TAP 是**虚拟网卡**：它的 `ndo_start_xmit` 不是写硬件寄存器，
而是把 skb 交给一个用户态进程（`labs/05`）。

```
内核对 -> 用户态：路由出口=tun0 -> ip_finish_output2 -> neigh_output
                    -> dev_hard_start_xmit -> ndo_start_xmit == tun_net_xmit
                         -> 入队 + 唤醒用户态 read()

用户态 -> 内核：用户态 write() -> tun_chr_write_iter -> tun_get_user()
                    -> netif_rx() 重新注入**接收**路径 -> ip_rcv ...
```

`tun_get_user` 把数据包装成 skb 并注入接收路径，是最能说明
"虚拟设备如何假装成一块网卡"的例子。**代价**是每包一次
内存拷贝 + 一次上下文切换，这也是 VPN 性能优化的主战场
（多队列 TUN、`vhost-net`、`io_uring`、XDP）。

---

## 2. 关键内核数据结构（真实 struct 与源码路径）

> 字段以 6.12 为准；凡本章未列细节者，可在
> <https://elixir.bootlin.com/linux/v6.12/source/> 核对。

### 2.1 `struct net_device`（`include/linux/netdevice.h`）

成员极多，按用途分组记忆即可：

| 组别 | 代表成员（概念） | 说明 |
|---|---|---|
| 标识 | `name`、`ifindex`、`dev_addr`、`addr_len`、`type` | `type` 即 ARPHRD_*（Ethernet=1，loopback=772） |
| 配置 | `mtu`、`flags`、`features`、`hw_features`、`wanted_features` | `features` 是 `NETIF_F_*` 位掩码（GRO/GSO/TSO/校验和……） |
| 统计 | `stats`（`struct rtnl_link_stats64`） | `ip -s link` 与 `/sys/class/net/*/statistics/*` 的来源 |
| 操作 | `netdev_ops`（`struct net_device_ops *`） | 驱动回调表 |
| 收发队列 | `num_tx_queues`、`real_num_tx_queues`、`_tx`（`netdev_queue` 数组） | 多队列发送 |
| NAPI | `napi_list` | 挂在本设备上的 NAPI 实例 |
| 队列规则 | `qdisc`、`qdisc_sleeping`、`qdisc_tx_busylock` | 默认/活动 qdisc |
| 关系 | `upper_level`/`lower_level`（由 `netdev_upper_dev_link` 维护） | 拓扑关系 |

`features` 相关的位定义在 `include/linux/netdev_features.h`，
实测确认存在：`NETIF_F_GRO`、`NETIF_F_GSO`、`NETIF_F_TSO`、
`NETIF_F_SG`（scatter-gather）、`NETIF_F_RXCSUM`、`NETIF_F_HW_CSUM`。

### 2.2 `struct net_device_ops`

函数表，驱动按需填充。前文 §1.1 表格中的回调都来自这里，
实测在 `include/linux/netdevice.h` 中存在：`ndo_open`、`ndo_stop`、
`ndo_start_xmit`、`ndo_set_rx_mode`、`ndo_get_stats64`。

### 2.3 `struct napi_struct`

表示一个 NAPI 实例（通常"一个队列一个 NAPI"）。核心语义：
- `poll`：轮询函数指针（驱动实现，内部通常调 `napi_alloc_skb` +
  `napi_gro_receive` 把收到的包上交）；
- `weight`：单次 poll 的预算（默认 64，由
  `netif_napi_add_weight()` 指定，实测存在）；
- `poll_list`：被调度后挂到当前 CPU 的 `softnet_data.poll_list` 上；
- `state`：`NAPI_STATE_SCHED` / `NAPI_STATE_NPSVC` 等状态位。

注册接口：`netif_napi_add()` / `netif_napi_add_weight()`。

### 2.4 `struct softnet_data`（每 CPU 一份）

这是理解收包路径的**枢纽结构**（`include/linux/netdevice.h`），
每 CPU 一个实例：

| 成员（概念） | 作用 |
|---|---|
| `poll_list` | 待轮询的 NAPI 实例链表（由 `napi_schedule` 挂入） |
| `input_pkt_queue` | **backlog 队列**：`netif_rx` 把 skb 放这里 |
| `process_queue` | `process_backlog` 处理时把包先摘下链表 |
| `backlog` | 一个"伪 NAPI 实例"，让 `process_backlog` 复用 NAPI 的预算机制 |
| `output_queue` / `completion_queue` | `net_tx_action` 用于延迟释放 skb 与唤醒发送队列 |
| `processed`、`dropped`、`time_squeeze`、`received_rps`、`flow_limit` | 就是 `/proc/net/softnet_stat` 的数据来源 |
| `cpu` | 该结构所属 CPU |

### 2.5 qdisc 相关

- `struct Qdisc`：qdisc 实例，含 `enqueue`/`dequeue` 函数指针、
  `q`（内部队列）、统计等。
- `struct netdev_queue`：一个 TX 队列，内含
  `qdisc`、`qdisc_state`、发送锁（`_xmit_lock`）、`trans_start` 等。
- `noop_qdisc`：`noqueue` 背后的空实现（`net/sched/sch_generic.c`），
  表示"直接发，不排队"。
- `dev_activate()`：设备 UP 时为其安装默认 qdisc。
- `pfifo_fast`、`__qdisc_run`、`qdisc_run`、`qdisc_restart`、
  `sch_direct_xmit` 都在 `net/sched/sch_generic.c`。

### 2.6 `struct skb_shared_info` 与 offload 字段（`include/linux/skbuff.h`）

`skb_shinfo(skb)` 返回 `skb->head` 末尾的 `struct skb_shared_info`，
它保存"这个 skb 的附加信息"，与 offload 直接相关：

| 字段 | 含义 |
|---|---|
| `gso_size` | GSO 分片的目标负载大小（MSS）；非 0 表示待分片 |
| `gso_type` | 分片类型（`SKB_GSO_TCPV4` 等） |
| `gso_segs` | 段数 |
| `frags[]` / `nr_frags` | scatter-gather 的页片段（配合 `NETIF_F_SG`） |
| `TSO`/`GRO` 相关的 `gso_type` 组合 | 决定由硬件还是软件分片 |

`skb->ip_summed` 取 `CHECKSUM_*`（见 §1.6），
`skb->csum` 则是 `CHECKSUM_COMPLETE`/`CHECKSUM_PARTIAL` 下的校验和载体。

---

## 3. 数据流与函数调用链

### 3.1 接收路径（RX，NAPI 主流路径）

```
[硬件] 收到包 -> DMA 写入 RX ring -> 产生中断
   └─► 驱动中断处理（ISR，上半部）
         ├─ 通常**不**处理数据，只做最少的事
         └─► napi_schedule(&napi)            (net/core/dev.c)
               └─► __napi_schedule
                     ├─ 把 napi 挂到当前 CPU 的 sd->poll_list
                     └─ 触发 NET_RX_SOFTIRQ（raise_softirq_irqoff）

[软中断上下文] NET_RX_SOFTIRQ
   └─► net_rx_action                       (net/core/dev.c)
         │  预算：netdev_budget(300) / netdev_budget_usecs(8000us)
         └─► __napi_poll / napi_poll       （按 weight 轮询，通常 64）
               └─► 驱动的 poll 函数（如 xxx_poll）
                     ├─► napi_alloc_skb      (net/core/skbuff.c)
                     │     （或 netdev_alloc_skb；见 §3.3 说明）
                     ├─ 从 RX ring 取描述符，构造 skb
                     └─► napi_gro_receive    (net/core/dev.c)
                           └─► dev_gro_receive 尝试合并同一流的段（GRO）
                                 └─► netif_receive_skb / netif_receive_skb_list
                                       └─► __netif_receive_skb_core
                                             ├─► AF_PACKET: packet_rcv / tpacket_rcv
                                             └─► ip_rcv          ← 进入第 4 章
```

### 3.2 接收路径（backlog：`netif_rx` / `process_backlog`）

```
netif_rx(skb)                       (net/core/dev.c)
   └─► netif_rx_internal
         └─► enqueue_to_backlog
               ├─ 若 sd->input_pkt_queue 队列长度 >= netdev_max_backlog
               │     -> 丢弃并把 sd->dropped++
               └─► __skb_queue_tail(&sd->input_pkt_queue, skb)
                     └─► napi_schedule(&sd->backlog)  借 NAPI 机制调度

[软中断] net_rx_action -> process_backlog
   └─ 把 sd->input_pkt_queue 移到 sd->process_queue
         └─► __netif_receive_skb(skb) -> __netif_receive_skb_core -> ip_rcv
```

要点：
- `netif_rx` 路径**先入队再处理**，所以队列满就会丢包
  （`/proc/net/softnet_stat` 的 `dropped`）。
- `process_backlog` 复用了 `sd->backlog` 这个"伪 NAPI"，因此
  也受 `weight`/`netdev_budget` 约束，这解释了为什么 backlog
  过载时 `time_squeeze` 也会涨。

### 3.3 关于 `napi_alloc_skb` / `netdev_alloc_skb` 的源码位置

一个常见误解是指它们位于 `include/linux/netdevice.h`。
**实测（6.12）：两者的声明在 `include/linux/skbuff.h`，实现在
`net/core/skbuff.c`**（其中还有 `__netdev_alloc_skb`）。
`napi_alloc_skb` 会优先使用当前 NAPI 的 per-CPU 缓存
（`napi_alloc_cache`），因此**比 `netdev_alloc_skb` 更快**——
这也是 NAPI poll 函数里推荐用它的原因。

### 3.4 发送路径（TX）

```
协议栈（如 ip_finish_output2）-> neigh_output -> dev_queue_xmit
   └─► __dev_queue_xmit                       (net/core/dev.c)
         ├─ 选择 TX 队列（多队列时按 XPS / skb 哈希）
         ├─ 若 qdisc 是 noqueue（noop_qdisc）-> 直接走
         │     __dev_xmit_skb / dev_hard_start_xmit
         └─► qdisc enqueue（如 pfifo_fast_enqueue / fq_codel_enqueue）
               └─► __qdisc_run -> qdisc_run
                     └─► qdisc_restart -> sch_direct_xmit   (net/sched/sch_generic.c)
                           └─► dev_hard_start_xmit           (net/core/dev.c)
                                 └─► ndo_start_xmit（驱动，如 tun_net_xmit）
                                       ├─ 成功：释放 skb 或挂到 completion_queue
                                       └─ 队列满：返回 NETDEV_TX_BUSY，稍后重试

[软中断] NET_TX_SOFTIRQ -> net_tx_action       (net/core/dev.c)
   ├─► 释放 completion_queue 上待回收的 skb（驱动"发送完成"后挂上来的）
   └─► 唤醒之前因队列满而停下的 TX 队列（netif_schedule_queue / netif_tx_wake_queue）
```

**为什么叫 `net_tx_action` 却主要做"释放"**：驱动在发送完成后
不能直接在中断里 `kfree_skb`（可能有锁/上下文限制），于是把 skb
挂到 `sd->completion_queue`，由软中断统一释放。这既降低了中断
处理时间，也批量化了内存释放。

### 3.5 TUN/TAP 的跨界路径（`labs/05` 的原理）

```
内核对 -> 用户态：
  ip_finish_output2 -> neigh_output -> dev_hard_start_xmit
    -> ndo_start_xmit == tun_net_xmit              (drivers/net/tun.c)
         -> 把 skb 放入 tun 设备关联 socket 的接收队列并唤醒
              -> 用户态 read()  -> tun_chr_read_iter -> tun_do_read

用户态 -> 内核：
  用户态 write() -> tun_chr_write_iter -> tun_get_user
    -> 构造 skb -> netif_rx() 注入**接收**路径（`netif_rx_ni()` 在 5.18 已删除，别再引用）
         -> __netif_receive_skb_core -> ip_rcv ...
```

`tun_sendmsg` 用于另一种用法（用户态提供 `msg_control` 指定设备）。
实测这些符号都在 `drivers/net/tun.c`：`tun_net_xmit`、`tun_get_user`、
`tun_chr_read_iter`、`tun_do_read`、`tun_chr_write_iter`、`tun_sendmsg`。

---

## 4. 可观测点与验证方法

> 以下除特别说明外，均为本机（6.12.107，20 vCPU）**实测输出**。

### 4.1 `/proc/net/softnet_stat` —— 最重要但最常被读错的文件

**每行一个 CPU**，列是**十六进制且无表头**。6.12 打印 **15 列**，
远多于大多数文档描述的 10 列。列顺序来自
`softnet_seq_show()`（`net/core/net-procfs.c`）的 `seq_printf`：

| 列 | 来源 | 含义 |
|---|---|---|
| 1 | `sd->processed` | 本 CPU 处理过的包数 |
| 2 | `sd->dropped` | **因 backlog 队列满而丢弃**的包数 |
| 3 | `sd->time_squeeze` | **预算用尽但仍有活**的次数（收包过载信号） |
| 4–8 | 恒 0 | 曾是 `fastroute`，已废弃 |
| 9 | 恒 0 | 曾是 `cpu_collision`，已废弃 |
| 10 | `sd->received_rps` | 被 **RPS** 转发到本 CPU 的包数 |
| 11 | `flow_limit_count` | 触发 **RFS flow limit** 的次数 |
| 12 | `input_qlen + process_qlen` | 当前 backlog 总长度（瞬时值） |
| 13 | `seq->index` | **CPU id**（重要：行号不等于 CPU id，因为离线 CPU 不显示） |
| 14 | `input_qlen` | `input_pkt_queue` 长度（由 `softnet_input_pkt_queue_len()` 计算） |
| 15 | `process_qlen` | `process_queue` 长度（由 `softnet_process_queue_len()` 计算） |

本机实测（截取前 3 行与最后 1 行）：

```
000ac596 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
0034cd46 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000001 00000000 00000000
00174c8c 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000002 00000000 00000000
...
00003ccd 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000013 00000000 00000000
```

实测 `awk '{print NF}'` 确认每行 **15** 个字段；第 13 列从
`00000000` 递增到 `00000013`（即 CPU 0–19），正好印证第 13 列是 CPU id。
本机第 1 列（processed）非 0（如 `0034cd46` = 3,461,446），
其余列几乎全 0，说明**当前没有收包瓶颈**。

**怎么用它做诊断**：

| 症状 | 看哪列 | 结论与对策 |
|---|---|---|
| 丢包 | 第 2 列 `dropped` 增长 | backlog 太小或处理太慢 → 调大 `net.core.netdev_max_backlog`，或启用 RPS 分散到多 CPU |
| 软中断饥饿 | 第 3 列 `time_squeeze` 增长 | 单轮预算内干不完 → 调大 `netdev_budget`/`netdev_budget_usecs`（治标），根治要看是不是中断集中在一个 CPU |
| 只有 1 个 CPU 在干活 | 只有一行第 1 列在涨 | 中断/RPS 没分散 → 配 RPS/XPS 或中断亲和性 |
| RFS 生效 | 第 10 列 `received_rps` | 包被 RPS 转发到该 CPU |

### 4.2 `/proc/interrupts` —— 看多队列与中断分布

本机实测（`eth0` 为 4 队列网卡，`wlan0` 无 MSI-X 队列名）：

```
           CPU0  CPU1 ... CPU19
 125:         0     0 ...     0   IR-PCI-MSIX-0000:01:00.0  0-edge  eth0
 126:         0     0 ...     0   IR-PCI-MSIX-0000:01:00.0  1-edge  eth0-TxRx-0
 ...        ...   ...   ...   ...
 130:         0     0 ... 193246   IR-PCI-MSI-0000:00:14.0   0-edge  xhci_hcd
```

读法：
- `IR-PCI-MSIX-...` 后面的编号是 **MSI-X vector index**；
- `eth0-TxRx-0..3` 说明**这块网卡有 4 个收发队列，每个队列一个独立中断**，
  这就是 **RSS** 的硬件基础；
- 右侧数字是**每个 CPU 上的中断次数**，可以用来判断
  "中断是不是都压在 CPU0 上"（常见性能问题）。

**中断亲和性**（本机实测，`eth0-TxRx-0` 是 IRQ 126）：

```console
$ cat /proc/irq/126/smp_affinity
fffff
$ cat /proc/irq/126/smp_affinity_list
0-19
```

- `smp_affinity` 是**十六进制位图**（`fffff` = 20 位全 1 = CPU 0–19）；
- `smp_affinity_list` 是更好读的列表形式，**推荐用这个**；
- 写入需要 root。多队列网卡通常把每个队列的中断绑到不同 CPU，
  以减少锁竞争与缓存失效（`irqbalance` 会自动做，但策略不一定最优）。

### 4.3 `/proc/softirqs` —— 看 NET_RX/NET_TX 的分布

本机实测（截取）：

```
                    CPU0       CPU1    ...      CPU19
      NET_TX:          0         12    ...          3
      NET_RX:     217001    4941614    ...       6557
```

- **`NET_RX` 分布严重不均**说明收包集中（上例 CPU1 是 CPU0 的 20 多倍）；
- 结合 §4.1 的第 13 列定位到具体 CPU，再决定绑定中断或启用 RPS。

### 4.4 `/sys/class/net/<dev>/statistics/*` 与 `ip -s link`

本机实测 `wlan0` 的统计文件（部分）：

```
rx_packets 3142708   tx_packets 2045042
rx_bytes   1881597119 tx_bytes  1330798818
rx_dropped 1         tx_dropped 7
rx_errors  0         tx_errors  0
```

这些文件对应 `struct rtnl_link_stats64`（由 `ndo_get_stats64` 提供）。
`ip -s link show wlan0` 给出同样的数据但按 RX/TX 分块：

```
4: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP mode DORMANT group default qlen 1000
    link/ether 00:11:22:33:44:05 brd ff:ff:ff:ff:ff:ff
    RX:  bytes packets errors dropped  missed   mcast
    1881914043 3143461      0       1       0       0
    TX:  bytes packets errors dropped carrier collsns
    1331520907 2045770      0       7       0       0
```

注意 `ip -s link` 直接暴露了 **qdisc 类型**：
- `wlan0` → `qdisc noqueue`（无线，无需排队）
- `eth0` → `qdisc mq`（多队列设备，每硬件队列一个子 qdisc）

```
2: eth0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc mq state DOWN ...
```

这是从用户态**直接观察 qdisc 分层**的最快方式。

### 4.5 网卡队列目录

本机实测：

```console
$ ls /sys/class/net/wlan0/queues/
rx-0  tx-0
$ ls /sys/class/net/eth0/queues/     # 多队列网卡
rx-0 rx-1 rx-2 rx-3 tx-0 tx-1 tx-2 tx-3
```

`/sys/class/net/<dev>/queues/rx-N/` 与 `tx-N/` 下的 `rps_cpus`、
`rps_flow_cnt`、`xps_cpus` 就是 RPS/RFS/XPS 的调优入口
（每个文件是一个 CPU 位掩码）。

### 4.6 `ss -i` —— 从连接视角看 MSS/PMTU/cwnd

本机实测（loopback 上的连接，节选）：

```
ESTAB 0 0 127.0.0.1:60140 127.0.0.1:3080
	 cubic wscale:7,7 rto:204 rtt:0.057/0.031 ato:40 mss:36992 pmtu:65535
	 rcvmss:29258 advmss:65483 cwnd:10 bytes_sent:1307 ...
	 segs_out:41708 segs_in:41707 ... minrtt:0.018
```

可读出的信息：
- `mss`/`pmtu`：路径 MTU 的**结果**（PMTU 发现是否生效）；
- `cwnd`/`rtt`/`rto`：拥塞窗口与往返时延；
- `segs_out`/`segs_in`：与 §1.5 的 GSO/GRO 效果对照（**段数** vs 包数）；
- `wscale`：窗口缩放因子。

### 4.7 `ethtool`：本章很多细节需要它（本机未安装）

本机实测：

```console
$ command -v ethtool
（无输出）
$ ethtool
ethtool 未安装 (Debian: sudo apt install ethtool)
```

Debian/Ubuntu 安装方式：

```console
$ sudo apt update && sudo apt install -y ethtool
```

装上后最有用的三条：

| 命令 | 看什么 |
|---|---|
| `ethtool -S <dev>` | 每队列/每原因的详细统计（比 `/sys/.../statistics` 细得多），定位 RX/TX 丢在哪个环节 |
| `ethtool -C <dev>` | **中断合并**参数（`rx-usecs`、`rx-frames`、`tx-usecs` 等），延迟 vs 吞吐的旋钮 |
| `ethtool -G <dev>` | ring buffer 大小（`rx`/`tx` 描述符数量），太小会丢包 |
| `ethtool -k <dev>` | 各 offload 开关状态（gro/gso/tso/rx-checksumming…），与 §1.5 对应 |
| `ethtool -l <dev>` | 队列数量（RSS 通道数） |

> 本机没有 `ethtool`，但 `ip -s link`、`/sys/class/net/*/statistics/*`、
> `/proc/net/softnet_stat` 已能覆盖大部分诊断需求 —— 这也是本 Lab
> 强调这几个文件的原因。

---

## 5. 用户态 API ↔ 内核实现对应表

| 用户态手段 | 内核入口 | 关键结构/文件 | 说明 |
|---|---|---|---|
| `ip link set up/down` | `dev_open`/`dev_close` -> `ndo_open`/`ndo_stop` | `net_device`, `net/core/dev.c` | 打开时 `dev_activate()` 安装 qdisc、启动 NAPI |
| `ip link show` | `rtnl_dump_ifinfo` | `net_device`, `net/core/rtnetlink.c` | netlink `RTM_GETLINK`（`labs/03`） |
| `ip -s link` | `ndo_get_stats64` / `dev_get_stats` | `rtnl_link_stats64` | 数据源同 sysfs statistics |
| `/sys/class/net/*/statistics/*` | 同上 | `net_device.stats` | sysfs 逐项暴露 |
| `tc qdisc add ...` | `tc_modify_qdisc` | `Qdisc`, `net/sched/` | 配置 qdisc |
| `tc class/filter` | `tc_ctl_tclass` / `tc_ctl_tfilter` | HTB 等 | 流量分类与整形 |
| `tc qdisc show` | `tc_dump_qdisc` | `Qdisc` | 与 `ip -s link` 里的 qdisc 名对应 |
| 收发数据（任何 socket） | `ndo_start_xmit` / NAPI `poll` | 驱动 | 数据面 |
| `AF_PACKET` 抓包 | `packet_rcv` / `tpacket_rcv` | `net/packet/af_packet.c` | 在 `__netif_receive_skb_core` 分发（`labs/04`） |
| `AF_PACKET` 发包 | `packet_snd` -> `dev_queue_xmit` | 同上 | 绕过协议栈但仍进 qdisc |
| `/dev/net/tun` + `TUNSETIFF` | `tun_set_iff` | `drivers/net/tun.c` | 创建虚拟网卡（`labs/05`） |
| `read`/`write` tun fd | `tun_chr_read_iter`/`tun_get_user` | 同上 | 跨内核-用户态搬 skb |
| `ethtool -S/-C/-G` | ethtool 的 netlink/ioctl 接口 | 驱动 | **本机未安装**（§4.7 有安装命令） |
| `sysctl net.core.netdev_budget` 等 | `net/core/sysctl_net_core.c` | `softnet_data` | 软中断预算 |
| `/proc/net/softnet_stat` | `softnet_seq_show` | `softnet_data` | 15 列，见 §4.1 |
| `/proc/interrupts` | 通用中断子系统 | — | MSI-X 队列名在此可见 |
| `/proc/softirqs` | 通用软中断子系统 | — | `NET_RX`/`NET_TX` 分布 |

---

## 6. 常见陷阱、调优旋钮、自测题

### 6.1 常见陷阱

1. **把 `softnet_stat` 当 10 列读**：6.12 是 **15 列**，第 13 列才是
   CPU id，第 12 列是瞬时队列长度。按老文档的列序解读会得出错误结论。
2. **`dropped` 与 `time_squeeze` 混淆**：
   `dropped` = 队列满**真的丢了**；`time_squeeze` = 预算用尽
   **还没来得及处理**（未必丢）。前者要防丢包，后者要看是否饥饿。
3. **以为 `netif_rx` 和 NAPI 是同一条路**：`netif_rx` 走
   backlog 队列（先排队后处理），NAPI 是中断触发后**直接轮询**驱动队列。
   两者最终都汇入 `__netif_receive_skb_core`，但前半段完全不同。
4. **`napi_alloc_skb` 的位置记错**：它在 `net/core/skbuff.c`
   （声明在 `include/linux/skbuff.h`），**不在** `netdevice.h`。
5. **在 `ndo_start_xmit` 里睡眠或长时间阻塞**：它可能运行在
   软中断/原子上下文，**不能睡眠**。返回 `NETDEV_TX_BUSY` 表示
   "发不出去，稍后重试"。
6. **混淆 GRO 与 GSO 的方向**：GRO 在**接收**（合并），
   GSO/TSO 在**发送**（延迟分片）。
7. **以为 `CHECKSUM_PARTIAL` 是"已校验"**：恰恰相反，
   它表示校验和**还没算**，等硬件补上。
8. **忘了 RPS 会引入跨 CPU 开销**：RPS 把包搬到别的 CPU 会增加
   缓存失效与 IPI（`received_rps` 可观测）。若网卡本身多队列，
   优先用 RSS 而不是 RPS。
9. **只调大 `netdev_max_backlog` 治丢包**：如果瓶颈是"处理太慢"
   而非"突发太大"，加大队列只会增加延迟（bufferbloat），
   应优先分散中断/RPS 或减少每包开销。
10. **`ip -s link` 里的 qdisc 被忽略**：`qdisc mq`/`noqueue`/`fq_codel`
    是判断发送路径行为的直接线索。
11. **TUN 忘记 `IFF_NO_PI` 导致解析错位**（详见 `labs/05`）。

### 6.2 调优旋钮（本机实测默认值一并列出）

| 旋钮 | 本机默认 | 作用与调整建议 |
|---|---|---|
| `net.core.netdev_budget` | **300** | 一轮软中断最多处理多少包；收包 PPS 极高时可适当调大 |
| `net.core.netdev_budget_usecs` | **8000** | 一轮软中断时间上限（µs）；与上者共同决定让出时机 |
| `net.core.netdev_max_backlog` | **1000** | backlog 队列长度上限；`softnet_stat.dropped` 增长时优先考虑调大 |
| `net.core.dev_weight` | **64** | NAPI 单次 poll 预算默认值（`netif_napi_add_weight` 的默认） |
| `net.core.busy_poll` | **0** | `SO_BUSY_POLL` 相关：让 `poll()`/`recvmsg()` 在 NIC 队列上**忙等**，换取更低延迟（代价是 CPU 占用）。对应 `napi_busy_loop()` |
| `net.core.busy_read` | **0** | 同上，作用于 `read()` 语义 |
| `/sys/class/net/<dev>/queues/rx-N/rps_cpus` | — | RPS：允许哪些 CPU 处理该队列的包（位掩码） |
| `/sys/class/net/<dev>/queues/rx-N/rps_flow_cnt` | — | RFS：该队列的流表大小 |
| `/sys/class/net/<dev>/queues/tx-N/xps_cpus` | — | XPS：该 TX 队列由哪些 CPU 使用 |
| `/proc/irq/<n>/smp_affinity_list` | 本机 irq126 = `0-19` | 中断亲和性；多队列网卡建议每队列绑不同 CPU |
| `ethtool -C <dev> rx-usecs/rx-frames` | —（需安装 ethtool） | 中断合并：调大换吞吐，调小换延迟 |
| `tc qdisc replace dev eth0 root fq_codel` | 有线默认常为 fq_codel；本机 wlan0=`noqueue`、eth0=`mq` | 抗 bufferbloat；`tc` 本机未安装 |
| `net.ipv4.tcp_mtu_probing`（第 4 章相关） | **0** | PMTU 黑洞探测 |

### 6.3 面试 / 自测题（≥8）

<details><summary>1. 从网卡中断到 ip_rcv，完整调用链是什么？</summary>

硬件中断 → 驱动 ISR（做最少的事）→ `napi_schedule()` →
`__napi_schedule`（挂到本 CPU `sd->poll_list`，触发 `NET_RX_SOFTIRQ`）
→ 软中断 `net_rx_action`（受 `netdev_budget`/`netdev_budget_usecs` 约束）
→ `__napi_poll`/`napi_poll`（受 NAPI `weight` 约束）→ 驱动 poll 函数
→ `napi_alloc_skb` 构造 skb → `napi_gro_receive`（GRO 合并）
→ `netif_receive_skb` → `__netif_receive_skb_core`
（此处给 `AF_PACKET` 的 `packet_rcv`/`tpacket_rcv` 一份）
→ `ip_rcv`（进入 IP 层）。
</details>

<details><summary>2. NAPI 解决了什么问题？为什么不是纯轮询？</summary>

纯中断在高 PPS 下会中断风暴（livelock）：CPU 全耗在进出中断的固定开销上，
吞吐崩塌。纯轮询在空闲时浪费 CPU。NAPI 用"中断唤醒 + 批量轮询"：
第一个包触发中断并关闭该队列中断，然后在软中断里按 weight 批量收包，
收完再开中断。这样中断次数与包数解耦，兼顾吞吐与空闲效率。
</details>

<details><summary>4. `netif_rx` 与 `napi_gro_receive` 有什么区别？</summary>

`napi_gro_receive` 是 **NAPI 驱动**在 poll 里上交 skb 的入口，
会先尝试 GRO 合并再进协议栈。`netif_rx` 用于**非 NAPI** 或特殊路径，
它把 skb 放入每 CPU 的 `input_pkt_queue`（backlog），
触发软中断后由 `process_backlog` 取出、再经
`__netif_receive_skb` 进入协议栈。前者不排队（除 GRO 合并），
后者会排队且**队列满就丢**（`softnet_stat` 的 `dropped`）。
</details>

<details><summary>7. RSS、RPS、RFS、XPS 各自解决什么问题？</summary>

RSS（硬件）：网卡多队列 + 每队列独立 MSI-X 中断，按流哈希分散到多 CPU。
RPS（软件 RX）：单队列网卡时，在软中断里按流哈希把 skb 转给其他 CPU。
RFS（软件 RX）：在 RPS 基础上记录"该流由哪个 CPU 的应用处理"，
把包送到同一 CPU 以提升缓存命中。
XPS（软件 TX）：为 TX 队列指定 CPU 集合，减少多 CPU 争抢同一发送队列锁。
本机 `eth0` 的 `eth0-TxRx-0..3` 四条 MSI-X 中断就是 RSS 的体现。
</details>

<details><summary>8. 发包为什么必须经过 qdisc？</summary>

qdisc 提供排队与调度能力：流量整形（HTB/TBF）、抗 bufferbloat
（fq_codel 的 CoDel）、多队列分流（mq）、以及设备忙时的缓冲。
同时 `__qdisc_run`/`sch_direct_xmit` 支持一次批量发送多个包，
降低每次发送的固定开销。`noqueue`（`noop_qdisc`）表示该设备
不需要排队，直接发送。
</details>

<details><summary>9. `/proc/net/softnet_stat` 一共几列？第 13 列是什么？</summary>

6.12 是 **15 列**。第 13 列是 **CPU id**（`seq->index`）——
因为离线 CPU 不显示，**行号不等于 CPU id**，必须用这一列对应到 CPU。
第 12 列是 `input_qlen + process_qlen`（瞬时 backlog 长度）。
本机实测第 13 列从 0 递增到 0x13(=19)，共 20 个在线 CPU。
</details>

<details><summary>10. TUN/TAP 在收发路径上的位置？为什么要跨内核-用户态？</summary>

TUN/TAP 是虚拟网卡：发送时协议栈走到
`dev_hard_start_xmit` → `ndo_start_xmit == tun_net_xmit`，
没有硬件，于是把 skb 放进关联 socket 的队列并唤醒用户态；
用户态 `write()` 经 `tun_get_user` 把数据包成 skb 并用 `netif_rx()`
重新注入**接收**路径。因为用户态要做隧道封装/解封装（VPN）
或实现完整协议栈，所以必须把 skb 在两个世界间搬运，
代价是每包一次拷贝 + 一次上下文切换（见 `labs/05`）。
</details>

<details><summary>11. 收包丢包了，你怎么定位？</summary>

按顺序看：
1. `/proc/net/softnet_stat` 第 2 列 `dropped` 是否增长 → backlog 满；
2. 第 3 列 `time_squeeze` 是否增长 → 预算不足/软中断饥饿；
3. `/proc/softirqs` 的 `NET_RX` 是否集中在一个 CPU；
4. `/proc/interrupts` 看中断是否压在 CPU0；
5. `/sys/class/net/*/statistics/rx_dropped`、`rx_missed_errors`、
   `rx_fifo_errors`（有 ethtool 则 `ethtool -S`）判断是驱动 ring 满
   还是协议栈丢；
6. 对症：调 `netdev_max_backlog`、配 RPS/XPS、绑中断亲和性、
   调大 ring（`ethtool -G`）、调中断合并（`ethtool -C`）。
</details>

---

## 7. 本章小结 + 延伸阅读

### 小结

- **`net_device` + `net_device_ops`** 是"一切皆网卡"的抽象；
  驱动只需填回调，尤其 `ndo_open`/`ndo_stop`/`ndo_start_xmit`。
- **NAPI = 中断唤醒 + 批量轮询**，用 `weight`/`netdev_budget`/
  `netdev_budget_usecs` 三级预算防止单一设备或软中断独占 CPU。
- **两条收包路径**：NAPI（`napi_gro_receive`）与 backlog
  （`netif_rx` → `input_pkt_queue` → `process_backlog`），
  后者队列满即丢包，是 `softnet_stat.dropped` 的来源。
- **发送必经 qdisc**：`__dev_queue_xmit` → enqueue →
  `__qdisc_run` → `sch_direct_xmit` → `dev_hard_start_xmit` →
  `ndo_start_xmit`；回收与唤醒由 `NET_TX_SOFTIRQ`/`net_tx_action` 完成。
- **卸载技术方向相反、目的一致**：GRO 收侧合并，GSO/TSO 发侧延迟分片；
  `skb->ip_summed` 的 `CHECKSUM_*` 描述校验和当前状态。
- **`/proc/net/softnet_stat` 在 6.12 是 15 列**，第 13 列是 CPU id ——
  这一点与绝大多数二手资料不同，务必以源码为准。

### 延伸阅读

- 内核源码（6.12）：收发核心与 NAPI 在 `net/core/dev.c`；结构定义在
  `include/linux/netdevice.h`、`netdev_features.h`、`skbuff.h`；
  skb 分配在 `net/core/skbuff.c`；qdisc 在 `net/sched/sch_generic.c`、
  `sch_fq_codel.c`、`sch_htb.c`；软中断预算旋钮在
  `net/core/sysctl_net_core.c`；softnet_stat 在 `net/core/net-procfs.c`；
  AF_PACKET 在 `net/packet/af_packet.c`；TUN/TAP 在 `drivers/net/tun.c`；
  TCP offload 在 `net/ipv4/tcp_offload.c`。
  核对入口：<https://elixir.bootlin.com/linux/v6.12/source/>
- 内核文档：`Documentation/networking/`（含 `scaling.rst` 讲 RSS/RPS/RFS/XPS，
  `segmentation-offloads.rst` 讲 GSO/TSO，`tuntap.rst`）。
- 工具：`ethtool`（本机需 `apt install ethtool`）、`tc`、`nstat`、
  `perf`/`trace-cmd`（tracepoint `net:net_dev_xmit`）。
- 教程内相关章节：`docs/04`（IP 层如何调用 `neigh_output`/`dev_queue_xmit`）、
  `labs/03`（netlink dump 出 net_device 属性）、
  `labs/04`（AF_PACKET 在 `__netif_receive_skb_core` 的旁路）、
  `labs/05`（TUN 上的用户态协议栈）。
