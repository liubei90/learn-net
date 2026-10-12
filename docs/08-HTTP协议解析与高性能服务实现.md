# 08 · HTTP 协议解析与高性能服务实现

> 本章要回答一个看起来简单、但决定了整个 Web 性能架构的问题：
> **内核把字节流交给你之后，「一条 HTTP 消息」的边界是谁定的？**
> 答案：没有别人，只有你自己。理解这一点，你就能理解 `Content-Length`、`chunked`、
> 请求走私、Slowloris，以及 HTTP/2 为什么要换掉内核 TCP（HTTP/3 over QUIC）。

**定位**：协议栈最上层（应用层）。前置：[docs/01 全景](01-Linux网络子系统全景与数据流.md) 的收发路径、
[docs/03 传输层](03-传输层-TCP与UDP实现.md) 的 TCP 字节流语义。
配套实验：[labs/07-http-parser](../labs/07-http-parser/)（含可直接运行的解析器与服务器）。

---

## 0. 本章定位

```
应用层      ← 本章在这里：HTTP 语义、消息边界、解析器状态机
───────────  syscall 边界（read/write/sendfile/epoll/io_uring）
套接字层     struct socket / struct sock
传输层  TCP  可靠、有序、字节流；不提供消息边界
网络层  IP
链路层  驱动/NAPI
```

关键事实：**内核提供的是「有序字节流」，不是「消息流」**。
`recv(fd, buf, 4096)` 返回 4096 字节与「一个 HTTP 请求」之间**没有任何对应关系**：

- 一次 `recv` 可能拿到 1.7 个请求（需要接着解析剩余 0.7 个）；
- 也可能只拿到半个请求头（要等下一次可读事件）；
- 甚至可能在第 3 个字节处切断 `\r\n`。

因此 HTTP 服务器必须为**每条连接**维护一个解析器状态，跨 `recv` 调用记住进度。
这与内核在 `struct tcp_sock` 里跨 skb 记住 `rcv_nxt` 是同一类问题——只是层次不同。

---

## 1. 核心概念与设计权衡

### 1.1 消息边界的三种定法

| 方式 | 谁决定长度 | 优点 | 缺点 | 典型用途 |
|---|---|---|---|---|
| `Content-Length: N` | 发送前就知道 | 简单、可预分配、可复用连接 | 必须先知道总长；边生成边发就不行 | 静态文件、API 响应 |
| `Transfer-Encoding: chunked` | 写在数据流里 | 无需预先知道长度，可流式 | 解析更复杂；HTTP/2 已移除 | 动态生成、代理转发 |
| 连接关闭（`Connection: close`） | 靠 EOF | 实现最简单 | 无法复用连接，无法区分「正常结束」与「被截断」 | HTTP/1.0、极简实现 |

