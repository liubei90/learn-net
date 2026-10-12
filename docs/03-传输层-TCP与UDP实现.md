# 第 03 章 传输层：TCP 与 UDP 实现

> 基准内核：Linux 6.12（本机 `6.12.107+deb13-amd64`，Debian 13）。
> 本章的 struct 成员名已在本机 `linux-headers-6.12.107+deb13-common` 中核对；
> 函数名取自 Linux 6.12 源码树（`net/ipv4/tcp*.c`、`net/ipv4/udp.c`、
> `net/ipv4/inet_*.c`、`net/ipv4/syncookies.c`）。
> **不给出任何行号**——行号在内核版本之间会漂移；请用函数名在
> <https://elixir.bootlin.com/linux/v6.12/source/> 检索。
> 标注「本机不可运行」的实验均已在本机实测确认受限原因。

---

## 0. 本章定位

- **协议栈层次**：第 02 章讲完了「用户态 → `struct sock`」的接口层；
  本章进入 `net/ipv4/`，讲 TCP/UDP 这两个传输协议控制块内部的
  **状态机、队列、窗口、定时器、拥塞控制**，以及它们与 IP 层/网卡特性的配合。
- **前置章节**：第 01 章（sk_buff、协议栈分层、NAPI 概览）、
  第 02 章（`struct socket`/`struct sock` 双层抽象、`sock_init_data()`、
  `tcp_sendmsg`/`tcp_recvmsg` 的用户态入口、`sk_receive_queue` 与内存记账）。
- **学完能回答什么问题**：
  1. 服务端 `listen()` 之后，SYN 到达时内核分配了什么？为什么会有
     「SYN 队列」和「accept 队列」两个队列？
  2. `ss -lnt` 的 `Recv-Q`/`Send-Q` 分别对应内核里哪个字段？
  3. 一个 `ACCEPT` 回来之前，三次握手到底走到哪一步了？
  4. `tcp_sendmsg()` 到 `ip_queue_xmit()` 之间发生了什么？
     `snd_una/snd_nxt/snd_wnd/snd_cwnd` 各自怎么推进？
  5. 乱序报文去哪里了？`tcp_ofo_queue()` 与 SACK 如何配合？
  6. cwnd 是谁在改？`tcp_cubic` 和 `tcp_bbr` 的插入点在哪？
  7. RTO 怎么算出来的？重传定时器、延迟 ACK 定时器、keepalive 定时器分别何时触发？
  8. TIME_WAIT 为什么要存在？`tcp_tw_reuse`、`SO_REUSEADDR`、`SO_REUSEPORT`
     在内核里是三件不同的事，具体差别是什么？
  9. Nagle 与延迟 ACK 为什么会互相伤害？`TCP_NODELAY` 关掉的是哪一个？
  10. TSO/GSO/GRO 与 TCP 如何配合？为什么 `tcp_info.tcpi_snd_mss` 可能是 64 KB？
  11. UDP 的 `sk_receive_queue` 满了会怎样？丢包计数在哪里看？
  12. `inet_hashinfo` 的 ehash/bhash/lhash 各查什么？一次收包要走几次查找？

---

## 1. 核心概念与设计权衡

### 1.1 TCP 控制块的三层继承

```
struct sock                （通用：队列、内存、等待队列、回调）
   └── struct inet_sock     （地址族：daddr/saddr/dport/sport、tos、ttl、cork）
          └── struct inet_connection_sock
                 （连接导向：icsk_accept_queue、重传/延迟ACK 定时器、
                   icsk_rto、icsk_ca_ops、icsk_af_ops、icsk_ack、拥塞状态）
                    └── struct tcp_sock
                           （TCP 专有：序号、窗口、cwnd、ofo 队列、RTT 估计、
                             SACK 状态、Nagle 状态）
```

首成员链式嵌套意味着任意方向的强制转换都是「零开销」：
`tcp_sk(sk)`、`inet_csk(sk)`、`inet_sk(sk)` 都是 `container_of` 式的宏/inline 转换。
内核大量使用这种「C 语言里的继承」来共享 `struct sock` 的通用设施。

### 1.2 两个队列：SYN 队列与 accept 队列

服务端 `listen()` 之后，同一时刻可能有成百上千个「握了一半」的连接。
内核用**两个阶段**分离它们：

| 队列 | 内核表示 | 进入条件 | 上限 | 观测 |
| --- | --- | --- | --- | --- |
| SYN 队列（半连接） | `request_sock`，挂在 `icsk_accept_queue` 的 hash/`qlen` 里 | 收到 SYN，回 SYN+ACK 后 | 判定函数 `inet_csk_reqsk_queue_is_full()` → `icsk_accept_queue.qlen > sk_max_ack_backlog`；`sk_max_ack_backlog = min(listen(backlog), net.core.somaxconn)` | `ss -lnt` 不直接显示半连接数（它只数 accept 队列）；看 `TcpExt: ListenDrops`、`SyncookiesSent`、`TCPReqQFullDrop` |
| accept 队列（全连接） | `request_sock`，挂在 `rskq_accept_head/tail` 链表 | 收到第三次握手的 ACK | 同一个 `sk_max_ack_backlog`；判定函数 `sk_acceptq_is_full()` → `sk_ack_backlog > sk_max_ack_backlog` | `ss -lnt` 的 **Recv-Q**（`sk_ack_backlog`）/ **Send-Q**（`sk_max_ack_backlog`） |

> **实测校准（重要）**：`listen(1)` 时 `ss -lnt` 显示 `Send-Q=1`，
> 但 accept 队列能容纳到「队满 + 1」为止 —— 因为 `sk_acceptq_is_full()`
> 用的是 `sk_ack_backlog > sk_max_ack_backlog`（严格大于），
> 且 `tcp_check_req()` 的 `queue->rskq_accept_head != NULL` 条件允许
> 队空时再进一个。本 lab 的 `backlog_probe --backlog 1` 实测
> `Recv-Q=2`、服务端共 accept 到 2 条连接，正对应这个语义。
> 另外 `net.ipv4.tcp_max_syn_backlog`（本机 2048）并不是上面两个
> `is_full` 判定的直接上限：它主要在 SYN cookie 关闭时参与
> `tcp_conn_request()` 的溢出决策，以及决定是否启用 cookie。

**权衡**：SYN 队列防止 SYN flood 快速耗尽内存（配合 SYN cookie 可以完全不占内存）；
accept 队列防止「应用 accept 太慢」把内核内存吃光。
两个队列都满时的行为不同：SYN 队列满 → 默认静默丢弃 SYN（客户端会重传），
`net.ipv4.tcp_syncookies=1` 时改用 cookie 不占队列；
accept 队列满 → 默认**丢弃第三次握手的 ACK**（`net.ipv4.tcp_abort_on_overflow=0`），
客户端此时状态已是 ESTABLISHED，会重传 ACK，服务端也会重传 SYN+ACK。
本机实测（`labs/02-tcp-state-machine`，`syncookies=1`、`abort_on_overflow=0`）：
`listen(1)` + 24 个并发连接时，客户端侧 `connect()` 有 2 个成功、
22 个因 800 ms 超时放弃，而 `TcpExt: ListenOverflows` / `ListenDrops` 各增加 22 ——
**没有回 RST，客户端在超时前一直以为自己连上了**。

### 1.3 滑动窗口与拥塞窗口的职责划分

- **接收窗口 `rcv_wnd`**：由接收方的**内存**决定（`sk_rcvbuf` 与已排队字节），
  通过 TCP 头部的 window 字段通告对端 → 对端记录为 `snd_wnd`。
  它表达「我还能收多少」。
- **拥塞窗口 `snd_cwnd`**：由发送方的**拥塞控制算法**根据 ACK 与丢包信号决定。
  它表达「网络还能承受多少」。c 单位是 MSS。
- **实际可发量** = `min(snd_cwnd, snd_wnd)`，且受 `packets_out` 与
  `snd_una + snd_wnd`（窗口右沿 `tcp_wnd_end()`）约束。

两套窗口独立演化，是 TCP 能在「接收方慢」与「网络堵」两种不同瓶颈下都保持稳定的关键。

### 1.4 队列纪律：Nagle、延迟 ACK、TSQ

- **发送侧 Nagle**（`tp->nonagle`）：有未确认的小包时不发新的小包，
  攒到 MSS 或收到 ACK 再发。目的是减少小包数量，代价是增加延迟。
- **接收侧延迟 ACK**（`icsk_ack.pending`）：收到数据后不立刻回 ACK，
  等约 40 ms 或收到第二个包再回，目的是搭车省包。
- 两者**叠加**会产生经典病态：Nagle 等 ACK，延迟 ACK 等第二个包 → 最多 ~40 ms 停顿。
  交互式应用必须二选一：要么 `TCP_NODELAY`，要么不用 Nagle 场景。
- **TSQ（TCP Small Queues）**：`tcp_small_queue_check()` 限制「每个 socket 在
  qdisc/驱动队列里未发送的字节数」，避免 bufferbloat 与延迟抖动。
  由 `net.ipv4.tcp_limit_output_bytes` 与 pacing rate 共同决定。

### 1.5 拥塞控制的可插拔设计

`struct tcp_congestion_ops`（`include/net/tcp.h`）定义了算法接口，
每个算法是一个模块（可编译进内核或 `modprobe`）。典型回调：

| 回调 | 时机 | 作用 |
| --- | --- | --- |
| `ssthresh(sk)` | 进入 Loss/Recovery | 计算新的慢启动阈值 |
| `cong_avoid(sk, ack, acked)` | 收到确认 | 慢启动 / 拥塞避免的 cwnd 演化 |
| `cong_control(sk, ack, flag, rs)` | 收到 ACK（可选） | 完全接管 cwnd 与 pacing（BBR 用它） |
| `set_state(sk, new_state)` | `tcp_set_ca_state()` | 通知状态迁移（如进入 Recovery） |
| `cwnd_event(sk, ev)` | 各类 CA 事件 | CA_EVENT_LOSS / CA_EVENT_TX_START / CA_EVENT_CWND_RESTART … |
| `undo_cwnd(sk)` | 撤销拥塞响应 | 处理虚假快速重传（spurious retransmit） |
| `pkts_acked(sk, sample)` | 每个 ACK 的采样 | 更新 pacing rate、RTT 等 |
| `get_info(...)` | `TCP_INFO` / `INET_DIAG` | 导出算法私有状态 |

`tcp_cong.c` 负责注册（`tcp_register_congestion_control()`）、按名字查找
（`tcp_ca_find()`）、按 `setsockopt(TCP_CONGESTION)` 切换
（`tcp_set_congestion_control()`），以及慢启动/拥塞避免的公共实现
（`tcp_slow_start()`、`tcp_cong_avoid_ai()`、`tcp_reno_cong_avoid()`）。
`icsk->icsk_ca_ops` 指向当前算法，`icsk_ca_priv[]` 是算法私有数据区
（`ICSK_CA_PRIV_SIZE`，默认 104 字节）。

---

## 2. 关键内核数据结构

### 2.1 `struct tcp_sock`（`include/linux/tcp.h`，已核对）

