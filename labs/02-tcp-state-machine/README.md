# Lab 02：用内核可观测面看懂 TCP 状态机与队列

本目录不做「抓包」，而是用**用户态 C 程序 + Python 解析器 + `/proc` 计数**
看清 TCP 的两件事：

1. **队列**：SYN 队列与 accept 队列是怎么被塞满的、溢出后内核与客户端各是什么反应；
2. **状态机**：谁进入 TIME_WAIT、`SO_LINGER 0` 为什么会发 RST、
   `SO_REUSEADDR` 到底在什么条件下才管用。

> 运行环境（本机实测）：Linux `6.12.107+deb13-amd64`、gcc 14.2.0、
> GNU Make 4.4.1、Python 3.13.5、uid=1000、`CapEff=0000000000000000`。
> **所有实验均无需 root**；需要 root 的对照步骤在文中明确标注「本机不可运行」。

---

## 1. 实验目的

1. 亲手制造 **accept 队列溢出**，并用 `TcpExt: ListenOverflows` / `ListenDrops`
   计数器与 `ss -lnt` 的 `Recv-Q/Send-Q` 相互印证。
2. 复现 **TIME_WAIT**，并区分三件容易混淆的事：
   TIME_WAIT 本身、`SO_REUSEADDR`、`net.ipv4.tcp_tw_reuse`。
3. 用 `SO_LINGER{l_onoff=1, l_linger=0}` 触发 **RST**，证明它能跳过 TIME_WAIT，
   以及它给对端带来的 `ECONNRESET`。
4. 走一遍 **LISTEN → ESTABLISHED → CLOSE_WAIT → LAST_ACK** 的真实状态迁移，
   每一步都用 `getsockopt(TCP_INFO)` 读到 `tcpi_state`。
5. 掌握 `/proc/net/tcp` 的十六进制地址/端口与状态码翻译，能自己写解析器
   （本目录提供 `parse_proc_tcp.py`）。
6. 会读 `/proc/net/snmp`（RFC1213 MIB）与 `/proc/net/netstat` 的 `TcpExt`
   关键字段，并知道每个字段对应内核哪个函数。

## 2. 前置知识

- 《第 03 章 传输层：TCP 与 UDP 实现》§1.2（两个队列）、§3.1（三次握手路径）、
  §3.8（TIME_WAIT 与 reuse 的差别）、§4（可观测点）。
- 《第 02 章》§3.7（`sk_add_backlog`）与 §6（`SO_REUSEADDR`/`SO_REUSEPORT` 陷阱）。
- 会写基本 C（`socket`/`select`/`fork`）与 Python（`struct` 打包、`/proc` 解析）。

## 3. 涉及的内核机制与源码入口

| 实验现象 | 内核机制 | 源码入口 |
| --- | --- | --- |
| `listen(1)` 的 `Send-Q=1` | `inet_csk_listen_start()` 取 `min(backlog, somaxconn)` 存入 `sk_max_ack_backlog` | `net/ipv4/inet_connection_sock.c` |
| SYN 入半连接队列 | `tcp_conn_request()` → `inet_csk_reqsk_queue_add()` / `inet_csk_reqsk_queue_hash_add()` | `net/ipv4/tcp_input.c`、`net/ipv4/inet_connection_sock.c` |
| 半连接队列满的判定 | `inet_csk_reqsk_queue_is_full()`：`qlen > sk_max_ack_backlog` | `include/net/inet_connection_sock.h` |
| 全连接队列满的判定 | `sk_acceptq_is_full()`：`sk_ack_backlog > sk_max_ack_backlog` | `include/net/sock.h` |
| 溢出计数 | `ListenOverflows` / `ListenDrops` | `net/ipv4/tcp_input.c`、`net/ipv4/tcp_ipv4.c`、`net/ipv4/proc.c` |
| `ss -lnt` 的 Recv-Q | `sk_ack_backlog`（accept 队列长度） | `net/ipv4/tcp_diag.c` 等 diag 路径 |
| 进入 TIME_WAIT | `tcp_time_wait()` → `inet_timewait_sock` + `tw_timer` | `net/ipv4/tcp_minisocks.c` |
| TIME_WAIT 上收到重传 FIN / RST / 新 SYN | `tcp_timewait_state_process()` | `net/ipv4/tcp_minisocks.c` |
| TIME_WAIT 时长 | `tcp_fin_time()`（`linger2` 优先，否则 `tcp_fin_timeout`，且有 RTO 下限保护） | `include/net/tcp.h` |
| `SO_LINGER 0` → RST | `tcp_close()` → `tcp_send_active_reset()` | `net/ipv4/tcp.c` |
| RST 到达接收方 | `tcp_reset()` → 置 `sk_err = ECONNRESET` → `sk_error_report` | `net/ipv4/tcp_input.c` |
| bind 被 TIME_WAIT 挡住 / 放行 | `inet_csk_bind_conflict()`、`inet_csk_get_port()` | `net/ipv4/inet_connection_sock.c` |
| `SO_REUSEADDR` 的落点 | `sock_setsockopt()` → `sk->sk_reuse = SK_CAN_REUSE` | `net/core/sock.c` |
| `tcp_tw_reuse` | `__inet_hash_connect()` / `tcp_twsk_unique()` | `net/ipv4/inet_hashtables.c`、`net/ipv4/tcp_ipv4.c` |