**HTTP/1.1 的默认规则**：`Content-Length` 与 `chunked` 二选一；两者同时出现必须视为错误
（[RFC 7230 §3.3.3](https://www.rfc-editor.org/rfc/rfc7230)，这是请求走私的入口）。

### 1.2 chunked 的确切格式

```
<hex 长度>[;扩展]\r\n
<恰好那么多字节的数据>\r\n
<hex 长度>[;扩展]\r\n
<恰好那么多字节的数据>\r\n
0\r\n
[trailer 头部...]
\r\n
```

三个容易踩的坑：

1. 长度是**十六进制**，`A` = 10（`labs/07` 里专门有用例）。
2. 长度后面可以跟 `;name=value` 扩展，必须跳过。
3. `0\r\n` 之后是 **trailer 区**，以空行结束——不是 `0\r\n\r\n` 就完事（很多实现漏了 trailer）。

### 1.3 为什么「文本协议 + 无边界」会带来安全问题

| 问题 | 成因 | 防御层 |
|---|---|---|
| 请求走私（request smuggling） | 前后端对 `CL`/`TE` 冲突的取舍不同 → 消息边界不一致 | 应用层：拒绝冲突；网关统一规范化 |
| Slowloris | 慢慢发头，内核老实缓存，连接长期占用 | 应用层：idle 超时 + 头上限；内核：`tcp_rmem` 总量控制 |
| 头部膨胀 | 一个头几 MB | 应用层：`max_header_bytes`、`max_headers` |
| 反序列化虫洞 | 解析器实现差异 | 用经过审计的解析器；对同名头/异常编码严格处理 |

> **安全原则**：解析器必须「**严格到可以拒绝**」。
> [`labs/07`](../labs/07-http-parser/) 里对重复 `Content-Length`、`CL`+`TE` 并存都直接 400。

### 1.4 从内核视角看 HTTP 的演进

这是本章最值得记住的一张演进图——**每一代 HTTP 都在试图绕开内核 TCP 的一个限制**：

```
HTTP/1.0  一请求一连接
   └─ 问题：三次握手 + TIME_WAIT 开销；解决方法：Connection: keep-alive
HTTP/1.1  持久连接 + pipelining
   └─ 问题：pipelining 有「队头阻塞(HOL)」——响应必须按序返回，一个慢响应卡住后面所有请求
            浏览器干脆不用 pipelining，改用「每域名 6 条并行连接」
   └─ 内核侧代价：连接数暴涨 → 端口耗尽、TIME_WAIT 堆积、somaxconn/backlog 压力
HTTP/2    在 TCP 之上做二进制分帧 + 多路复用
   └─ 解决了应用层 HOL，但没解决 TCP 层 HOL：
            一个 TCP 段丢包 → 内核按序交付阻塞 → 所有流一起等（内核只认字节序）
   └─ 内核侧代价：内核完全不知道「帧」的存在，它看到的仍是一条字节流
HTTP/3    把可靠传输搬到用户态的 QUIC over UDP
   └─ 多路复用与重传都在用户态实现，TCP 层 HOL 消失
   └─ 内核侧代价：内核只看到 UDP；于是内核为 UDP 补 GSO/GRO 等加速
            （UDP_SEGMENT / UDP_GRO），把分段/合并卸载给网卡
```

**这解释了几件事**：

- 为什么内核代码里 UDP 路径这些年一直在加优化（UDP GSO、`sendmsg` 批量）——因为 HTTP/3、QUIC、游戏、音视频把大量流量搬到了 UDP。
- 为什么「应用层协议设计」和「内核协议栈实现」是互相塑形的：协议改一层，内核要跟着加卸载能力。
- 为什么容器/网关的性能调优总在 `somaxconn`、`tcp_tw_reuse`、`ip_local_port_range` 上打转——
  HTTP/1.1 的连接模型直接决定了这些内核计数器的压力。

---

## 2. 关键数据结构与内核接口

### 2.1 应用层解析器（本教程的实现模型）

对应 [`labs/07-http-parser/http_parser.h`](../labs/07-http-parser/http_parser.h)：

```c
typedef struct {
    /* 已解析出的消息字段 */
    http_method_t method;  char method_str[16];
    char *target; size_t target_len;
    int version_major, version_minor;
    http_header_t headers[100]; size_t num_headers;
    int keep_alive, expect_continue, chunked, has_content_length;
    unsigned long long content_length;

    /* 消息体（chunked 会被还原成原始字节） */
    char *body; size_t body_len;

    /* 状态机：跨 recv 调用保存进度 —— 这是与内核 tcp_sock 对等的部分 */
    http_state_t state;
    char *line; size_t line_len;      /* 正在拼装的行 */
    unsigned long long remaining;     /* CL 或当前 chunk 剩余 */
    int crlf_state;

    /* 安全上限 */
    size_t max_line, max_headers, max_header_bytes, max_body;
} http_parser_t;
```

**状态机全貌**（`labs/07` 的实际实现，建议对照源码读）：

```
                 ┌──────────────┐
                 │ HP_ST_START  │
                 └──────┬───────┘
                        ▼
              ┌───────────────────┐  读到 '\n'   ┌──────────────────┐
              │ HP_ST_REQUEST_LINE│─────────────▶│ 解析请求行        │
              └───────────────────┘              │ 0.9 → DONE       │
                        ▲                        │ 否则 → HEADERS   │
                        │                        └──────────────────┘
                        │                                 ▼
              ┌───────────────────┐  空行      ┌──────────────────┐
              │  HP_ST_HEADERS    │───────────▶│ finish_headers() │
              └───────────────────┘            └───┬───────┬──────┘
                                                   │       │
                              chunked ─────────────┘       └───────── CL=N
                                 ▼                                     ▼
                       ┌──────────────────┐                  ┌────────────────┐
                       │ HP_ST_CHUNK_SIZE │                  │ HP_ST_BODY_CL  │
                       └───┬──────────┬───┘                  └───────┬────────┘
                      size=0│         │size>0                          │ 收满 N 字节
                            ▼         ▼                                ▼
                 ┌────────────────┐  ┌──────────────────┐        ┌──────────┐
                 │ HP_ST_TRAILERS │  │ HP_ST_CHUNK_DATA │        │HP_ST_DONE│
                 └───────┬────────┘  └────────┬─────────┘        └──────────┘
                         │ 空行               │ 收满一个 chunk
                         │                    ▼
                         │           ┌───────────────────┐
                         │           │ HP_ST_CHUNK_CRLF  │──▶ 回到 CHUNK_SIZE
                         │           └───────────────────┘
                         ▼
                   HP_ST_DONE ──▶ 返回 COMPLETE，调用者 reset 后解析下一条
```

### 2.2 内核侧真正参与 HTTP 性能的那些旋钮

这些不是「HTTP 的参数」，而是**内核为通用 TCP/UDP 提供的接口**，恰好决定了 HTTP 服务的性能上限：

| 内核机制 | 位置 | 对 HTTP 服务的意义 |
|---|---|---|
| `sk_receive_queue` + `sk_rcvbuf` | `net/core/sock.c`、`include/net/sock.h` | 应用读得慢 → 内核队列满 → 丢包 → TCP 重传。`ss` 的 `Recv-Q` 持续 > 0 就是这个信号 |
| `sk_sndbuf` / `wmem` | 同上 | 大响应写不动时 `write()` 阻塞/返回 `EAGAIN`，`ss -tmi` 的 `wmem` |
| `TCP_NODELAY` | `tcp_setsockopt` → `icsk->icsk_ack.pingpong` | 关掉 Nagle。小响应下避免「Nagle + 延迟 ACK」的几十毫秒叠加 |
| `TCP_CORK` / `MSG_MORE` | `tcp_push` 的 `nonagle` 判定 | 强制攒包：把「响应头 + 响应体」凑成一个 TCP 段发出去 |
| `writev()` | `sock_sendmsg` → `tcp_sendmsg` 的 iov 遍历 | 一次 syscall 发出「头 + 体」，减少系统调用与 TCP 段数 |
| `sendfile()` | `do_sendfile` → `splice`/`sendpage` | 文件 → socket **不经过用户态**，省掉 2 次拷贝；对大文件/静态服务影响巨大 |
| `splice()`/`tee()` | `fs/splice.c` | 管道零拷贝，代理转发场景 |
| `SO_REUSEPORT` | `net/core/sock.c` → `inet_csk_get_port` | 多进程各自 listen，内核按哈希分发，避免 `accept()` 惊群 |
| `TCP_DEFER_ACCEPT` | `net/ipv4/tcp_minisocks.c` | 等第一个数据包到达才唤醒 `accept()`，避免空连接开销 |
| `SO_RCVLOWAT` / `SO_RCVTIMEO` | `net/core/sock.c` | 攒够 N 字节才可读 / 读超时，减少事件次数；也是慢速连接的防线之一 |
| `epoll`（`ep_poll_callback`） | `fs/eventpoll.c` | `sk_data_ready → sock_def_readable → ep_poll_callback`，就绪通知路径 |
| `io_uring` | `io_uring/`、`net/socket.c` | 批量提交/完成，避免每请求一次 syscall |
| `MSG_ZEROCOPY` | `net/ipv4/tcp.c` `tcp_sendmsg_locked` | 发送大 body 时避免用户态→内核态拷贝（需处理 completion 通知） |
| `SO_BUSY_POLL` | `net/core/dev.c` | 低延迟场景忙轮询，用 CPU 换尾延迟 |

### 2.3 一次 HTTP 请求在内核里的完整旅程（把 01 章的图收窄）

```
客户端 write("GET / HTTP/1.1\r\n...")          客户端内核
   └─ tcp_sendmsg → tcp_write_xmit → tcp_transmit_skb → ip_queue_xmit
        → ip_output → neigh_output → dev_queue_xmit → 网卡
                    ↓ 网络
服务端网卡 → NAPI → netif_receive_skb → ip_rcv → ip_local_deliver
        → tcp_v4_rcv → tcp_rcv_established → tcp_data_queue
        → sk->sk_data_ready = sock_def_readable
        → ep_poll_callback（把 socket 挂进 epoll 就绪链表）
                    ↓
服务端 epoll_wait 返回 → recv(fd, buf, 65536)   ← 内核拷贝 skb 数据到用户态
   └─ tcp_recvmsg → skb_copy_datagram_msg → 多个 skb 被拼成一段连续字节
                    ↓
应用层 HTTP 解析器：把这段字节喂进状态机（本实验的核心）
   └─ 得到一个完整请求 → 生成响应 → writev/sendfile
                    ↓
响应再次走一遍内核发送路径
```

**注意加粗的那一步**：`recv()` 返回的 buf 里，是内核把 `sk_receive_queue` 上**多个 skb**
拼起来的连续字节。这就是「TCP 是字节流」在实现层面的含义——skb 边界被抹平了。

---

## 3. 高性能 HTTP 服务的内核视角

### 3.1 架构选型的本质：怎么用有限的核处理大量连接

| 模型 | 内核机制依赖 | 优点 | 瓶颈 |
|---|---|---|---|
| 一连接一线程 | `clone()` | 编程简单 | 每连接 8MB 栈 + 上下文切换；几百连接就崩 |
| `select`/`poll` | 每次调用遍历全部 fd | 兼容性好 | O(n) 扫描，fd 上限 1024（select） |
| `epoll` LT | `ep_poll_callback` | 编程简单，不易漏事件 | 每次就绪都通知，事件多时开销高 |
| `epoll` ET | 同上 + 必须读到 `EAGAIN` | 事件少、效率高 | 容易漏事件、编程难度高 |
| `epoll` + 线程池 | 同上 + `SO_REUSEPORT` | 利用多核，避免惊群 | 需处理跨线程写竞争（`EPOLLONESHOT`） |
| `io_uring` | 环形提交/完成队列 | 批量 syscall、真异步 | 内核版本要求高、编程复杂 |
| 协程/用户态调度 | `epoll` + `setjmp`/`ucontext` | 同步写法异步性能 | 阻塞调用必须全部改写 |

**`SO_REUSEPORT` 为什么重要**：传统 `accept()` 惊群是「一个连接到达，所有进程被唤醒，
只有一个成功」。`SO_REUSEPORT` 让内核在 `bind` 阶段就把连接**按哈希分发**到某个 socket，
从根本上消除了惊群，也天然做了负载均衡（对应内核里的 `reuseport_select_sock`）。

### 3.2 减少拷贝与系统调用：HTTP 服务优化的主战场

| 手段 | 省掉什么 | 何时用 |
|---|---|---|
| `writev` 一次发包 | 1 次 syscall、可能少 1 个 TCP 段 | 响应头 + 小 body |
| `TCP_CORK` | 减少小包（但会增加延迟，注意 200ms 上限） | 明确要攒包时 |
| `sendfile` | 2 次内存拷贝 + 2 次上下文切换 | 静态文件 |
| `splice` | 同上，可走管道 | 代理转发 |
| `MSG_ZEROCOPY` | 用户态→内核态拷贝 | 大 body（>10KB 量级） |
| `io_uring` 批量 | N 次 syscall | 高 IOPS 场景 |
| `TCP_NODELAY` | 延迟（Nagle 攒包） | 小响应、低延迟优先 |

**权衡**：`TCP_NODELAY` 与 `TCP_CORK` 是相反方向的旋钮。默认 `TCP_NODELAY` 关闭 + Nagle 开启，
小包会被攒着；某些服务因此出现「偶发几十毫秒延迟」。而 `TCP_CORK` 打开后又可能引入新延迟。
**没有银弹，只有测量**——这正是 [docs/09 观测与调优](09-内核网络观测与性能调优.md) 存在的理由。

### 3.3 内核参数与 HTTP 服务的对应关系

| 现象 | 内核计数器/参数 | 调优方向 |
|---|---|---|
| 高并发下新连接被拒 | `somaxconn`、`tcp_max_syn_backlog`、`ss -lnt` 的 `Send-Q` | 调大队列；但先确认真的是队列瓶颈而不是应用 `accept` 慢 |
| 「Cannot assign requested address」 | `ip_local_port_range`、`tcp_tw_reuse`、TIME_WAIT 数量 | 扩大端口范围、启用 `tcp_tw_reuse`（仅客户端侧有效）、复用连接 |
| 响应慢、`Recv-Q` 持高 | `tcp_rmem`、应用读取速度 | 先修应用，再考虑缓冲 |
| 丢包重传 | `netdev_max_backlog`、`softnet_stat`、ring buffer | 见 [docs/05](05-链路层-网卡驱动-NAPI与中断.md)、[docs/09](09-内核网络观测与性能调优.md) |
| 突发流量打成 SYN flood | `tcp_syncookies`、SYN 队列 | 开 syncookies；上 synproxy |

---

## 4. 可观测点与验证方法

```bash
# ① 应用层：解析器自己报告（labs/07 的 /headers 路由）
curl -s http://127.0.0.1:8080/headers

# ② socket 层：内核队列与 TCP 细节
ss -lnt                                   # 监听队列 Send-Q/Recv-Q
ss -tni                                   # cwnd / rtt / 窗口 / 拥塞算法
ss -tmi                                   # 内存（wmem/rmem）与拥塞状态

# ③ 传输层计数器
cat /proc/net/snmp                        # Tcp: RetransSegs / InErrs / OutRsts
cat /proc/net/netstat | grep TcpExt       # ListenOverflows / ListenDrops

# ④ 连接明细（十六进制地址:端口 + 状态码）
cat /proc/net/tcp

# ⑤ 丢包与重传的根因（需 root / tracefs）
bpftrace -e 'kretprobe:tcp_retransmit_skb { @[kstack] = count(); }'
bpftrace -e 'tracepoint:skb:kfree_skb { @[args->reason] = count(); }'

# ⑥ 解析器本身的验证（本实验最推荐的方式）
make -C labs/07-http-parser test          # 2.7 万断言：整包/逐字节/穷举切分
```

第 ⑥ 条要强调：**HTTP 解析器的正确性必须靠「分片穷举」测试**，
而不是「手工点几下浏览器」。因为生产环境的 bug 几乎都只在不巧的分片下出现。

---

## 5. 用户态 API ↔ 内核实现对应表

| 用户态 | 内核入口 | 关键实现 | 语义要点 |
|---|---|---|---|
| `socket(AF_INET, SOCK_STREAM, 0)` | `__sock_create` → `inet_create` | `net/ipv4/af_inet.c` | 建立 `struct socket` + `struct tcp_sock` |
| `bind()` | `inet_bind` | `__inet_bind` | 端口分配规则、`SO_REUSEADDR` 语义 |
| `listen(fd, backlog)` | `inet_listen` | `inet_csk_listen_start` | backlog 是**全连接队列**长度，受 `somaxconn` 截断 |
| `accept()` | `inet_accept` → `inet_csk_accept` | `tcp_check_req` | 从 accept 队列取；队列空则阻塞或 `EAGAIN` |
| `recv()` | `inet_recvmsg` → `tcp_recvmsg` | `tcp_recvmsg_locked` | 从 `sk_receive_queue` 摘 skb 并拷贝；`MSG_PEEK`/`MSG_WAITALL` |
| `send()`/`writev()` | `inet_sendmsg` → `tcp_sendmsg` | `tcp_sendmsg_locked` | 缓冲不足会阻塞（`sk_stream_wait_memory`） |
| `sendfile()` | `do_sendfile` | `net/socket.c` | 文件→socket，内核内零拷贝 |
| `epoll_wait()` | `do_epoll_wait` | `fs/eventpoll.c` | 由 `sk_data_ready` 回调驱动就绪 |
| `setsockopt(TCP_NODELAY)` | `do_tcp_setsockopt` | `net/ipv4/tcp.c` | 关 Nagle |
| `setsockopt(TCP_CORK)` | 同上 | `tcp_push` 的 `nonagle` | 攒包 |
| `setsockopt(SO_REUSEPORT)` | `sock_setsockopt` | `reuseport_alloc` | 内核侧连接分发 |
| `close()`/`shutdown()` | `inet_release` | `tcp_close` | FIN/`TIME_WAIT`/`SO_LINGER` 决定 RST |

---

## 6. 常见陷阱与自测题

**陷阱清单**

1. **假设一次 `recv` == 一个请求**。这是初学 HTTP 服务器最常见、也最难复现的 bug。
2. **用 `strlen` 判断 body 长度**。body 里可能有 `\0`（表单、二进制上传）。
3. **不设上限**。`max_line`/`max_header_bytes`/`max_body` 缺一个就能被单连接打爆内存。
4. **不设 idle 超时**。Slowloris 用极慢的速度发头，你的连接池会被耗尽。
5. **忽略 `CL` + `TE` 冲突**。这是请求走私的入口。
6. **响应不按请求顺序返回**。HTTP/1.1 pipelining 要求有序，乱序会让客户端彻底错乱。
7. **在数据路径上做阻塞日志**。一次同步写磁盘就能把 P99 拖垮。
8. **只用浏览器测**。必须做分片穷举测试与畸形报文测试。

**自测题**

1. 内核给的是字节流，为什么 `Content-Length` 是必需的设计而不是「多余」的？
2. 一次 `recv` 返回 0 意味着什么？返回 `-1/EAGAIN` 意味着什么？两者的区别为什么关键？
3. `chunked` 的 trailer 区在什么场景下有用？（提示：`Trailer` 头 + 边生成边计算校验和）
4. 为什么 HTTP/2 的多路复用仍然有队头阻塞？HTTP/3 是怎么解决的？内核在其中扮演什么角色？
5. `SO_REUSEPORT` 与「先 `bind` 再 `fork`」相比，为什么能消除 `accept` 惊群？
6. `sendfile` 相比 `read`+`write` 省掉了哪两次拷贝？和 `skb` 的页分片（`skb_shinfo->frags`）有什么关系？
7. `TCP_NODELAY` 与 `TCP_CORK` 分别往哪个方向调？能同时开吗？
8. `ss -lnt` 里监听套接字的 `Send-Q` 一直很大，说明什么？该调哪个参数？
9. 你的服务 `ss -tni` 显示 `app_limited`，这告诉你瓶颈在哪一层？
10. 如果要支持 10 万并发长连接，你会先怀疑哪三个内核参数？（提示：fd 上限、内存、`somaxconn`）

<details>
<summary>参考答案要点</summary>

1. 因为 TCP 不提供消息边界。没有 CL 或 chunked，接收方无法区分「这个请求结束了」与「连接还没发完」，
   只能靠关闭连接（HTTP/1.0 做法），从而无法复用连接。
2. 0 = 对端发了 FIN（有序关闭，已收到全部数据）；`EAGAIN` = 现在没数据但连接还在。
   把 `EAGAIN` 当作 EOF 会截断请求；把 0 当作 `EAGAIN` 会死循环。
3. 需要在发送完 body 之后才能确定、但又要随消息一起传递的头部（如动态计算的 `Content-MD5`、
   `Digest`）。HTTP/2 用 `trailers` 帧取代。
4. HTTP/2 在 TCP 上多路复用，TCP 只保证**字节流**的按序交付：一个段丢失，其后所有字节都不能交付，
   于是所有流一起等待。HTTP/3 把重传/排序搬到用户态 QUIC over UDP，每条流独立。
   内核此时只是「UDP 的搬运工」，因此要为 UDP 提供 GSO/GRO 等卸载来补性能。
5. `SO_REUSEPORT` 在内核 `bind` 阶段就建立了「一个监听地址 → 多个 socket」的映射，
   新连接由内核按四元组哈希**直接交给**某个 socket 的 accept 队列，不唤醒其他进程。
6. 省掉「内核页缓存 → 用户态缓冲」和「用户态缓冲 → socket 发送缓冲」两次拷贝。
   内核侧数据直接以页的形式挂进 `skb_shinfo->frags`（页分片），而不是拷进线性区。
7. `TCP_NODELAY` 关闭 Nagle（立即发小包）；`TCP_CORK` 强制攒包（等够一个 MSS 或 200ms）。
   两者语义相反，通常只开一个。
8. 全连接队列（accept queue）积压 = 应用 `accept()` 太慢。先查应用（是否有阻塞操作、
   是否单线程 accept）。参数侧是 `somaxconn` + `listen()` 的 backlog 取值。
9. 发送速率受「应用供给数据的速度」限制，而不是拥塞窗口——也就是说瓶颈在应用层
   （磁盘 IO、上游依赖、CPU），不在网络。
10. `net.core.somaxconn`、`net.ipv4.ip_local_port_range` 与 `tcp_tw_reuse`、
    以及 fd 上限 `ulimit -n` / `fs.file-max`；（服务端还要看 `tcp_max_syn_backlog`）。
</details>

---

## 7. 本章小结

- 内核 TCP 提供**可靠有序的字节流**，消息边界是应用层的责任——一切 HTTP 复杂性的根源。
- 消息边界有三种定法（CL / chunked / EOF），HTTP/1.1 要求严格拒绝 `CL`+`TE` 冲突。
- HTTP 解析器必须是**增量状态机**，并用「穷举分片」测试来保证正确性。
- 高性能的核心是**减少拷贝与系统调用**（`writev`/`sendfile`/`splice`/`MSG_ZEROCOPY`/`io_uring`）
  和**正确的并发模型**（`epoll` + `SO_REUSEPORT`）。
- 应用层与内核层要**各自设上限**：应用防慢速与超大报文，内核总量控制靠 `tcp_rmem`/`somaxconn`。
- HTTP/1.1 → 2 → 3 的演进，本质是不断绕开内核 TCP 的字节流语义（先绕应用层 HOL，再绕 TCP 层 HOL）。

## 8. 延伸阅读

- RFC 7230 / RFC 9112（HTTP/1.1 语义与消息体长度）；RFC 9113（HTTP/2）；RFC 9000（QUIC）
- `Documentation/networking/`；`man 7 tcp`、`man 7 socket`、`man 2 sendfile`、`man 2 splice`
- 配套实验：[labs/07-http-parser](../labs/07-http-parser/)（解析器 + 服务器 + 内核侧观测）
- 相关章节：[docs/03 传输层](03-传输层-TCP与UDP实现.md)（拥塞控制与窗口）、
  [docs/09 观测与调优](09-内核网络观测与性能调优.md)（丢包与延迟定位）