```c
struct tcp_sock {
	struct inet_connection_sock	inet_conn;     /* 首成员 */

	/* TX 读多写少 */
	u32	max_window;        /* 对端通告过的最大窗口 */
	u32	rcv_ssthresh;      /* 接收窗口钳制（SWS 避免） */
	u32	reordering;        /* 乱序度量，默认 3 */
	u16	gso_segs;          /* 每个 GSO 包最多几段 */
	struct sk_buff *lost_skb_hint;
	struct sk_buff *retransmit_skb_hint;

	/* TXRX */
	u32	tsoffset;
	u32	snd_wnd;           /* ★ 对端通告窗口（字节） */
	u32	mss_cache;         /* 缓存的有效 MSS（不含 SACK 选项） */
	u32	snd_cwnd;          /* ★ 拥塞窗口（单位 MSS） */
	u32	prr_out;
	u32	lost_out;          /* 判定丢失的包数 */
	u32	sacked_out;        /* 被 SACK 确认的包数 */
	u16	tcp_header_len;
	u8	scaling_ratio;
	u8	is_sack_reneg:1, is_cwnd_limited:1, ...;

	/* RX 读多写少 */
	u32	copied_seq;        /* ★ 应用尚未读走的数据头部序号 */
	u32	rcv_tstamp;        /* 最后收到 ACK 的时间（keepalive 用） */
	u32	snd_wl1;           /* 窗口更新的序号依据 */
	u32	tlp_high_seq;      /* TLP 发出时的 snd_nxt */
	u32	rttvar_us;
	u32	retrans_out;
	u16	advmss;
	u16	urg_data;
	u32	lost;
	struct minmax rtt_min;
	struct rb_root	out_of_order_queue;   /* ★ 乱序队列（红黑树，按 seq） */
	u32	snd_ssthresh;      /* ★ 慢启动阈值 */

	/* TX 读写 */
	u32	segs_out, data_segs_out;
	u64	bytes_sent;
	u32	snd_sml;           /* 最近发出的小包尾部 */
	u32	write_seq;         /* ★ 发送缓冲区尾部 + 1 */
	u32	pushed_seq;
	u32	lsndtime;
	u32	mdev_us;
	u32	rtt_seq;
	u64	tcp_wstamp_ns;     /* pacing：下一个包的出发时间 */
	struct list_head tsorted_sent_queue;
	struct sk_buff *highest_sack;

	/* TXRX 读写 */
	u8	nonagle : 4;       /* ★ Nagle 状态（TCP_NODELAY 落点） */
	__be32	pred_flags;        /* 头部预测标志（快路径） */
	u64	tcp_clock_cache, tcp_mstamp;
	u32	rcv_nxt;           /* ★ 期望收到的下一个序号 */
	u32	snd_nxt;           /* ★ 下一个要发的序号 */
	u32	snd_una;           /* ★ 最早未确认的序号 */
	u32	window_clamp;      /* 通告窗口上限 */
	u32	srtt_us;           /* 平滑 RTT << 3（微秒） */
	u32	packets_out;       /* 在途包数 */
	u32	snd_up;            /* 紧急指针 */
	u32	delivered, delivered_ce, app_limited;
	u32	rcv_wnd;           /* ★ 本端当前接收窗口 */
	struct tcp_options_received rx_opt;   /* SACK/TS/WS 协商结果 */

	/* RX 读写 */
	u64	bytes_received;
	u32	segs_in, data_segs_in;
	u32	rcv_wup;           /* 上次通告窗口时的 rcv_nxt */
	u32	max_packets_out;
	u32	rate_delivered, rate_interval_us;
	u64	first_tx_mstamp, delivered_mstamp;
	u64	bytes_acked;

	u32	rcv_ssthresh;       /* 见上（SWS 钳制） */
	u32	cwnd_seq;
	/* … pacing、TSO、repair、ulp、fastopen、压缩 ACK 等字段 */
};
```

本章最需要记住的 8 个序号/窗口变量：

| 字段 | 含义 | 推进者 |
| --- | --- | --- |
| `snd_una` | 最早未被确认的序号 | `tcp_ack()` 处理累积确认时 |
| `snd_nxt` | 下一个要发送的新序号 | `tcp_event_new_data_sent()` |
| `snd_wnd` | 对端通告的窗口大小 | `tcp_ack()` 的窗口更新分支 |
| `snd_cwnd` | 拥塞窗口（MSS 为单位） | 拥塞算法 / `tcp_fastretrans_alert()` |
| `snd_ssthresh` | 慢启动阈值 | 丢包/恢复流程、`tcp_reno_ssthresh()` |
| `rcv_nxt` | 期望收到的下一个序号 | `tcp_data_queue()`、`tcp_ofo_queue()` |
| `rcv_wnd` | 当前可通告的接收窗口 | `tcp_select_window()` |
| `copied_seq` | 应用已读走的位置 | `tcp_recvmsg_locked()` |

### 2.2 `struct inet_connection_sock`（`include/net/inet_connection_sock.h`，已核对）

```c
struct inet_connection_sock {
	struct inet_sock	  icsk_inet;
	struct request_sock_queue icsk_accept_queue;  /* ★ 两个队列都在这里 */
	struct inet_bind_bucket	  *icsk_bind_hash;
	struct inet_bind2_bucket  *icsk_bind2_hash;
	unsigned long		  icsk_timeout;
	struct timer_list	  icsk_retransmit_timer;   /* ★ 重传 / 延迟ACK / keepalive */
	struct timer_list	  icsk_delack_timer;
	__u32			  icsk_rto;         /* ★ 当前 RTO（jiffies） */
	__u32			  icsk_rto_min;
	__u32			  icsk_delack_max;
	__u32			  icsk_pmtu_cookie;
	const struct tcp_congestion_ops *icsk_ca_ops;  /* ★ 拥塞算法 */
	const struct inet_connection_sock_af_ops *icsk_af_ops;
	const struct tcp_ulp_ops  *icsk_ulp_ops;
	__u8			  icsk_ca_state:5,     /* ★ TCP_CA_Open/Disorder/CWR/Recovery/Loss */
				  icsk_ca_initialized:1,
				  icsk_ca_setsockopt:1,
				  icsk_ca_dst_locked:1;
	__u8			  icsk_retransmits;    /* ★ 连续重传次数（指数退避） */
	__u8			  icsk_pending;        /* 当前挂起的定时器类型（ICSK_TIME_*） */
	__u8			  icsk_backoff;
	__u8			  icsk_syn_retries;
	__u8			  icsk_probes_out;     /* ★ 零窗口探测次数 */
	__u16			  icsk_ext_hdr_len;
	struct {
		__u8	pending;      /* ACK 待发 */
		__u8	quick;        /* 快速 ACK 模式剩余次数 */
		__u8	pingpong;
		__u8	retry;
		__u32	ato:8, ...;   /* ACK 超时（延迟 ACK 定时器） */
		unsigned long timeout;
		__u32	lrcvtime;
		__u16	last_seg_size, rcv_mss;
	} icsk_ack;
	struct { int search_high, search_low; u32 probe_size:31, enabled:1;
	         u32 probe_timestamp; } icsk_mtup;   /* PMTU 发现 */
	u32			  icsk_probes_tstamp;
	u32			  icsk_user_timeout;  /* TCP_USER_TIMEOUT */
	u64			  icsk_ca_priv[104 / sizeof(u64)];  /* 拥塞算法私有数据 */
};
```

`icsk_af_ops` 指向 `ipv4_specific`（`net/ipv4/tcp_ipv4.c`）：
`queue_xmit = ip_queue_xmit`、`send_check = tcp_v4_send_check`、
`conn_request = tcp_v4_conn_request`（实际转 `tcp_conn_request`）、
`syn_recv_sock = tcp_v4_syn_recv_sock`、`rebuild_header = tcp_v4_rebuild_header`。
`tcp_transmit_skb()` 最后就是通过 `icsk->icsk_af_ops->queue_xmit()`
进入 IP 层的——这是传输层与网络层之间的正式边界。

### 2.3 `struct request_sock_queue` / `struct request_sock`（`include/net/request_sock.h`，已核对）

```c
struct request_sock_queue {
	spinlock_t		rskq_lock;
	u8			rskq_defer_accept;
	u32			synflood_warned;
	atomic_t		qlen;              /* 半连接队列长度 */
	atomic_t		young;
	struct request_sock	*rskq_accept_head; /* 全连接队列头 */
	struct request_sock	*rskq_accept_tail;
	struct fastopen_queue	fastopenq;         /* TFO 专用队列 */
};

struct request_sock {
	struct sock_common		__req_common;   /* 复用 common 做 hash/状态 */
#define rsk_refcnt		__req_common.skc_refcnt
#define rsk_hash		__req_common.skc_hash
#define rsk_listener		__req_common.skc_listener
#define rsk_rcv_wnd		__req_common.skc_rcv_wnd

	struct request_sock		*dl_next;
	u16				mss;
	u8				num_retrans;   /* SYN+ACK 重传次数 */
	u8				syncookie:1;   /* 该请求用 cookie 验证 */
	u8				num_timeout:7;
	u32				ts_recent;
	struct timer_list		rsk_timer;     /* SYN+ACK 重传定时器 */
	const struct request_sock_ops	*rsk_ops;
	struct sock			*sk;           /* 指向 listener */
	struct saved_syn		*saved_syn;    /* SYN cookie 需要重放 SYN 选项 */
	u32				secid, peer_secid, timeout;
};
```

### 2.4 `struct inet_hashinfo`（`include/net/inet_hashtables.h`，已核对）

6.12 的查找结构已经从经典的 3 张表演化为 4 张：

```c
struct inet_hashinfo {
	struct inet_ehash_bucket	*ehash;        /* ★ 已建立连接：精确四元组 */
	spinlock_t			*ehash_locks;
	unsigned int			ehash_mask, ehash_locks_mask;

	struct kmem_cache		*bind_bucket_cachep;
	struct inet_bind_hashbucket	*bhash;       /* ★ 按本地端口（bind 冲突用） */
	struct kmem_cache		*bind2_bucket_cachep;
	struct inet_bind_hashbucket	*bhash2;      /* ★ 按本地端口 + 本地地址（加速冲突判定） */
	unsigned int			bhash_size;

	unsigned int			lhash2_mask;
	struct inet_listen_hashbucket	*lhash2;      /* ★ 监听表：按端口 + 地址 */
	bool				pernet;
};
```

- `ehash`：ESTABLISHED 及 `TCP_SYN_RECV` 等「有完整身份」的 socket，
  键是 `(saddr, sport, daddr, dport)`，hash 函数 `inet_ehashfn()`。
  TIME_WAIT 的 `inet_timewait_sock` 也在 ehash 里（这样查找路径统一）。
- `lhash2`：LISTEN socket，键是 `(port, 本地地址)`；`__inet_lookup_listener()`
  先精确匹配地址，找不到再用通配地址的 listener。
- `bhash` / `bhash2`：`bind()` 时的端口占用表。**6.12 新增 `bhash2`** 的意义是
  把「端口 + 地址」的冲突判定从「遍历 bhash 同端口桶里的所有 socket」变成
  「按地址再散列一次」，显著改善了大量 bind 同端口不同地址场景的扩展性。