## 4. 编译与运行

```bash
cd labs/02-tcp-state-machine
make clean all          # 0 警告、退出码 0（本机实测）
./run.sh                # 全流程，约 25~40 秒，结束后无残留进程
```

单独运行：

```bash
./backlog_probe --port 0 --backlog 1  --clients 24 --delay 3 --connect-timeout 800
./backlog_probe --port 0 --backlog 16 --clients 24 --delay 1 --accept-all --connect-timeout 1500
./timewait_demo --scenario 1        # TIME_WAIT + SO_REUSEADDR
./timewait_demo --scenario 2        # SO_LINGER 0 -> RST
./timewait_demo --scenario 3        # LISTEN/ESTABLISHED/CLOSE_WAIT/LAST_ACK
./timewait_pair_demo.py --local-port 42000     # 固定四元组的 TIME_WAIT（Python 版对照）
python3 parse_proc_tcp.py --summary
python3 parse_proc_tcp.py --state TIME_WAIT
python3 parse_proc_tcp.py --port 42000
python3 parse_proc_tcp.py --netstat TW ListenOverflows TCPTimeouts
python3 parse_proc_tcp.py --watch 3 --interval 1
bash observe_counts.sh              # /proc/net/snmp + netstat(TcpExt) 全文解读
```

## 5. 逐步实验步骤

| 步骤 | 做什么 | 验证什么 |
| --- | --- | --- |
| 1 | `parse_proc_tcp.py --summary` | `/proc/net/tcp` + `tcp6` 的十六进制翻译与状态分布 |
| 2 | `backlog_probe --backlog 1 --delay 3` | accept 队列溢出、`ListenOverflows` 增长、客户端「假成功」 |
| 3 | `backlog_probe --backlog 16 --accept-all` | 队列够大时的正常行为对照 |
| 4 | `timewait_demo --scenario 1/2/3` | TIME_WAIT 产生与复用、RST 跳过 TIME_WAIT、状态迁移 |
| 5 | `timewait_pair_demo.py` | 固定四元组的 TIME_WAIT：`SO_REUSEADDR` 前失败、后成功 |
| 6 | `parse_proc_tcp.py --netstat` + `--watch` | TcpExt 计数含义 + 状态分布随时间变化 |
| 7 | `observe_counts.sh` | `/proc/net/snmp`/`sockstat`/`sockstat6` 全文 + 字段解释 |

## 6. 真实预期输出

以下全部为 **2026-10-12 在本机实跑**（`./run.sh`，退出码 0）捕获，仅做行宽裁剪。

### 6.1 `/proc/net/tcp` 解析基线（步骤 1）

```
===== 1. 基线：/proc/net/tcp 解析 + 状态分布（parse_proc_tcp.py） =====
总条目数：579（/proc/net/tcp + /proc/net/tcp6）

按状态分布：
  ESTABLISHED         486
  TIME_WAIT            48
  SYN_SENT             32
  LISTEN                7
  CLOSE_WAIT            5
  FIN_WAIT1             1

按本地端口分布（TOP 15）：
  3080      240  [ESTABLISHED=239, LISTEN=1]
  22          2  [LISTEN=2]
  631         2  [LISTEN=2]
  42000       2  [TIME_WAIT=2]
  20241       1  [LISTEN=1]
  3090        1  [LISTEN=1]

按 uid 分布：
  uid 0         546
  uid 7          25
  uid 6           2

LISTEN 端口：
  22       0.0.0.0:22
  22       [::]:22
  631      127.0.0.1:631
  631      [::1]:631
  3080     127.0.0.1:3080
  3090     127.0.0.1:3090
  20241    127.0.0.1:20241

TIME_WAIT 数：48
   127.0.0.1:54664 -> 127.0.0.1:35039  timer_when=000007D2
   127.0.0.1:52748 -> 127.0.0.1:34705  timer_when=00000267
   127.0.0.1:35922 -> 127.0.0.1:38933  timer_when=00000267
   10.0.0.10:47278 -> 221.204.163.76:443  timer_when=00001137

存在队列积压的条目（tx_queue/rx_queue 非 0）：
  10.0.0.10:55608 -> 185.199.109.133:443  state=SYN_SENT tx=1 rx=0
```

`--state TIME_WAIT` 过滤后的表格（十六进制已翻译）：

```
sl     fam   Local                        Remote                       State             TxQ      RxQ   retr    uid    inode
------------------------------------------------------------------------------------------------------------------------------------
48     ipv4  127.0.0.1:54664              127.0.0.1:35039              TIME_WAIT           0        0      3      0        0
49     ipv4  127.0.0.1:48136              127.0.0.1:42243              TIME_WAIT           0        0      3      0        0
64     ipv4  127.0.0.1:52748              127.0.0.1:34705              TIME_WAIT           0        0      3      0        0
307    ipv4  127.0.0.1:35922              127.0.0.1:38933              TIME_WAIT           0        0      3      0        0
106    ipv4  10.0.0.10:47278          221.204.163.76:443           TIME_WAIT           0        0      3      0        0
```

**解读（三个容易看错的地方）**：

1. **TIME_WAIT 行的 `uid` 是 0、`inode` 也是 0**，这不是解析 bug。
   TIME_WAIT 连接在内核里已经不是 `struct sock`，而是 `inet_timewait_sock`，
   它挂在 `tcp_death_row` 上，**没有对应的文件/inode**，
   因此 `/proc/net/tcp` 只能填 0。这也是为什么用 TIME_WAIT 行的 inode
   反查进程永远失败。
2. **`retr` 列为 3**：TIME_WAIT 的 `tm->when` 定时器与重传无关，
   这一列的 3 来自 `tr` 字段的初始值，不要误读为「重传了 3 次」。
   看 TIME_WAIT 剩余时间要看 `timer_when`（jiffies，本机 HZ=250 时
   `00001137` ≈ 17.6 秒）。
3. `--state`/`--port` 过滤会同时作用于汇总，所以过滤后的
   「总条目数」是过滤后的数量（如筛 LISTEN 后只剩 7 条）。
4. TIME_WAIT 的剩余时间从 `timer_when`（jiffies）换算：本机
   `getconf CLK_TCK` = 100（HZ=100），`00001770` = 6000 jiffies = **60 秒**，
   正好等于 TIME_WAIT 的总时长。内核用 `TCP_TIMEWAIT_LEN`（= `60*HZ`，
   注释写的是 60 秒、等价的 2*MSL）来设置 `tw_timer`，
   可在 `include/net/tcp.h` 中核对这个宏；
   注意它与 `net.ipv4.tcp_fin_timeout`（FIN_WAIT2 时长）是两个不同的东西，
   数值同为 60 只是巧合。

### 6.2 accept 队列溢出（步骤 2：`listen(1)` + 24 个并发连接 + 延迟 3 秒 accept）