### 2.5 `struct inet_timewait_sock`（`include/net/inet_timewait_sock.h`，已核对）

TIME_WAIT 不保留完整的 `struct sock`（太贵），而是用轻量结构：

```c
struct inet_timewait_sock {
	struct sock_common	__tw_common;   /* 仍用 sock_common 参与 hash 查找 */
#define tw_family	__tw_common.skc_family
#define tw_state	__tw_common.skc_state
#define tw_reuse	__tw_common.skc_reuse
#define tw_reuseport	__tw_common.skc_reuseport
#define tw_refcnt	__tw_common.skc_refcnt
#define tw_hash		__tw_common.skc_hash
#define tw_daddr	__tw_common.skc_daddr
#define tw_rcv_saddr	__tw_common.skc_rcv_saddr
#define tw_dport	__tw_common.skc_dport
#define tw_num		__tw_common.skc_num

	__u32			tw_mark;
	unsigned char		tw_substate;    /* TCP_TIME_WAIT / FIN_WAIT2 子状态 */
	unsigned char		tw_rcv_wscale;
	__be16			tw_sport;
	unsigned int		tw_transparent:1, tw_flowlabel:20, tw_usec_ts:1, ...;
	u32			tw_txhash, tw_priority;
	struct timer_list	tw_timer;       /* ★ TIME_WAIT 定时器 */
	struct inet_bind_bucket	*tw_tb;
	struct inet_bind2_bucket *tw_tb2;
};
```

它由 `inet_twsk_alloc()` 从 `tcp_death_row` 的 `tw_sock_TCP` kmem_cache 分配
（`/proc/slabinfo` 里能看到），计入 `tcp_death_row.tw_count`
（`net.ipv4.tcp_max_tw_buckets`）。`tcp_time_wait()` 在 `net/ipv4/tcp_minisocks.c`
里完成「全量 sock → timewait 结构」的转换。

### 2.6 `struct tcp_congestion_ops`（`include/net/tcp.h`，已核对首成员）

```c
struct tcp_congestion_ops {
	u32 (*ssthresh)(struct sock *sk);
	void (*cong_avoid)(struct sock *sk, u32 ack, u32 acked);
	void (*set_state)(struct sock *sk, u8 new_state);
	void (*cwnd_event)(struct sock *sk, enum tcp_ca_event ev);
	void (*in_ack_event)(struct sock *sk, u32 flags);
	void (*pkts_acked)(struct sock *sk, const struct ack_sample *sample);
	u32 (*min_tso_segs)(struct sock *sk);
	void (*cong_control)(struct sock *sk, u32 ack, int flag,
	                     const struct rate_sample *rs);
	u32 (*undo_cwnd)(struct sock *sk);
	u32 (*sndbuf_expand)(struct sock *sk);
	size_t (*get_info)(struct sock *sk, u32 ext, int *attr,
	                   union tcp_cc_info *info);
	char 			name[TCP_CA_NAME_MAX];
	struct module		*owner;
	struct list_head	list;
	u32			key;
	u32			flags;
	void (*init)(struct sock *sk);
	void (*release)(struct sock *sk);
} ____cacheline_aligned_in_smp;
```

注释里明确区分了「fast path 字段放前面填满一个 cache line」
和「control/slow path 放后面」，这是内核为了 cache 局部性做的刻意布局。

### 2.7 `struct udp_sock`（`include/linux/udp.h`，已核对）

```c
struct udp_sock {
	struct inet_sock inet;            /* 首成员 */
#define udp_port_hash		inet.sk.__sk_common.skc_u16hashes[0]
#define udp_portaddr_hash	inet.sk.__sk_common.skc_u16hashes[1]
#define udp_portaddr_node	inet.sk.__sk_common.skc_portaddr_node

	unsigned long	 udp_flags;       /* UDP_FLAGS_*：ENCAP/GRO/ACCEPT_FRAGLIST... */
	int		 pending;         /* cork：是否有待发帧 */
	__u8		 encap_type;      /* 是否封装 socket（如 VXLAN/Geneve） */
	__u16		 len;             /* cork 累计长度 */
	__u16		 gso_size;        /* ★ UDP_SEGMENT 设置的 GSO 段大小 */
	__u16		 pcslen, pcrlen;  /* UDP-Lite 校验范围 */
	int (*encap_rcv)(struct sock *sk, struct sk_buff *skb);
	void (*encap_err_rcv)(...);
	int (*encap_err_lookup)(...);
	void (*encap_destroy)(...);
	struct sk_buff *(*gro_receive)(struct sock *sk, struct list_head *head,
	                               struct sk_buff *skb);
	int (*gro_complete)(struct sock *sk, struct sk_buff *skb, int nhoff);
	struct sk_buff_head reader_queue ____cacheline_aligned_in_smp;  /* ★ 与 sk_receive_queue 并行 */
	int		forward_deficit;
	int		forward_threshold;
	bool		peeking_with_offset;
};
```

注意 `reader_queue`：这是 5.x 引入的优化——`udp_recvmsg()` 在持有 reader 锁的情况下
先把 skb 挪到 `reader_queue`，从而把「取数据」与「收包入队」的锁分开，
降低多线程 UDP 收包的争用。

---

## 3. 数据流与函数调用链

### 3.1 三次握手（服务端视角）

```
① SYN 到达
   ip_rcv → ip_local_deliver → ip_protocol_deliver_rcu → tcp_v4_rcv()
net/ipv4/tcp_ipv4.c: tcp_v4_rcv(struct sk_buff *skb)
   ├─ __inet_lookup_skb(&tcp_hashinfo, skb, ..., &refcounted)
   │     → 未命中 ehash → __inet_lookup_listener() 命中 LISTEN socket
   ├─ 校验（tcp_v4_checksum_init / __skb_checksum_complete）
   ├─ nf_hook(NF_INET_PRE_ROUTING) 之后的 tcp_filter()
   ├─ sk->sk_state == TCP_LISTEN
   └─ tcp_v4_do_rcv(sk, skb)                    net/ipv4/tcp_ipv4.c
        └─ tcp_rcv_state_process(sk, skb)       net/ipv4/tcp_input.c
             └─ case TCP_LISTEN → icsk->icsk_af_ops->conn_request(sk, skb)
                  = tcp_v4_conn_request() → tcp_conn_request()   net/ipv4/tcp_input.c
                     ├─ inet_csk_reqsk_queue_is_full(sk) / tcp_synq_overflow(sk)
                     │     队列满且未开 cookie → 丢包（ListenDrops / TCPReqQFullDrop）
                     ├─ 决定是否启用 SYN cookie（tcp_synq_overflow + sysctl_tcp_syncookies）
                     ├─ req = inet_reqsk_alloc(&tcp_request_sock_ops, sk, ...)
                     │     从 request_sock_TCP kmem_cache 分配 struct tcp_request_sock
                     ├─ tcp_openreq_init_rwin()/tcp_parse_options()：记录 MSS/WS/SACK/TS
                     └─ 发送 SYN+ACK：
                          tcp_v4_send_synack() → icsk->icsk_af_ops->queue_xmit
                          若用 cookie：cookie_v4_init_sequence()（net/ipv4/syncookies.c）
                             **不分配/不保存 request_sock**，信息编码进 SYN+ACK 的序号
                     └─ inet_csk_reqsk_queue_add(sk, req, ...)/inet_csk_reqsk_queue_hash_add()
                          → request_sock 进入 SYN 队列（icsk_accept_queue 的 hash + qlen）
                          → 启动 rsk_timer（SYN+ACK 重传，tcp_syn_retries 相关）

② 第三次握手 ACK 到达
   tcp_v4_rcv() → __inet_lookup_skb() 命中 ehash 中的
        **TCP_SYN_RECV 状态的 request_sock**（NEW_SYN_RECV）
   → tcp_v4_do_rcv() → tcp_rcv_state_process()
        └─ case TCP_SYN_RECV / tcp_check_req(req, skb, ...)   net/ipv4/tcp_input.c
             ├─ 校验 ACK 是否匹配（seq/ack/TS/cookie）
             ├─ newsk = tcp_v4_syn_recv_sock(sk, skb, req, dst, req_unhash, own_req)
             │     net/ipv4/tcp_ipv4.c
             │     ├─ tcp_create_openreq_child()：tcp_v4_syn_recv_sock 里的核心，
             │     │     从 listener 派生一个完整 struct sock
             │     │     （复制 mss/窗口/选项，状态置 TCP_SYN_RECV）
             │     └─ 失败（accept 队列满）→ 返回 NULL → 走「丢弃 ACK」分支
             └─ inet_csk_complete_hashdance(sk, child, req, own_req)
                  net/ipv4/inet_connection_sock.c
                  ├─ inet_csk_reqsk_queue_drop()：从 SYN 队列摘除
                  ├─ inet_csk_reqsk_queue_add()：child 挂到 rskq_accept_head 链表（★ accept 队列）
                  └─ inet_ehash_insert()/inet_hash()：child 进入 ehash
             → tcp_child_process(sk, child, skb)  net/ipv4/tcp_minisocks.c
                  └─ 唤醒 listener 上等待 accept 的进程：
                     sk->sk_data_ready(sk) → sock_def_readable() → 唤醒 epoll/accept

③ 应用 accept()
   __sys_accept4() → inet_accept() → inet_csk_accept()
   net/ipv4/inet_connection_sock.c
     ├─ 若 rskq_accept_head 为空 → 等待（阻塞）或返回 EAGAIN
     ├─ req = rskq_accept_head; 从链表摘除
     ├─ if (!req->sk) { 走到这里说明只有半连接 → inet_csk_reqsk_queue_add 之前的状态 }
     └─ newsk = req->sk; req->sk = NULL;
         返回前：newsk 状态从 TCP_SYN_RECV 置为 TCP_ESTABLISHED
         （tcp_rcv_state_process 里对 child 的处理 / tcp_set_state）
```

### 3.1.1 客户端视角：SYN_SENT 与 `tcp_rcv_synsent_state_process()`

上面的 §3.1 是服务端视角。客户端 `connect()`（`tcp_v4_connect()`）发出 SYN 后
进入 `TCP_SYN_SENT`，SYN+ACK 到达时走的是**另一条分支**：

```
tcp_v4_rcv() → __inet_lookup_skb() 命中 ehash 中 TCP_SYN_SENT 的 sk
   └─ tcp_v4_do_rcv(sk, skb)
        └─ tcp_rcv_state_process(sk, skb)      net/ipv4/tcp_input.c
             └─ case TCP_SYN_SENT:
                  tcp_rcv_synsent_state_process(sk, skb, th)
                  net/ipv4/tcp_input.c
                    ├─ 校验 SYN+ACK 的 ack_seq 是否等于 tp->snd_nxt（我们发的 SYN 序号+1）
                    ├─ 解析选项：MSS / 窗口缩放 / SACK permitted / 时间戳
                    │     （tcp_parse_options → tp->rx_opt）
                    ├─ 记录对端初始序号：tp->rcv_nxt = TCP_SKB_CB(skb)->seq + 1
                    ├─ tcp_init_wl()/tcp_update_wl()：初始化 snd_wl1，接受窗口 tp->snd_wnd
                    ├─ tcp_finish_connect(sk, skb)：
                    │     tcp_set_state(sk, TCP_ESTABLISHED)
                    │     tp->snd_una = tp->write_seq
                    │     tcp_init_congestion_control(sk) / tcp_init_buffer_space(sk)
                    │     sk->sk_state_change(sk)（唤醒阻塞在 connect 的进程）
                    └─ 发送第三个 ACK：tcp_send_ack(sk)
```