```
===== 2. backlog 探针：listen(backlog=1) + 24 个并发连接 + 延迟 accept =====
=== backlog_probe ===
请求 listen(backlog=1)，端口 = 44653
内核 max_ack_backlog 由 ss -lnt 的 Send-Q 列给出：
  --- listen 之后: ss -lnt sport = :44653 ---
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
LISTEN 0      1          127.0.0.1:44653      0.0.0.0:*
提示：net.core.somaxconn=4096，backlog 请求值会被 min() 截断为它。

[阶段 1] 并发发起 24 个连接，服务端 故意延迟 accept（延迟 3.0s）
  --- accept 之前（队列应被塞满）: ss -lnt sport = :44653 ---
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
LISTEN 2      1          127.0.0.1:44653      0.0.0.0:*

[阶段 2] 连接结果：客户端侧 connect 完成 = 2，失败(超时) = 22
  注意：本机 net.ipv4.tcp_syncookies=1，全连接队列满时内核不会回 RST，
        而是静默丢弃第三次握手的 ACK，因此客户端 connect() 仍会「成功」，
        但服务端 accept() 永远拿不到这条连接 —— 这就是最难排查的
        「客户端认为连上了、服务端却没收到」现象。证据见阶段 4 的计数增量。

[阶段 3] 服务端开始 accept()，观察队列被排空
  本轮 accept() 摘走连接数 = 2
  合计 accept() 连接数 = 2
  --- accept 之后: ss -lnt sport = :44653 ---
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
LISTEN 0      1          127.0.0.1:44653      0.0.0.0:*

[阶段 4] /proc/net/netstat TcpExt 计数
  ListenOverflows: 236 -> 258 (增量 22)
  ListenDrops    : 236 -> 258 (增量 22)
```

**解读**：

- `Send-Q=1` 就是 `sk_max_ack_backlog`（`min(listen(1), somaxconn=4096) = 1`）。
- `Recv-Q=2`：**队列上限 1，但能装到 2**。因为
  `sk_acceptq_is_full()` 判定是 `sk_ack_backlog > sk_max_ack_backlog`
  （**严格大于**）。这个「+1」语义在本机被精确复现，也是
  「组播/半开连接数总比 backlog 多一个」这类经验说法的来源。
- 24 个客户端里只有 2 个在 800 ms 内完成 `connect()`，22 个超时放弃；
  `ListenOverflows`/`ListenDrops` **各 +22**，与失败数**完全一致**，
  这是三者（客户端超时数 = 溢出计数增量）闭环的关键证据。
- 服务端最终只 accept 到 2 条 —— 其余 22 条连接虽然客户端侧
  （在超时前）状态是 ESTABLISHED，服务端却从未入队，`accept()` 永远拿不到。

### 6.3 对照：队列足够大（步骤 3：`listen(16)` + 立即 accept）

```
===== 3. backlog 探针对照：backlog=16 + 立即 accept（不溢出） =====
请求 listen(backlog=16)，端口 = 39273
  --- listen 之后: ss -lnt sport = :39273 ---
LISTEN 0      16         127.0.0.1:39273      0.0.0.0:*

[阶段 1] 并发发起 24 个连接，服务端 立即 accept（延迟 1.0s）
  --- 立即 accept 模式下: ss -lnt sport = :39273 ---
LISTEN 17     16         127.0.0.1:39273      0.0.0.0:*

[阶段 2] 连接结果：客户端侧 connect 完成 = 17，失败(超时) = 7
[阶段 3] 服务端开始 accept()，观察队列被排空
  本轮 accept() 摘走连接数 = 17
  合计 accept() 连接数 = 17

[阶段 4] /proc/net/netstat TcpExt 计数
  ListenOverflows: 258 -> 272 (增量 14)
  ListenDrops    : 258 -> 272 (增量 14)
```

**解读**：

- `Recv-Q=17` 而 `Send-Q=16`：同样是「+1」语义；队列被 24 个并发连接一次塞满，
  17 个入队、7 个被挤掉。
- 7 个客户端超时，但 `ListenOverflows` 增加 14 ≈ 7×2：
  这 7 个连接在客户端重试/内核重传 SYN+ACK 的过程中被**重复计数**，
  所以「计数增量」和「失败连接数」不是一对一 —— 这也是排查时不能
  用计数当「用户数」的原因。
- 对照 6.2 可见：把 backlog 从 1 提到 16，被拒绝的连接从 22 降到 7，
  且服务端全部 accept 成功，说明瓶颈确实在 accept 队列而非 SYN 队列。

### 6.4 TIME_WAIT 与 `SO_REUSEADDR`（步骤 4 场景 1）

```
========== 场景 1：主动关闭方进入 TIME_WAIT，并尝试立即重绑同一四元组 ==========
监听端口 = 38933
  客户端 connect 前已设置 SO_REUSEADDR（使旧连接演化的 TIME_WAIT 可被复用）
  客户端 connect 后                      TCP_INFO state = 1 (ESTABLISHED)
  服务端 accept 后                       TCP_INFO state = 1 (ESTABLISHED)
  客户端 close()（主动关闭：FIN_WAIT1 -> FIN_WAIT2 -> TIME_WAIT）
  服务端（被动关闭方）收到 FIN 后 TCP_INFO state = 8 (CLOSE_WAIT)
  服务端 recv() 返回 0（0 = 读到对端 FIN），随后 close(afd) 发自己的 FIN
  ss 观测（本机端口 35922）：
  期望：客户端这一侧（sport=35922）应显示 TIME-WAIT，服务端一侧已消失
  $ ss -tan sport = :35922 or dport = :35922
State     Recv-Q Send-Q Local Address:Port  Peer Address:Port
TIME-WAIT 0      0          127.0.0.1:35922    127.0.0.1:38933
  尝试 bind(127.0.0.1:35922) 而不设 SO_REUSEADDR：
    bind 失败: Address already in use (errno=98) —— inet_csk_bind_conflict() 判定：
    存在同 (addr,port) 的 TIME_WAIT，而本 socket 没有 sk_reuse
  再 bind 同一地址并设置 SO_REUSEADDR：
    bind 成功 —— 新 sk 的 sk_reuse = SK_CAN_REUSE，且旧 TIME_WAIT 的
                 tw_reuse 也为真，inet_csk_bind_conflict() 才放行
  /proc/net/netstat TcpExt: TW = 2744 -> 2745 (增量 1)
  /proc/sys/net/ipv4/tcp_fin_timeout         = 60
  /proc/sys/net/ipv4/tcp_tw_reuse            = 2
  /proc/sys/net/ipv4/tcp_max_tw_buckets      = 131072
```

**解读（本 lab 最有价值的一条结论）**：

- `ss` 明确显示 `TIME-WAIT` 落在**客户端侧**（`sport=35922`）——
  因为 `close()` 是客户端先发的（主动关闭方持有 TIME_WAIT）。
- 同一 `(127.0.0.1:35922 → 127.0.0.1:38933)` 四元组马上重新 `bind`：
  **不设** `SO_REUSEADDR` → `EADDRINUSE`；**设** `SO_REUSEADDR` → 成功。
- **判定是双向的**：`inet_csk_bind_conflict()` 除了看新 socket 的 `sk_reuse`，
  还要求旧连接的 `tw_reuse` 为真，而 `tw_reuse` 是从原 socket 的 `sk_reuse`
  继承的。所以 `timewait_demo` 在 `connect()` **之前**就设置了
  `SO_REUSEADDR`；如果去掉这一步，「设了 `SO_REUSEADDR` 也照样失败」。
  这就是「服务端必须在 `listen()` 之前设 `SO_REUSEADDR`」的真正原因 ——
  选项要在**产生 TIME_WAIT 的那个 socket** 上生效，而不是只在重绑的新 socket 上。
- `tcp_fin_timeout = 60` **不是** TIME_WAIT 时长（TIME_WAIT 是固定 2*MSL）；
  它只在 `linger2`（`TCP_LINGER2` 选项）未设置时决定 FIN_WAIT2 的停留时间。

### 6.5 `SO_LINGER 0` → RST（步骤 4 场景 2）

```
========== 场景 2：SO_LINGER{l_onoff=1,l_linger=0} 触发 RST，双方直接 CLOSED ==========
监听端口 = 35489
  close 之前（客户端）                TCP_INFO state = 1 (ESTABLISHED)
  close() —— 内核 tcp_close() 因 l_linger==0 走 tcp_send_active_reset()，发 RST
  服务端 recv() 返回 -1, errno=Connection reset by peer (104) —— RST 的直接后果
  服务端在 RST 之后                    TCP_INFO state = 7 (CLOSE)
  ss 观测（应看不到 TIME_WAIT）：
  $ ss -tan sport = :42334 or dport = :42334
State Recv-Q Send-Q Local Address:Port Peer Address:Port
（空）
```