要点：

- 客户端在收到 SYN+ACK 后才把状态置为 `TCP_ESTABLISHED`，
  并把 `snd_una` 对齐到 `write_seq`；
- `tcp_init_congestion_control()` 与 `tcp_init_buffer_space()` 在这里才真正
  为连接选定拥塞算法、初始化 `snd_cwnd`/`snd_ssthresh` 与接收缓冲空间 ——
  这解释了为什么 `TCP_INFO` 里 `snd_cwnd` 在刚建连时是 10（`TCP_INIT_CWND`）；
- 若 SYN+ACK 里带 `SYN` 以外的数据（TCP Fast Open），会在
  `tcp_rcv_synsent_state_process()` 的分支里一并入队。

**关键结论**：三次握手完全由内核完成，`accept()` 只是在 accept 队列上取结果。
这也解释了为什么 `ss -lnt` 的 `Recv-Q` 可以直接读到「已握手但还没被 accept 的连接数」。
注意「客户端什么时候算连上」与「服务端什么时候把它放进 accept 队列」是两个独立事件：
本 lab 的 `backlog_probe` 实测到客户端 `connect()` 成功、服务端却永远
`accept()` 不到（accept 队列溢出、静默丢 ACK），就是这两个事件解耦的直接后果。

### 3.2 发送路径

```
用户态 send()/write()
   ▼  第 02 章已展开：sock_write_iter → sock_sendmsg → inet_sendmsg → tcp_sendmsg
tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)   net/ipv4/tcp.c
   └─ tcp_sendmsg_locked(sk, msg, size)
        ├─ tcp_send_mss(sk, &mss_now, &size_goal) → tcp_current_mss()
        │     考虑：PMTU、tp->mss_cache、是否已协商 TS/SACK 选项、TSO
        ├─ 循环切片，把用户数据放进 sk_write_queue 的 skb
        │     ├─ 需要新 skb 时 tcp_stream_alloc_skb()（记账 sk_wmem_queued）
        │     ├─ sk_stream_memory_free()/sk_stream_wait_memory()：sndbuf 不足则阻塞
        │     └─ 优先把多段数据合并进同一个 skb（减少段数、利于 TSO）
        └─ tcp_push(sk, flags, mss_now, nonagle, size_goal)  net/ipv4/tcp.c
             └─ __tcp_push_pending_frames(sk, mss_now, nonagle)  net/ipv4/tcp_output.c
                  └─ tcp_write_xmit(sk, mss_now, nonagle, 0, gfp)  net/ipv4/tcp_output.c
                       ├─ tcp_mtu_probe(sk)（PMTU 探测）
                       ├─ while ((skb = tcp_send_head(sk))):
                       │   ├─ tcp_pacing_check(sk)：pacing（BBR/fq）未到点则 break
                       │   ├─ cwnd_quota = tcp_cwnd_test(tp)      ← min(snd_cwnd, ...)
                       │   ├─ tcp_snd_wnd_test(tp, skb, mss)      ← 对端窗口是否允许
                       │   ├─ tcp_nagle_test(tp, skb, mss, nonagle)  ← Nagle 判定
                       │   ├─ tcp_tso_should_defer()（TSO 攒批）
                       │   ├─ tcp_small_queue_check()（TSQ 限流）
                       │   └─ tcp_transmit_skb(sk, skb, 1, gfp)  net/ipv4/tcp_output.c
                       │        ├─ tcp_options_write()：写 SACK/TS/WS 选项
                       │        ├─ icsk->icsk_af_ops->send_check()：校验和
                       │        └─ icsk->icsk_af_ops->queue_xmit(sk, skb, &fl4)
                       │             = ip_queue_xmit()   net/ipv4/ip_output.c
                       │                  → 进入 IP 层：路由、分片（一般不分片）、
                       │                    ip_local_out → dst_output → 设备队列
                       └─ tcp_event_new_data_sent(sk, skb)：推进 snd_nxt、
                          packets_out、重挂重传定时器
```

要点：

- `skb` 在 `sk_write_queue`（发送队列）里等 ACK；被 ACK 确认后从队列移除并释放。
- `packets_out` 计「在途包数」，`tcp_packets_in_flight()` =
  `packets_out - (sacked_out + lost_out) + retrans_out`，这才是拥塞控制眼里的在途量。
- TSQ（`tcp_small_queue_check`）限制的是「已交给 qdisc/驱动但还没发完的字节」
  （`sk_wmem_alloc`），与 `sndbuf` 是两套限制，前者治 bufferbloat。

### 3.3 接收路径

```
tcp_v4_rcv()（已在前文展开）
   └─ tcp_v4_do_rcv(sk, skb)  net/ipv4/tcp_ipv4.c
        ├─ 若 socket 被用户态持有 → tcp_add_backlog(sk, skb, &reason)
        │     → sk_add_backlog()（第 02 章 §3.7）
        └─ sk->sk_state == TCP_ESTABLISHED
             → tcp_rcv_established(sk, skb)   net/ipv4/tcp_input.c
                  ├─ 【快路径】头部预测 pred_flags 命中：
                  │     ├─ tcp_ack()（若带 ACK）
                  │     ├─ tcp_queue_rcv()：skb_queue_tail(&sk->sk_receive_queue, skb)
                  │     └─ tcp_data_ready(sk) → sk->sk_data_ready(sk)
                  │            → sock_def_readable() → 唤醒 epoll/select/阻塞 recv
                  └─ 【慢路径】
                        ├─ tcp_ack(sk, skb, flags)         net/ipv4/tcp_input.c
                        │     ├─ tcp_clean_rtx_queue()：把被确认的 skb 从重传队列摘除
                        │     ├─ tcp_rtt_estimator() / tcp_set_rto()：更新 RTT/RTO
                        │     ├─ tcp_fastretrans_alert()：拥塞状态机（见 §3.5）
                        │     └─ tcp_select_window() → 更新 tp->rcv_wnd
                        └─ tcp_data_queue(sk, skb)         net/ipv4/tcp_input.c
                              ├─ 完全按序 → tcp_queue_rcv()（进 sk_receive_queue）
                              │     └─ 更新 tp->rcv_nxt、rcv_wup
                              ├─ 乱序 → tcp_data_queue_ofo(sk, skb)
                              │     └─ tp->out_of_order_queue（红黑树）
                              │           └─ tcp_ofo_queue()：当空洞被填上时，
                              │                把树里连续的一段搬到 sk_receive_queue
                              ├─ 重复段/DSACK → tp->duplicate_sack / tp->dsack
                              ├─ 内存压力 → tcp_prune_queue() → tcp_collapse()
                              │     （把相邻小 skb 合并，统计 TCPRcvCollapsed）
                              └─ tcp_data_ready(sk)：唤醒应用
```

`tcp_data_ready(sk)` 的实现（`net/ipv4/tcp_input.c`）在启用
`tp->recvmsg_inq`/低延迟特性时会额外判断「新数据是否满足 sk_rcvlowat」，
最终仍调用 `sk->sk_data_ready(sk)`，即默认的 `sock_def_readable()`。

### 3.4 乱序队列、SACK 与重排序容忍

- 乱序段进入 `tp->out_of_order_queue`（红黑树，键是序号），
  `tcp_ofo_queue()` 在空洞被填补后把连续前缀搬入 `sk_receive_queue`。
- `tp->reordering`（默认 3，可由 `net.ipv4.tcp_reordering` 调整）是
  「容忍多少个乱序段才判定丢包」的阈值。`tcp_sacktag_write_queue()` 负责
  按 SACK 块标记哪些段已被接收方收到：
  `tp->sacked_out`、`TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED`。
- 快速重传的触发条件（Reno 风格，`tcp_fastretrans_alert()`）：
  `tp->sacked_out >= tp->reordering` 或收到重复 ACK 且 SACK 显示空洞
  → `tcp_enter_recovery()` / `tcp_enter_loss()` → 重传 `snd_una` 处的段。
- **SACK reneging**：接收方可能「撤销」之前的 SACK。内核用
  `tp->is_sack_reneg` 与 `tcp_check_sack_reneging()` 处理，
  这也是为什么不能把 sacked 当作永久确认。
- **DSACK**：重复段信息通过 `tp->duplicate_sack` 回送给对端，
  对端据此区分「真丢包」与「伪重传」，`undo_cwnd()` 就是给虚假快速重传兜底的。

### 3.5 拥塞控制状态机（`tcp_fastretrans_alert()` 的内部分支）

`icsk->icsk_ca_state` 只有 5 个状态
（`include/uapi/linux/tcp.h` 的 `enum tcp_ca_state`）：

```
TCP_CA_Open ──丢包/3 dupACK──▶ TCP_CA_Disorder ──确认丢包──▶ TCP_CA_Recovery
     ▲                                │                            │
     │                                └── ECE/ECN ──▶ TCP_CA_CWR     │
     └────────────── 恢复完成 ◀──────────────────────────────────┘
                              │
                              └── RTO 超时 ──▶ TCP_CA_Loss ──▶ 回到 Open
```

- `tcp_enter_loss()`：RTO 超时或判定严重丢失 → `ssthresh` 取
  `max(flight/2, 2*MSS)`，`snd_cwnd` 归 1（或按 `undo` 逻辑），
  状态进 `TCP_CA_Loss`，`tcp_set_ca_state()` 通知算法。
- `tcp_enter_recovery()`：快速重传路径 → `TCP_CA_Recovery`，
  cwnd 减半，进入**快速恢复**（PRR 算法靠 `tp->prr_out`/`tp->prr_delivered` 两个**字段**控制恢复期发送节奏，不是函数）。
- `tcp_set_ca_state()`（`net/ipv4/tcp_cong.c`）负责把状态变化通知给
  `icsk_ca_ops->set_state()`，并触发 `cwnd_event()`。
- 慢启动：`cwnd < ssthresh` 时 ACK 一个段 cwnd +1（`tcp_slow_start()`）；
  拥塞避免：`tcp_cong_avoid_ai()` 每个 RTT 加 1 个 MSS。
- **CUBIC**（`net/ipv4/tcp_cubic.c`，本机默认）：以三次函数
  `W(t) = C*(t-K)^3 + W_max` 描述窗口增长，`K = cbrt(W_max*β/C)`，
  比 Reno 的线性增长更适合高速长距离链路。
- **BBR**（`net/ipv4/tcp_bbr.c`）：不使用丢包作为拥塞信号，
  通过 `cong_control()` 完全接管 cwnd 与 pacing，
  维护 `BtlBw`（瓶颈带宽）与 `RTprop`（最小 RTT）估计，输出
  `pacing_rate` 与 `cwnd = BDP * gain`。
  **注意：本机 `tcp_available_congestion_control` 只有 `reno cubic`，
  没有 bbr 模块**（在没有 root/insmod 的情况下无法加载，
  这也说明「部署 BBR」在实践中往往需要 `modprobe tcp_bbr` + sysctl）。