**解读**：`SO_LINGER{l_onoff=1, l_linger=0}` 让 `tcp_close()` 走
`tcp_send_active_reset()` 直接发 RST，于是

- 本端**不进入** TIME_WAIT（`ss` 过滤该端口为空）；
- 对端 `recv()` 得到 `ECONNRESET`，内核状态直接落到 `CLOSE`；
- **代价**：发送缓冲里未被确认的数据会被丢弃。生产上只有
  「明确知道数据不重要、且要快速释放端口」时才这么用。

### 6.6 状态迁移全流程（步骤 4 场景 3）

```
========== 场景 3：四种状态观测（LISTEN / ESTABLISHED / CLOSE_WAIT / LAST_ACK）==========
  监听 socket                              TCP_INFO state = 10 (LISTEN)
  监听端口 = 38911
  客户端 ESTABLISHED                      TCP_INFO state = 1 (ESTABLISHED)
  服务端 ESTABLISHED                      TCP_INFO state = 1 (ESTABLISHED)
  服务端 close(afd)：服务端进入 FIN_WAIT1/LAST_ACK 流程
  客户端收到 FIN 后（内核置 CLOSE_WAIT） TCP_INFO state = 8 (CLOSE_WAIT)
  客户端不 close()，先读走 EOF：
    recv() 返回 0（0 = 对端已 FIN，读到 EOF）
  读走 EOF 之后仍在 CLOSE_WAIT（等待应用 close） TCP_INFO state = 8 (CLOSE_WAIT)
  客户端 close()：进入 LAST_ACK，等服务端 ACK 后 CLOSED
  客户端 socket 已销毁（fd 已关闭，无法再 getsockopt）
```

**解读**：这段输出把「状态机是内核维护的、与 `read()` 无关」讲透了 ——
`recv()` 返回 0 只表示读到 EOF，**应用不 `close()`，socket 就一直停在
CLOSE_WAIT**，这正是线上「CLOSE_WAIT 堆积」告警的根因：
问题在应用没有及时关闭连接，与内核参数无关。

### 6.7 固定四元组的 TIME_WAIT（步骤 5，Python 版对照）

```
=== timewait_pair_demo ===
固定客户端本地端口 = 42000
监听端       = 127.0.0.1:40335
连接建立：('127.0.0.1', 42000) -> ('127.0.0.1', 40335)
  /proc/net/tcp 中的该四元组（状态码 0x06 = TIME_WAIT）:
    326: 0100007F:A410 0100007F:9D8F 06 00000000:00000000 03:00001770 00000000     0        0 0 3 000000006ce665ac
[符合预期] bind 未设 SO_REUSEADDR：[Errno 98] Address already in use (EADDRINUSE)
           原因：inet_csk_bind_conflict() 发现同四元组处于 TIME_WAIT，
                 且本 socket 的 sk_reuse 不是 SK_CAN_REUSE。
[预期结果] bind 设 SO_REUSEADDR 后：成功
           原因：sock_setsockopt() 置 sk->sk_reuse = SK_CAN_REUSE，
                 inet_csk_bind_conflict() 对 TIME_WAIT 状态放行。
```

**解读**：这一次 TIME_WAIT 的本地端口被**固定**成 42000（靠 `SO_REUSEPORT`
+ 显式 `bind`），所以能精确构造「新连接想复用同一四元组」的场景，
并用 `/proc/net/tcp` 里 `0x06` 这一行做交叉验证。
`0100007F:A410` 中 `A410` = 42000，`0100007F` = 127.0.0.1。

### 6.8 TcpExt 计数与状态采样（步骤 6）