### 3.6 RTT / RTO 与定时器

```
tcp_ack() 处理带时间戳/带被确认数据的 ACK
   └─ tcp_clean_rtx_queue()：摘除已被确认的段，产出 RTT 采样
        └─ tcp_rtt_estimator(sk, seq_rtt_us)
             ├─ srtt_us = srtt + (rtt - srtt)/8        （平滑 RTT，<<3 存微秒）
             ├─ rttvar  = rttvar + (|rtt - srtt| - rttvar)/4
             └─ (Karn 算法：重传过的段不采样 RTT)
   └─ tcp_set_rto(sk)  net/ipv4/tcp_input.c
        └─ icsk_rto = (srtt_us>>3) + max(1 jiffy, rttvar_us>>2)，
           再 clamp 到 [TCP_RTO_MIN, TCP_RTO_MAX]
   └─ tcp_rearm_rto(sk)：按 icsk_pending 重新挂 icsk_retransmit_timer
```

`icsk_retransmit_timer` 是**多用途定时器**，通过 `icsk_pending` 区分：

| `icsk_pending` 取值 | 触发函数 | 用途 |
| --- | --- | --- |
| `ICSK_TIME_RETRANS` | `tcp_retransmit_timer()` | RTO 超时重传（指数退避 `icsk_backoff`） |
| `ICSK_TIME_PROBE0` | `tcp_probe_timer()` | 零窗口探测（`icsk_probes_out`） |
| `ICSK_TIME_LOSS_PROBE` | `tcp_send_loss_probe()` | TLP：尾部丢包探测 |
| `ICSK_TIME_REO_TIMEOUT` | `tcp_rack_reo_timeout()` | 重排序超时（RACK 判定乱序超时） |

> 两个容易写错的地方：① 延迟 ACK 用的是**另一个定时器对象** `icsk_delack_timer` →
> `tcp_delack_timer()`（`net/ipv4/tcp_timer.c`），它**不在**上面那个 switch 里；
> ② `ICSK_TIME_EARLY_RETRANS` 在 6.12 **不存在**
> （`include/net/inet_connection_sock.h` 里的枚举是 RETRANS=1 / DACK=2 / PROBE0=3 / LOSS_PROBE=5 / REO_TIMEOUT=6，值 4 为空）。

**keepalive** 不用这个定时器：它由 `inet_csk_reset_keepalive_timer()` 单独管理，
参数来自 `tp->keepalive_time`/`keepalive_intvl`/`keepalive_probes`
（或对应 sysctl `net.ipv4.tcp_keepalive_*`）。需要 `SO_KEEPALIVE` 打开。

**TIME_WAIT** 定时器是 `tw_timer`（`struct inet_timewait_sock`），
时长取自 `tcp_fin_time()`（`include/net/tcp.h`）：

```c
static inline int tcp_fin_time(const struct sock *sk)
{
	int fin_timeout = tcp_sk(sk)->linger2 ? :
		READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_fin_timeout);
	const int rto = inet_csk(sk)->icsk_rto;

	if (fin_timeout < (rto << 2) - (rto >> 1))
		fin_timeout = (rto << 2) - (rto >> 1);
	return fin_timeout;
}
```

注意 `linger2`（`TCP_LINGER2` 选项）与 `tcp_fin_timeout` 的关系：
**`tcp_fin_timeout` 只影响 FIN_WAIT2 的时长，不影响 TIME_WAIT 的 2*MSL**。
TIME_WAIT 在 `tcp_timewait_state_process()`（`net/ipv4/tcp_minisocks.c`）里
被处理：收到重传 FIN 时重启定时器，收到 RST 则立即销毁，
新 SYN 的处理取决于 `tw_substate` 与 `tcp_tw_reuse` 的判定。

### 3.7 sliding window 与零窗口探测

```
tcp_ack() 的窗口更新分支
   └─ tcp_may_update_window()/tcp_update_wl()：更新 tp->snd_wnd、tp->snd_wl1
        （只有更新的 window 序号 snd_wl1 更大，或 ACK 推进了 snd_una 才接受）

发送侧受限判断（tcp_write_xmit 内）
   ├─ tcp_snd_wnd_test(tp, skb, mss)：skb 结尾是否超出 tcp_wnd_end(tp)
   │     tcp_wnd_end(tp) = tp->snd_una + tp->snd_wnd
   └─ 若窗口为 0 → 挂 ICSK_TIME_PROBE0 定时器 → tcp_probe_timer()
        └─ tcp_send_probe0()：发送零窗口探测（1 字节或重传）
             失败/仍然 0 窗口 → icsk_probes_out++，指数退避
             超过 tcp_retries2 上限 → tcp_write_err() → ETIMEDOUT

接收侧窗口计算
   └─ tcp_select_window(sk)  / __tcp_select_window(sk)  net/ipv4/tcp_output.c
        ├─ free_space = tcp_space(sk) = tcp_win_from_space(sk, sk_rcvbuf
        │                            - sk_backlog.len - sk_rmem_alloc)
        ├─ allowed_space = tcp_full_space(sk) = tcp_win_from_space(sk, sk_rcvbuf)
        ├─ 若 free_space < full_space/2 且 tcp_under_memory_pressure() →
        │     tcp_adjust_rcv_ssthresh()（SWS 避免：窗口不增长到小于 MSS）
        ├─ 不允许窗口缩小（RFC 793），除非 sysctl_tcp_shrink_window 打开
        └─ 结果按 rcv_wscale 右移后填入 TCP 头 window 字段
```

`sk_rcvbuf` 与通告窗口并不相等：内核用 `scaling_ratio`
（`tcp_win_from_space()`）把「内存字节」折算成「窗口字节」，
因为排队 skb 的 `truesize` 远大于数据长度。这就是
`tcpi_rcv_space`（TCP_INFO 暴露的 `rcv_space`）与 `sk_rcvbuf` 不同的原因。

### 3.8 TIME_WAIT / reuse / REUSEPORT 的内核差别

| 机制 | 内核落点 | 语义 |
| --- | --- | --- |
| TIME_WAIT | `tcp_time_wait()` → `inet_timewait_sock` + `tw_timer` | 主动关闭方保留 2*MSL，防止旧连接的迟到报文污染新连接、并保证最后的 ACK 可重发 |
| `tcp_tw_reuse` | `__inet_hash_connect()`/`tcp_twsk_unique()` | 允许**主动连接**复用仍处于 TIME_WAIT 的四元组（本机值为 2，且需要时间戳开启） |
| `SO_REUSEADDR` | `sock_setsockopt()` → `sk->sk_reuse = SK_CAN_REUSE`，判定在 `inet_csk_bind_conflict()` | 允许 `bind` 一个被 TIME_WAIT 占用的本地端口；也允许通配地址与具体地址共存。**判定是双向的**：旧连接变成 `inet_timewait_sock` 后其 `tw_reuse` 必须也为真（从原 socket 的 `sk_reuse` 继承），所以「服务端先设 `SO_REUSEADDR` 再 `listen`」才是正确姿势 —— 本 lab 的 `timewait_demo` 场景 1 实测了这一对行为 |
| `SO_REUSEPORT` | `sock_setsockopt()` → `sk->sk_reuseport = 1`，判定同上 + `inet_lookup_reuseport()` | 允许多个 socket 同时 `bind+listen` 同一 `(addr, port)`，收包时按流哈希选择一个 |
| `SO_LINGER{1,0}` | `tcp_close()` → `tcp_send_active_reset()` | 发 RST 而不是 FIN，跳过 TIME_WAIT（会丢未确认数据） |

`net.ipv4.tcp_max_tw_buckets` 限制并发 TIME_WAIT 数，超限时内核
直接销毁最老的 timewait 并累加 `TcpExt: TWKilled`
（`tcp_timewait_state_process()` 里对 `tcp_death_row.tw_count` 的检查）。

**实测补充（`labs/02-tcp-state-machine/timewait_demo.c --scenario 1`）**：
在同一进程里先用 `SO_REUSEADDR` 建连再主动 `close()`，随后

- `bind(同一 127.0.0.1:port)` **不设** `SO_REUSEADDR` → `EADDRINUSE`；
- 设 `SO_REUSEADDR` 再 `bind` → 成功。

两者唯一的差别就是新 socket 的 `sk_reuse`；而「旧 TIME_WAIT 的 `tw_reuse`」
由原连接继承而来，这就是为什么服务端必须在 `listen` **之前**设置该选项。

### 3.9 Nagle 与延迟 ACK 的交互

```
发送侧：tcp_nagle_test(tp, skb, mss_now, nonagle)   net/ipv4/tcp_output.c
   ├─ tp->nonagle == TCP_NAGLE_OFF（TCP_NODELAY 打开）→ 直接允许发送
   ├─ 在途包（packets_out）为 0 → 允许
   ├─ 数据已满一个 MSS → 允许
   └─ 否则再判断 TCP_NAGLE_CORK / TCP_NAGLE_PUSH：
        默认「有未确认数据 + 小包」→ 抑制发送（Nagle 生效）

接收侧：tcp_send_delayed_ack() / __tcp_ack_snd_check()  net/ipv4/tcp_input.c
   └─ 若处于 quickack 模式（icsk_ack.quick）→ 立即 ACK
      否则挂 ICSK_TIME_DACK → icsk_ack.ato（通常 40ms，随 pingpong 调整）
```

经典病态：客户端 `send` 一个小包 → 服务端延迟 ACK（等 40 ms）→
客户端 Nagle 因为「有未确认小包」而不发下一个 → 单次交互延迟 ~40 ms。
解决办法：`TCP_NODELAY`（关发送侧 Nagle）或 `TCP_QUICKACK`（临时关接收侧延迟 ACK），
或应用层用 writev/合并写。

### 3.10 TSO / GSO / GRO 与 TCP 的配合

- **TSO（TCP Segmentation Offload，发送侧）**：网卡支持时，
  TCP 可以构造一个「超过 MTU 的大 skb」交给驱动，由网卡硬件切成 MSS 大小的段。
  这样 `tcp_write_xmit()` 一次循环可以推进多个 MSS 的数据，
  `tp->gso_segs` 记录该 skb 含几段。
  这解释了一个常见困惑：**`ss -ti` 里 `tcpi_snd_mss` 可能是 65483 这种
  「远大于 1460」的值**——它反映的是内核看到的「可发送分片大小」，
  而不是链路上的 MSS（本机回环 MTU 65536，正是这个原因）。
- **GSO（Generic Segmentation Offload）**：网卡不支持 TSO 时，
  在进入驱动前由软件（`skb_gso_segment()`）切分。TSO/GSO 的逻辑都在
  `net/core/dev.c` 与 `net/ipv4/tcp_output.c`（`tso_fragment()`、
  `tcp_set_skb_tso_segs()`、`tcp_tso_should_defer()`）。