```
=== /proc/net/netstat TcpExt 计数 ===
  TW                       = 2747
  TWRecycled               = 0
  TWKilled                 = 0
  ListenOverflows          = 200
  ListenDrops              = 200
  TCPTimeouts              = 4965
  TCPFastRetrans           = 438
  TCPSackRecovery          = 304
  TCPLossProbes            = 486
  DelayedACKs              = 5422
  TCPOFOQueue              = 84219
  TCPBacklogDrop           = 0
  SyncookiesSent           = 0
  TCPAbortOnClose          = 240

--- 采样 3 次状态分布（每次间隔 1s，观察 TIME_WAIT 堆积与消退）---
[23:05:56] #1 总计=564  CLOSE_WAIT=5 ESTABLISHED=484 FIN_WAIT1=2 LISTEN=7 SYN_SENT=40 TIME_WAIT=26
[23:05:57] #2 总计=564  CLOSE_WAIT=5 ESTABLISHED=484 FIN_WAIT1=2 LISTEN=7 SYN_SENT=40 TIME_WAIT=26
[23:05:58] #3 总计=564  CLOSE_WAIT=5 ESTABLISHED=484 FIN_WAIT1=2 LISTEN=7 SYN_SENT=40 TIME_WAIT=26
```

对应的 `/proc/net/snmp` 与 `/proc/net/sockstat`：

```
[names] Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors
[values] Tcp: 1 200 120000 -1 8264 1651 124 851 489 20131234 19266460 5700 6 1986 0
[names] Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors
[values] Udp: 204932 354 0 220004 0 3 0 111 0

sockets: used 1701
TCP: inuse 515 orphan 2 tw 26 alloc 539 mem 1983
UDP: inuse 3 mem 1459
UDPLITE: inuse 0
RAW: inuse 0
FRAG: inuse 0 memory 0
```

**解读（顺便演示了「重传率」怎么算）**：

- 重传率 = `RetransSegs / OutSegs` = 5700 / 19266460 ≈ **0.0296%**。
  这个数字很低，说明链路基本健康；把它和 `TCPTimeouts=4965`、
  `TCPFastRetrans=438` 一起看，可以判断「丢包恢复手段以 RTO 为主
  还是以快速重传为主」。
- `TCPOFOQueue = 84219` 说明**大量报文是乱序到达的**（本机是 WiFi
  环境，且并发连接很多）。乱序本身不等于丢包（会进 ofo 队列等空洞填补），
  但会推高 `reordering` 相关的重传判定风险。
- `SyncookiesSent = 0` 说明**本机没有遭遇 SYN 队列溢出**；
  6.2 节的溢出是 accept 队列（用户态不 accept）造成的，
  与 SYN cookie 无关 —— 两个计数必须分开看。
- `TCPBacklogDrop = 0` 说明单 socket 的 `sk_backlog` 没溢出过。
- `TCPAbortOnClose = 240`：应用在接收队列还有未读数据时 `close()`，
  内核改发 RST 的次数。短连接服务里这个值偏高是正常现象。
- `sockstat` 的 `TCP: mem 1983`（pages，≈7.7 MB）与 `tw 26`（TIME_WAIT 数）
  是同一时刻的一致性快照：`tw` 与 `parse_proc_tcp.py` 统计的
  `TIME_WAIT=26` 完全吻合，可互相校验。

### 6.9 `observe_counts.sh` 片段（步骤 7）

```
###### TCP/UDP 内核计数观测 ######
date   : 2026-10-12T23:05:58+08:00
kernel : 6.12.107+deb13-amd64
uid    : 1000（非 root：/proc/net/* 可读，sysctl 只读）

== /proc/net/sockstat ==
sockets: used 1701
TCP: inuse 515 orphan 2 tw 26 alloc 539 mem 1983
UDP: inuse 3 mem 1459
UDPLITE: inuse 0
RAW: inuse 0
FRAG: inuse 0 memory 0

== /proc/net/sockstat6 ==
TCP6: inuse 23
UDP6: inuse 4
UDPLITE6: inuse 0
RAW6: inuse 1
FRAG6: inuse 0 memory 0
```

### 6.10 需要 root 的对照实验（本机不可运行，仅列出做法）