- **GRO（Generic Receive Offload）**：接收侧把多个同流的小包合并成一个大 skb
  再上送协议栈，减少 `tcp_v4_rcv()` 调用次数。GRO 的合并必须满足
  「同五元组 + 序号连续 + 选项一致」，否则会破坏 TCP 语义。
  `ethtool -K <dev> gro on|off`、`net.ipv4.tcp_gro*` 相关 sysctl 控制。
  UDP 也有 `udp_gro_receive()`/`udp_gro_complete()`。
- **UDP GSO/encap**：`UDP_SEGMENT` 让应用把一个大缓冲交给内核，
  由内核/网卡按指定段大小切分（`udp_sock.gso_size`）；
  隧道场景（VXLAN/Geneve）通过 `udp_sock.encap_type` 与
  `encap_rcv`/`gro_receive`/`gro_complete` 回调接入。

### 3.11 UDP 路径

```
发送：udp_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)  net/ipv4/udp.c
   ├─ udp_sendmsg 里处理：IP_OPTIONS/IP_TOS/组播、路由查找
   │     （已 connect 的 UDP 有 sk_dst_cache，跳过路由查找）
   ├─ cork 逻辑：udp_sock.pending / len，攒够或 MSG_MORE 清除后发出
   ├─ 构造 skb（ip_append_data/ip_make_skb 路径），填 UDP 头
   └─ udp_send_skb(sk, skb, fl4, is_udplite)
        ├─ udp_sock.gso_size 非 0 → 标记 GSO（由硬件/软件切分）
        ├─ udp_csum/udp4_check_checksum
        └─ ip_send_skb() → 进入 IP 层
   失败时（ENOBUFS/EMSGSIZE 等）累加 Udp: SndbufErrors/OutErrors

接收：udp_rcv(struct sk_buff *skb) → __udp4_lib_rcv(skb, &udp_table, IPPROTO_UDP)
        net/ipv4/udp.c
   ├─ __udp4_lib_lookup_skb()：按四元组查 udp_table（hash 由 udp_ehashfn 计算）
   │     └─ 未命中 → __udp4_lib_miss_rcv()：发 ICMP port unreachable，
   │          累加 Udp: NoPorts
   ├─ 校验和验证 → 失败则丢包（Udp: InErrors/InCsumErrors）
   ├─ 若 sk 有 encap_rcv（隧道）→ 走封装回调
   └─ udp_queue_rcv_skb(sk, skb)
        ├─ sk_filter()（BPF 过滤）
        ├─ udp_lib_checksum_complete
        ├─ 记账 sk_rmem_alloc（超 sk_rcvbuf → 更新 forward_deficit）
        └─ __udp_queue_rcv_skb() → __skb_queue_tail(&sk->sk_receive_queue, skb)
             └─ sk->sk_data_ready(sk)（唤醒等待的 recvfrom/epoll）
   ★ 与 TCP 最大的区别：UDP 没有重传、没有排序、没有流控；
     接收队列满时**直接丢包**（Udp: RcvbufErrors），
     TCP 则会通过缩小窗口让对端停发。

连接态 UDP：connect() 之后 sk 里记了 daddr/dport，
  发送走 send()（不用每次给出地址，省一次路由查找），
  接收只接受该对端的包（其它源的包被丢弃或走 ICMP 处理），
  且 ICMP 错误可以被投递到 socket（recv 返回 ECONNREFUSED），
  本机 labs/01 的 udp_echo 已实测到这一点。
```

### 3.12 查表结构：一次收包要走几次查找

```
__inet_lookup_skb(&tcp_hashinfo, skb, doff, sport, dport, sdif, &refcounted)
   └─ __inet_lookup(net, hashinfo, skb, doff, saddr, sport, daddr, dport, ...)
        ├─ ① __inet_lookup_established()      → ehash（inet_ehashfn 哈希四元组）
        │      命中：已是 ESTABLISHED / SYN_RECV / TIME_WAIT 的连接
        └─ ② __inet_lookup_listener()         → lhash2（按 daddr+port）
               先精确地址，再通配地址；SO_REUSEPORT 时
               走 inet_lookup_reuseport() 按流哈希选组内某个 sk
```

`bind()` 时用 `bhash`/`bhash2`；`connect()` 时用 `ehash` 找可用本地端口
（`__inet_hash_connect()` 遍历 `ip_local_port_range` 并对每个候选端口查 ehash）。

---

## 4. 可观测点与验证方法

### 4.1 `/proc/net/tcp` 字段逐列解释

```
  sl  local_address rem_address   st tx_queue:rx_queue tr:tm->when retrnsmt   uid  timeout inode
   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 12345 ...
```

| 列 | 含义 |
| --- | --- |
| `sl` | 序号（内核里是 hash bucket 遍历顺序，无稳定语义） |
| `local_address` | `HEX_IP:HEX_PORT`，IPv4 是**小端**字节序，需要字节反转才是点分十进制 |
| `rem_address` | 同上，LISTEN 时为 `00000000:0000` |
| `st` | 状态码（十六进制，见下表） |
| `tx_queue:rx_queue` | 发送队列 / 接收队列的字节数（十六进制） |
| `tr:tm->when` | `tr` 是重传超时计数；`tm->when` 是定时器剩余时间（jiffies，`0x` 前缀表示无效） |
| `retrnsmt` | 该 socket 的重传次数 |
| `uid` | socket 属主 uid |
| `timeout` | 已废弃（历史字段） |
| `inode` | socket 的 inode 号，可与 `/proc/<pid>/fd` 关联定位进程 |

状态码表（与 `enum tcp_state` 一致）：

| 码 | 状态 | 码 | 状态 | 码 | 状态 |
| --- | --- | --- | --- | --- | --- |
| 01 | ESTABLISHED | 05 | FIN_WAIT2 | 09 | LAST_ACK |
| 02 | SYN_SENT | 06 | TIME_WAIT | 0A | LISTEN |
| 03 | SYN_RECV | 07 | CLOSE | 0B | CLOSING |
| 04 | FIN_WAIT1 | 08 | CLOSE_WAIT | | |

`labs/02-tcp-state-machine/parse_proc_tcp.py` 就实现了这套转换，
并额外处理 IPv6（`/proc/net/tcp6` 的地址是 4 组 32bit 小端字）。

### 4.2 `/proc/net/snmp` 的 `Tcp:` 行

```
Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails
     EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors
```

| 字段 | 含义 | 排障用途 |
| --- | --- | --- |
| `RtoAlgorithm` | 固定为 1（RFC793 算法） | — |
| `RtoMin`/`RtoMax` | RTO 上下限（毫秒） | 确认 RTO 区间 |
| `MaxConn` | -1（不限制） | — |
| `ActiveOpens` | `connect()` 成功发起数 | 对比客户端 QPS |
| `PassiveOpens` | 被动建立数 | 对比服务端 accept 数 |
| `AttemptFails` | 连接尝试失败数 | SYN 无响应/被拒 |
| `EstabResets` | 从 ESTABLISHED 被 RST 的次数 | 对端异常断开、`SO_LINGER 0` |
| `CurrEstab` | 当前 ESTABLISHED 数 | 与 `ss -tan state established \| wc -l` 对照 |
| `InSegs`/`OutSegs` | 收发报文总数 | 吞吐/丢包基线 |
| `RetransSegs` | 重传报文数 | **重传率 = RetransSegs / OutSegs** |
| `InErrs`/`InCsumErrors` | 错误报文/校验和错误 | 链路质量、网卡 offload 问题 |
| `OutRsts` | 发出的 RST 数 | 端口未监听、队列溢出回 RST |

### 4.3 `/proc/net/netstat` 的 `TcpExt:` 行（排障重点字段）

| 字段 | 触发点 | 含义 |
| --- | --- | --- |
| `ListenOverflows` | `tcp_v4_syn_recv_sock()` 后队列满 | 三次握手已完成但 accept 队列满，连接无法入队 |
| `ListenDrops` | `tcp_conn_request()` 等 | 所有丢弃连接请求的总和（含 SYN 队列溢出） |
| `TCPReqQFullDrop` / `TCPReqQFullDoCookies` | `tcp_conn_request()` | SYN 队列满时丢包 / 转用 SYN cookie |
| `SyncookiesSent` / `SyncookiesRecv` / `SyncookiesFailed` | `net/ipv4/syncookies.c` | SYN cookie 使用情况（队列溢出信号） |
| `TCPTimeouts` | `tcp_retransmit_timer()` | RTO 超时次数 |
| `TCPFastRetrans` | `tcp_fastretrans_alert()` | 快速重传次数 |
| `TCPSlowStartRetrans` | 慢启动期重传 | 早期丢包 |
| `TCPSackRecovery` / `TCPSackRecoveryFail` | SACK 触发的恢复 | 有 SACK 时的恢复效率 |
| `TCPLossProbes` / `TCPLossProbeRecovery` | TLP | 尾丢包探测（比 RTO 快） |
| `TCPSpuriousRTOs` | `tcp_clean_rtx_queue()` | 伪 RTO（重传后很快收到 ACK） |
| `TCPOFOQueue` / `TCPOFODrop` / `TCPOFOMerge` | `tcp_data_queue_ofo()` | 乱序队列入队/丢弃/合并 |
| `TCPRcvCollapsed` | `tcp_collapse()` | 接收队列折叠次数（内存压力或大数据量） |
| `TCPBacklogDrop` | `sk_add_backlog()` 失败 | 单 socket backlog 溢出 |
| `TCPAbortOnClose` / `TCPAbortOnMemory` / `TCPAbortOnLinger` / `TCPAbortOnTimeout` | `tcp_close()` 相关 | 各种「非正常关闭」原因分解 |
| `TCPMemoryPressures` | `tcp_enter_memory_pressure()` | TCP 全局内存压力事件 |
| `DelayedACKs` / `DelayedACKLost` | `tcp_delack_timer()` | 延迟 ACK 发出数 / 延迟期间检测到丢包 |
| `TW` / `TWRecycled` / `TWKilled` | `tcp_time_wait()` | TIME_WAIT 数量 / 复用 / 被强制销毁 |
| `PAWSActive` / `PAWSEstab` | 时间戳 PAWS 检查 | 时间戳拒绝报文（乱序/A 端重启） |

### 4.4 `/proc/net/sockstat`

```
TCP: inuse 12 orphan 0 tw 3 alloc 15 mem 42
UDP: inuse 5 mem 1
```

| 字段 | 含义 |
| --- | --- |
| `inuse` | 已分配的 TCP socket 数（不含 TIME_WAIT） |
| `orphan` | 无文件/socket 引用但仍存在的 socket（孤儿） |
| `tw` | TIME_WAIT 数量（`tcp_death_row.tw_count`） |
| `alloc` | 已分配的 TCP socket 总数（含 TIME_WAIT） |
| `mem` | 当前 TCP 内存用量，**单位是 page（4 KiB）** |

### 4.5 命令模板