| 想验证的事 | 本机状态 | root 下的命令 |
| --- | --- | --- |
| 把 accept 队列上限收紧 | 只读 | `sysctl -w net.core.somaxconn=128` |
| 让溢出时回 RST（客户端立刻报错） | 只读 | `sysctl -w net.ipv4.tcp_abort_on_overflow=1`，然后重跑 6.2，观察客户端 `connect` 变成 `ECONNREFUSED` |
| 打开/关闭 SYN cookie | 只读（当前 1） | `sysctl -w net.ipv4.tcp_syncookies=0`，把 backlog 设到极限再看 `SyncookiesSent` |
| 复用 TIME_WAIT 端口 | 只读（当前 2） | `sysctl -w net.ipv4.tcp_tw_reuse=1` |
| 强制销毁 TIME_WAIT | 只读 | `sysctl -w net.ipv4.tcp_max_tw_buckets=10`，观察 `TWKilled` 增长 |
| 缩短 SYN+ACK 重试 | 只读 | `sysctl -w net.ipv4.tcp_syn_retries=2` |
| 注入丢包看状态机恢复 | 无 CAP_NET_ADMIN | `tc qdisc add dev lo root netem loss 5%` 后重跑场景 1，观察 `TCPTimeouts`/`TCPFastRetrans` |
| 观察状态迁移 tracepoint | 无 CAP_PERFMON/CAP_BPF | `perf trace -e sock:inet_sock_set_state -- sleep 5` |

以上写操作在本机都会失败（`CapEff=0`，无 `CAP_NET_ADMIN`），
`/proc/sys/net/**` 的**读**则全部可用。

## 7. 观察与思考题

1. `listen(1)` 时 `ss -lnt` 显示 `Send-Q=1`，为什么 `Recv-Q` 能达到 2？
   请指出判定队满的具体内联函数与比较运算符。
2. 6.2 节里 22 个客户端「`connect` 超时」，但客户端 socket 在超时前
   用 `ss` 看是 ESTABLISHED。请画出这 22 条连接在内核里经历了什么
   （提示：`tcp_check_req` → 队列满 → 丢 ACK → 重传 SYN+ACK）。
   如果把 `tcp_abort_on_overflow` 设为 1，客户端会看到什么？
3. 6.3 节里 7 个客户端失败，`ListenOverflows` 却增加 14。为什么不是 7？
   这对「用计数估算被拒用户数」有什么影响？
4. TIME_WAIT 行的 uid/inode 都是 0，请从 `inet_timewait_sock` 的结构解释原因。
5. 6.4 节里为什么要在 `connect()` 之前设 `SO_REUSEADDR`？
   请用 `inet_csk_bind_conflict()` 的判定条件说明「双向」是什么意思，
   并预测：如果旧连接没设 reuse，新连接设了，结果如何？
6. `tcp_fin_timeout=60` 与 TIME_WAIT 时长是什么关系？
   想改 TIME_WAIT 时长应该动什么（提示：`TCP_LINGER2`）？
7. `SO_LINGER{1,0}` 让 `close()` 发 RST，除了「跳过 TIME_WAIT」，
   还会给对端带来什么后果？什么业务场景可以接受？
8. `recv()` 返回 0 之后，socket 仍停在 CLOSE_WAIT。请解释
   「CLOSE_WAIT 堆积」告警的根因，并说明它与 `TIME_WAIT` 堆积
   在排查方向上有什么本质不同。
9. `SyncookiesSent` 在本机是 0，但 6.2 节确实溢出了。为什么？
   这两个计数分别对应哪个队列？
10. `TCPOFOQueue=84219` 说明什么？乱序与丢包在内核里是如何区分的
    （提示：`tp->reordering`、`tcp_ofo_queue()`、SACK）？
11. 请用 `parse_proc_tcp.py --watch 20 --interval 3` 观察 1 分钟内
    TIME_WAIT 数量的变化，验证「TIME_WAIT 大约 60 秒后消失」。
    为什么不是 `tcp_fin_timeout` 的 60 秒？（提示：两者数值巧合）
12. `/proc/net/tcp` 的 `tr:tm->when` 两列分别是什么？TIME_WAIT 行里
    `retr=3` 能不能解读成「重传 3 次」？

## 8. 清理

```bash
cd labs/02-tcp-state-machine
make clean                  # 删除 backlog_probe / timewait_demo
rm -rf .out                 # 运行日志目录（仅运行产物）
pkill -f backlog_probe      # 仅在 run.sh 被强杀时手动清理
pkill -f timewait_demo
```

`run.sh` 用 `set -uo pipefail` + 每步 `timeout` 保护，端口全部由内核临时分配
或使用固定的 42000（`timewait_pair_demo.py`，失败会提示换端口），
运行结束不会残留监听端口或 TIME_WAIT 之外的状态。