```bash
# TCP 连接级细节（cwnd/rtt/retrans/sacked/sndbuf）
ss -tni
# socket 内存（r=sk_rcvbuf rb=sk_rmem_alloc t=sk_wmem_queued w=sk_sndbuf f=sk_forward_alloc）
ss -tme
# 只看监听与队列
ss -lnt
# 状态过滤
ss -tan state syn-recv
ss -tan state time-wait
# 定时器
ss -tno
# 重传率
awk '/^Tcp:/{n++; if(n==2) print "OutSegs="$10" RetransSegs="$11}' /proc/net/snmp
# 队列溢出计数
python3 labs/02-tcp-state-machine/parse_proc_tcp.py --netstat ListenOverflows ListenDrops \
        TCPTimeouts TCPSackRecovery TCPOFOQueue
# 状态分布
python3 labs/02-tcp-state-machine/parse_proc_tcp.py --summary
```

**tracepoint 模板（本机不可运行：无 CAP_BPF/CAP_PERFMON，无 bpftool/clang）**：

```bash
# 状态机迁移（最直观）
perf trace -e sock:inet_sock_set_state -- sleep 5
# 接收队列溢出
perf trace -e sock:sock_rcvqueue_full -- sleep 5
# TCP 重传事件
perf trace -e tcp:tcp_retransmit_skb -- sleep 5
# 拥塞控制事件
perf trace -e tcp:tcp_cong_state_set -- sleep 5
# SYN 队列/cookie 丢包
perf trace -e tcp:tcp_drop -- sleep 5
```

对应源码位置：`net/ipv4/tcp.c`（`tcp_set_state()` 内触发状态迁移）、
`net/core/sock.c`（`__sk_mem_schedule()` 附近触发内存类事件）、
`net/ipv4/tcp_input.c`/`tcp_output.c`/`tcp_ipv4.c`（`tcp:tcp_*` tracepoint 类）。
请以 <https://elixir.bootlin.com/linux/v6.12/source/> 核对具体 TRACE 宏位置。

### 4.6 本机明确不可运行的项（实测）

| 想做的事 | 本机状态 | 原因 | root 下的做法 |
| --- | --- | --- | --- |
| 抓原始报文（AF_PACKET/tcpdump） | **在 userns 内可行** | 初始命名空间无 CAP_NET_RAW；`unshare -Urn` 内 `CapEff` 变满即可用 AF_PACKET（无 `tcpdump` 命令行工具） | `sudo tcpdump -i lo -nn 'tcp port 8080'` |
| eBPF / bpftrace 观测 | 不可行 | 无 CAP_BPF/CAP_PERFMON（userns 内的 capability 不足以通过 BPF 校验），无 bpftool/libbpf/clang | `sudo bpftrace -e 'kprobe:tcp_retransmit_skb {...}'` |
| TUN/TAP 虚拟网卡 | 不可行 | 无 `/dev/net/tun` 设备节点；userns 内 `mknod c 10 200` 返回 EPERM | `sudo modprobe tun && ip tuntap add ...` |
| 改 sysctl 造拥塞/溢出 | 不可行 | 写初始 netns 的 `/proc/sys/...` 返回 EACCES（`/proc/sys` 属于初始 userns） | `sudo sysctl -w net.ipv4.tcp_syncookies=1` |
| 换拥塞算法为 BBR | 不可行 | 本机 `tcp_available_congestion_control = reno cubic`（无 bbr 模块，且不能 insmod） | `sudo modprobe tcp_bbr && sudo sysctl -w net.ipv4.tcp_congestion_control=bbr` |
| 注入丢包（netem） | 不可行 | 无 `tc` 命令（iproute2 未装全），且需在目标 netns 内操作 | `sudo tc qdisc add dev lo root netem loss 5%` |
| 网络命名空间隔离 | **可行（实测）** | `unshare -Urnmpf --mount-proc` 可建 userns+netns+veth 并改 MTU；注意 `/sys` 无法重挂（EPERM），MTU 要用 `ip link show` 而非 `/sys/class/net/*/mtu`，且 `ip netns add` 因 `/run` 只读而失败，需改用两个 `unshare -n` 进程 | `sudo ip netns add ns1` |
| socket/epoll/TCP_INFO/ss/proc 观测 | **完全可行** | 纯用户态 | — |
| socket/epoll/TCP_INFO/ss/proc 观测 | **完全可行** | 纯用户态 | — |

---

## 5. 用户态 API ↔ 内核实现对应表

| 用户态接口/现象 | 内核函数 | 源码文件 | 相关字段 |
| --- | --- | --- | --- |
| SYN 到达（LISTEN socket） | `tcp_v4_rcv` → `tcp_v4_do_rcv` → `tcp_rcv_state_process` → `tcp_conn_request` | `net/ipv4/tcp_ipv4.c`、`net/ipv4/tcp_input.c` | `icsk_accept_queue.qlen`、`rsk_timer` |
| SYN+ACK 发送 | `tcp_v4_send_synack` → `ip_queue_xmit` | `net/ipv4/tcp_ipv4.c` | `request_sock.num_retrans` |
| 第三次 ACK → 建连 | `tcp_check_req` → `tcp_v4_syn_recv_sock` → `inet_csk_complete_hashdance` → `tcp_child_process` | `net/ipv4/tcp_input.c`、`tcp_ipv4.c`、`inet_connection_sock.c`、`tcp_minisocks.c` | `rskq_accept_head/tail` |
| `accept()` | `inet_accept` → `inet_csk_accept` | `net/ipv4/af_inet.c`、`inet_connection_sock.c` | `rskq_accept_head`、`sk_state` |
| `ss -lnt` 的 Recv-Q/Send-Q | 同上 | — | `rskq_accept_head` 链表长度 / `sk_max_ack_backlog` |
| `send()` | `tcp_sendmsg` → `tcp_sendmsg_locked` | `net/ipv4/tcp.c` | `write_seq`、`sk_wmem_queued` |
| 真正发包 | `tcp_push` → `__tcp_push_pending_frames` → `tcp_write_xmit` → `tcp_transmit_skb` → `ip_queue_xmit` | `net/ipv4/tcp.c`、`tcp_output.c`、`ip_output.c` | `snd_nxt`、`packets_out`、`snd_cwnd`、`snd_wnd` |
| Nagle 判定 | `tcp_nagle_test` / `tcp_nagle_check` | `net/ipv4/tcp_output.c` | `tp->nonagle` |
| 收到 ACK | `tcp_ack` → `tcp_clean_rtx_queue` | `net/ipv4/tcp_input.c` | `snd_una`、`rttvar_us`、`srtt_us` |
| RTT/RTO 更新 | `tcp_rtt_estimator` → `tcp_set_rto` → `tcp_rearm_rto` | `net/ipv4/tcp_input.c` | `icsk_rto`、`icsk_pending` |
| 快速重传/恢复 | `tcp_fastretrans_alert` → `tcp_enter_recovery` / `tcp_enter_loss` | `net/ipv4/tcp_input.c` | `icsk_ca_state`、`snd_ssthresh` |
| 数据入队 | `tcp_data_queue` → `tcp_queue_rcv` / `tcp_data_queue_ofo` → `tcp_ofo_queue` | `net/ipv4/tcp_input.c` | `rcv_nxt`、`out_of_order_queue` |
| SACK 处理 | `tcp_sacktag_write_queue`、`tcp_parse_options` | `net/ipv4/tcp_input.c` | `sacked_out`、`rx_opt.sack_ok` |
| 唤醒应用 | `tcp_data_ready` → `sk->sk_data_ready` → `sock_def_readable` | `net/ipv4/tcp_input.c`、`net/core/sock.c` | `sk_wq` |
| 窗口通告 | `tcp_select_window` / `__tcp_select_window` | `net/ipv4/tcp_output.c` | `rcv_wnd`、`rcv_wup`、`rcv_ssthresh` |
| 零窗口探测 | `tcp_probe_timer` → `tcp_send_probe0` | `net/ipv4/tcp_timer.c` | `icsk_probes_out` |
| RTO 重传 | `tcp_retransmit_timer` → `tcp_retransmit_skb` | `net/ipv4/tcp_timer.c`、`tcp_output.c` | `icsk_backoff`、`icsk_retransmits` |
| 延迟 ACK | `tcp_delack_timer` → `__tcp_ack_snd_check` | `net/ipv4/tcp_timer.c`、`tcp_input.c` | `icsk_ack.ato`、`icsk_ack.quick` |
| keepalive | `tcp_keepalive_timer` | `net/ipv4/tcp_timer.c` | `keepalive_time/intvl/probes` |
| TIME_WAIT 进入 | `tcp_time_wait` | `net/ipv4/tcp_minisocks.c` | `inet_timewait_sock` |
| TIME_WAIT 状态处理 | `tcp_timewait_state_process` | `net/ipv4/tcp_minisocks.c` | `tw_substate`、`tw_timer` |
| `SO_LINGER{1,0}` → RST | `tcp_close` → `tcp_send_active_reset` | `net/ipv4/tcp.c` | `SOCK_LINGER` |
| SYN cookie | `cookie_v4_init_sequence`、`cookie_v4_check` | `net/ipv4/syncookies.c` | `TcpExt: Syncookies*` |
| 拥塞算法注册/切换 | `tcp_register_congestion_control`、`tcp_set_congestion_control`、`tcp_ca_find` | `net/ipv4/tcp_cong.c` | `icsk_ca_ops` |
| 慢启动/拥塞避免 | `tcp_slow_start`、`tcp_cong_avoid_ai`、`tcp_reno_cong_avoid` | `net/ipv4/tcp_cong.c` | `snd_cwnd`、`snd_ssthresh` |
| CUBIC | `tcp_cubic_*`（`cubictcp_*`） | `net/ipv4/tcp_cubic.c` | `icsk_ca_priv`、`hystart` |
| BBR | `tcp_bbr_*` | `net/ipv4/tcp_bbr.c` | `pacing_rate`、`cong_control` |
| UDP 发送 | `udp_sendmsg` → `udp_send_skb` | `net/ipv4/udp.c` | `udp_sock.pending/len/gso_size` |
| UDP 接收 | `udp_rcv` → `__udp4_lib_rcv` → `udp_queue_rcv_skb` | `net/ipv4/udp.c` | `sk_receive_queue`、`reader_queue` |
| UDP 丢包统计 | `udp_queue_rcv_skb`/`udp_send_skb` 失败分支 | `net/ipv4/udp.c` | `Udp: RcvbufErrors/SndbufErrors/NoPorts` |
| 四元组查表 | `__inet_lookup_established`、`__inet_lookup_listener`、`__inet_lookup` | `net/ipv4/inet_hashtables.c` | `ehash`、`lhash2` |
| bind 冲突判定 | `inet_csk_bind_conflict`、`inet_csk_get_port` | `net/ipv4/inet_connection_sock.c` | `bhash`、`bhash2`、`sk_reuse`、`sk_reuseport` |
| 临时端口分配 | `inet_hash_connect` → `__inet_hash_connect` | `net/ipv4/inet_hashtables.c` | `ip_local_port_range` |
| `TCP_INFO` 取值 | `tcp_get_info`（经 `do_tcp_getsockopt`） | `net/ipv4/tcp.c` | `struct tcp_info` |
| ULP / kTLS | `tcp_ulp_ops`、`tls_init` | `net/ipv4/tcp_ulp.c`、`net/tls/` | `icsk_ulp_ops`、`icsk_ulp_data` |

---

## 6. 常见陷阱、调优旋钮、面试/自测题

### 6.1 陷阱

1. **`tcp_max_syn_backlog` 与 `somaxconn` 都要够大**。
   SYN 队列上限受两者影响（取较大者并受 `sk_max_ack_backlog` 关系影响），
   accept 队列上限是 `min(listen(backlog), somaxconn)`。
   只调 `somaxconn` 不调 `tcp_max_syn_backlog`，突发 SYN 仍会被丢。
2. **accept 队列溢出时默认不回 RST**（`tcp_abort_on_overflow=0`），
   客户端以为连上了、发了请求却超时，这是最难查的一类「假成功」。
   排查入口就是 `TcpExt: ListenOverflows`。
3. **`tcp_tw_reuse` 只对主动连接生效**，且需要 `net.ipv4.tcp_timestamps=1`
   （`tcp_twsk_unique()` 的时间戳检查）。它不解决服务端端口占用问题，
   服务端要用 `SO_REUSEADDR`。
4. **`tcp_fin_timeout` 不是 TIME_WAIT 时长**。它只影响 FIN_WAIT2。
   TIME_WAIT 是固定的 2*MSL（`TCP_TIMEWAIT_LEN`），只能靠复用策略规避。
5. **TIME_WAIT 过多不等于 bug**。它是协议正确性的保证；
   真正要优化的是「短连接数量」或改用连接池，而不是粗暴减小 MSL。
6. **`TCP_NODELAY` 与 `TCP_CORK` 是相反方向**：前者禁用攒批，
   后者强制攒批（适合 `sendfile`/大响应体）。不要在同一个 socket 上反复切换。
7. **`ss -ti` 的 `snd_mss` 可能远大于 1460**（本机回环是 65483），
   那是 TSO/GSO 视角的「分片大小」，不是链路上的 MSS。
8. **`rcv_wnd` 与 `sk_rcvbuf` 不是一回事**：前者是窗口，
   后者是内存字节；换算经过 `scaling_ratio`（`tcp_win_from_space()`）。
9. **重传率要从 `RetransSegs/OutSegs` 算**，而不是单看 `RetransSegs`；
   还要注意 `TCPTimeouts`（RTO）与 `TCPFastRetrans`（快速重传）是不同信号，
   前者说明 RTO 偏保守或丢包严重，后者通常是正常的单包丢失恢复。
10. **UDP 丢包在内核里是静默的**：接收队列满直接丢，
    应用只能靠 `Udp: RcvbufErrors`/`InErrors` 或自己加序号统计。
11. **`SO_REUSEPORT` 需要组内所有 socket 都设置**，且组内
    不能混用不同协议/类型；非特权用户还要求 uid 一致
    （`inet_csk_bind_conflict()` 里的检查）。
12. **SYN cookie 开启后 SYN 队列不再增长**，`ss -lnt` 的
    `Recv-Q` 依然是 0 附近——不要因为看不到半连接就以为没在挨打；
    看 `TcpExt: SyncookiesSent`。

### 6.2 调优旋钮速查

| 旋钮 | 位置 | 典型用途 |
| --- | --- | --- |
| `net.core.somaxconn` | sysctl | accept 队列上限 |
| `net.ipv4.tcp_max_syn_backlog` | sysctl | SYN 队列上限 |
| `net.ipv4.tcp_abort_on_overflow` | sysctl | 队列溢出时是否回 RST（0/1） |
| `net.ipv4.tcp_syncookies` | sysctl | SYN flood 兜底 |
| `net.ipv4.tcp_tw_reuse` | sysctl | 允许复用 TIME_WAIT（主动连接） |
| `net.ipv4.tcp_max_tw_buckets` | sysctl | TIME_WAIT 总上限 |
| `net.ipv4.tcp_fin_timeout` | sysctl | FIN_WAIT2 时长 |
| `net.ipv4.ip_local_port_range` | sysctl | 临时端口范围（客户端连接数上限） |
| `net.ipv4.tcp_rmem` / `tcp_wmem` | sysctl | 单 socket 自动调优区间（min/default/max） |
| `net.core.rmem_max` / `wmem_max` | sysctl | `setsockopt` 能设到的上限 |
| `net.ipv4.tcp_congestion_control` | sysctl | 默认拥塞算法（本机 cubic） |
| `net.ipv4.tcp_moderate_rcvbuf` | sysctl | 接收缓冲自动调优开关 |
| `net.ipv4.tcp_sack` / `tcp_timestamps` / `tcp_window_scaling` | sysctl | 三个关键 TCP 选项（本机均开启） |
| `net.ipv4.tcp_limit_output_bytes` | sysctl | TSQ 上限（治 bufferbloat） |
| `TCP_NODELAY` / `TCP_CORK` / `TCP_QUICKACK` | setsockopt | 攒批与 ACK 行为 |
| `TCP_USER_TIMEOUT` | setsockopt | 重传多久后放弃（覆盖默认 retries2） |
| `TCP_CONGESTION` | setsockopt | 单连接切换拥塞算法 |
| `SO_REUSEADDR` / `SO_REUSEPORT` / `SO_LINGER` | setsockopt | 绑定与关闭语义 |

### 6.3 自测题（≥8 个）

1. 服务端 `listen(2)` 之后来了 SYN，内核分配了什么对象？
   它和最终 `accept()` 返回的 socket 是什么关系？
2. `ss -lnt` 的 `Recv-Q=17`、`Send-Q=16` 说明什么？此时客户端 `connect()` 成功了吗？
3. `tcp_v4_rcv()` 如何区分「发给 LISTEN socket」「发给已建立连接」
   和「发给 TIME_WAIT」三种情况？TIME_WAIT 为什么不需要完整的 `struct sock`？
4. 画出发送一个 4 KB `write()` 的完整路径，标出
   `snd_nxt`/`packets_out`/`snd_cwnd`/`snd_wnd` 分别在哪个函数里被读写。
5. `tcp_write_xmit()` 里 `tcp_cwnd_test()` 与 `tcp_snd_wnd_test()` 分别限制什么？
   两者同时为 0 时内核会挂哪个定时器？
6. 乱序报文进 `out_of_order_queue` 后，什么条件下会被搬到 `sk_receive_queue`？
   `tp->reordering` 影响哪个判断？
7. `tcp_fastretrans_alert()` 在 `TCP_CA_Open → TCP_CA_Recovery` 时对
   `snd_cwnd`/`snd_ssthresh` 做了什么？`undo_cwnd()` 何时被调用？
8. RTO 是如何从 RTT 采样算出来的？为什么重传过的段不能用于 RTT 采样？
9. `ICSK_TIME_DACK`/`ICSK_TIME_RETRANS`/`ICSK_TIME_PROBE0`/`ICSK_TIME_LOSS_PROBE`
   分别对应什么场景？keepalive 为什么不用 `icsk_retransmit_timer`？
10. TIME_WAIT、`tcp_tw_reuse`、`SO_REUSEADDR`、`SO_REUSEPORT` 四者
    分别解决什么问题？它们的判定函数各是谁？
11. 解释 Nagle 与延迟 ACK 的「40 ms 停顿」，给出两种修法并说明各自代价。
12. TSO、GSO、GRO、TSQ 各自发生在哪一侧、解决什么问题？
    为什么它们会改变 `ss -ti` 里 MSS 与 cwnd 的观感？
13. UDP 的 `sk_receive_queue` 满时内核做什么？应用如何发现丢包？
    连接态 UDP（`connect()` 过）与非连接态在收发路径上有什么不同？
14. `inet_hashinfo` 的 `ehash`/`lhash2`/`bhash`/`bhash2` 各自键是什么？
    一次收到数据包最多要走几次查找？

---

## 7. 本章小结 + 延伸阅读

### 小结

- TCP 控制块是 `struct sock → inet_sock → inet_connection_sock → tcp_sock`
  的链式继承；`icsk_accept_queue` 同时承载 SYN 队列（hash + `qlen`）
  与 accept 队列（`rskq_accept_head/tail` 链表）。
- 三次握手的内核主线是
  `tcp_v4_rcv → tcp_v4_do_rcv → tcp_rcv_state_process → tcp_conn_request`
  （半连接）与
  `tcp_check_req → tcp_v4_syn_recv_sock → inet_csk_complete_hashdance →
  tcp_child_process`（全连接），应用侧 `inet_csk_accept()` 只做摘取。
- 发送主线 `tcp_sendmsg → tcp_push → __tcp_push_pending_frames →
  tcp_write_xmit → tcp_transmit_skb → ip_queue_xmit`，
  接收主线 `tcp_v4_rcv → tcp_v4_do_rcv → tcp_rcv_established →
  tcp_data_queue / tcp_ofo_queue → tcp_data_ready → sock_def_readable`。
- 窗口/拥塞的双重约束（`min(snd_cwnd, snd_wnd)`）、
  RTT/RTO 估计（`tcp_rtt_estimator`/`tcp_set_rto`）、
  多用途重传定时器（`icsk_pending`）与 TIME_WAIT 轻量结构
  （`inet_timewait_sock` + `tw_timer`）是 TCP 稳定性的四根支柱。
- UDP 是「无状态、无重传、队列满即丢」的数据报服务，
  但同样复用 `struct sock` 的队列、内存记账与唤醒回调，
  并用 `reader_queue` 降低收发锁争用。

### 延伸阅读

- 内核源码（Linux 6.12）：`net/ipv4/tcp_input.c`、`tcp_output.c`、
  `tcp_timer.c`、`tcp_ipv4.c`、`tcp_minisocks.c`、`tcp_cong.c`、
  `tcp_cubic.c`、`tcp_bbr.c`、`tcp.c`、`tcp_metrics.c`、
  `udp.c`、`inet_connection_sock.c`、`inet_hashtables.c`、`syncookies.c`、
  `net/core/sock.c`、`include/net/tcp.h`、`include/net/request_sock.h`、
  `include/net/inet_connection_sock.h`、`include/net/inet_timewait_sock.h`、
  `include/linux/tcp.h`、`include/linux/udp.h`。
  在线检索入口：<https://elixir.bootlin.com/linux/v6.12/source/>。
- RFC：793（TCP）、1122（主机要求）、1323（窗口缩放/时间戳）、
  2018（SACK）、5681（拥塞控制）、6298（RTO 计算）、
  6675（DSACK）、943（TIME_WAIT/2MSL 相关讨论）。
- 内核文档：`Documentation/networking/`（`tcp_ao.rst`、`msg_zerocopy.rst`、
  `scaling.rst`）；`Documentation/admin-guide/sysctl/net.rst`
  是 sysctl 语义的权威列表。
- 配套实验：
  - `labs/01-socket-api-basics/`：epoll LT/ET、`TCP_INFO`、
    `SO_*` 缓冲加倍、UDP 无连接/连接态对比。
  - `labs/02-tcp-state-machine/`：accept 队列溢出复现、
    TIME_WAIT/RST/`SO_REUSEADDR` 复现、`/proc/net/tcp` 解析器、
    `/proc/net/snmp` 与 `netstat` 计数解读。
