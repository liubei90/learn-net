# Lab 01：套接字 API 基础与内核观测

本目录用**纯用户态程序 + 内核可观测面**，把《第 02 章 套接字层与用户态接口》
里讲的每一个结论都实测一遍：`getsockopt` 反推内核字段、`ss` 看连接与内存记账、
`/proc` 看队列与统计。**全部实验都不需要 root**。

> 运行环境（本机实测）：Linux `6.12.107+deb13-amd64`（Debian 13）、
> gcc 14.2.0、GNU Make 4.4.1、Python 3.13.5、uid=1000、
> `CapEff=0000000000000000`（无 CAP_NET_ADMIN/CAP_NET_RAW/CAP_BPF）。

---

## 1. 实验目的

1. 走通 `socket → bind → listen → accept → send/recv → close` 的用户态生命周期，
   并把每个调用对应到内核函数（`inet_create`、`inet_csk_get_port`、`inet_csk_accept`、
   `tcp_sendmsg`、`tcp_recvmsg`、`tcp_close`）。
2. 亲手确认 `SO_SNDBUF`/`SO_RCVBUF` 的**内核加倍语义**，以及它与
   `net.core.{r,w}mem_max`、`net.ipv4.tcp_{r,w}mem` 的关系。
3. 用 `epoll` 实现同一份逻辑的 **LT 与 ET** 两种触发模式，理解
   「ET 必须读到 `EAGAIN`」这条硬规则。
4. 用 `getsockopt(TCP_INFO)` 读取内核 `struct tcp_info` 的**真实字段**
   （state / cwnd / snd_wnd / rtt / retransmits / sacked），
   而不是靠猜。
5. 对比 UDP **无连接**与**连接态**两种收发方式，观察
   `ECONNREFUSED`（ICMP port unreachable）这类只有连接态 UDP 才能感知的错误。
6. 学会读 `ss -lnt`、`ss -tni`、`ss -tme`、`/proc/net/tcp`、
   `/proc/net/sockstat`、`/proc/net/snmp`、`/proc/net/netstat`。

## 2. 前置知识

- 《第 02 章》全部；至少读过其中的 `struct socket`/`struct sock` 双层抽象、
  `sock_init_data()`、`sk_mem_charge()`/`sk_mem_allocated`、
  `sk_data_ready` 回调链。
- 基本 C 语言与 POSIX socket API（`socket(2)`、`epoll(7)`）。
- 会看 `ss` 输出。

## 3. 涉及的内核机制与源码入口

| 实验现象 | 内核机制 | 源码入口 |
| --- | --- | --- |
| `socket()` 返回 fd | `sock_create` → `inet_create` → `sk_alloc` → `sock_init_data` | `net/socket.c`、`net/ipv4/af_inet.c`、`net/core/sock.c` |
| `SO_SNDBUF` 设 65536 读出 131072 | `sock_setsockopt` 的 `val*2` 与上限 clamp | `net/core/sock.c` |
| `SO_RCVBUF` 影响通告窗口 | `tcp_select_window` 用 `sk_rcvbuf` 计算窗口 | `net/ipv4/tcp_output.c`、`net/ipv4/tcp_input.c` |
| `TCP_NODELAY` | `do_tcp_setsockopt` 置 `tp->nonagle = TCP_NAGLE_OFF` | `net/ipv4/tcp.c`、`net/ipv4/tcp_output.c` |
| `getsockopt(TCP_INFO)` | `do_tcp_getsockopt` → `tcp_get_info` 填 `struct tcp_info` | `net/ipv4/tcp.c`、`include/uapi/linux/tcp.h` |
| `ss -tme` 的 `skmem` | `sk_mem_charge/sk_mem_uncharge` 记账 `sk_forward_alloc` | `include/net/sock.h`、`net/core/sock.c` |
| `/proc/net/sockstat` 的 `TCP: mem` | `proto->memory_allocated` 汇总 | `net/ipv4/tcp.c`、`net/ipv4/proc.c` |
| `sendto`/`recvfrom` | `udp_sendmsg` → `udp_send_skb`、`__udp4_lib_rcv` → `udp_queue_rcv_skb` | `net/ipv4/udp.c` |
| 连接态 UDP 收到 ICMP 报错 | `sk_error_report` + `SO_ERROR` | `net/ipv4/udp.c`、`net/ipv4/ip_sockglue.c` |
| epoll 唤醒 | `sk_data_ready` → `sock_def_readable` → `ep_poll_callback` | `net/core/sock.c`、`fs/eventpoll.c` |

## 4. 编译与运行

```bash
cd labs/01-socket-api-basics
make clean all          # 必须 0 警告、退出码 0（本机实测：4 个目标全部干净编译）
./run.sh                # 全流程实跑，端口由内核临时分配，结束时自动清理
```

单个程序也可以独立运行：

```bash
./tcp_echo_server --port 0 --trigger lt --verbose     # stdout 第一行给出实际端口
./tcp_echo_server --port 0 --trigger et --verbose     # 对比 ET
./tcp_client --host 127.0.0.1 --port <P> --count 4 --size 4096
./tcp_client --host 127.0.0.1 --port <P> --count 200 --size 4096 --sndbuf 65536 --sleep-after 3
./udp_echo server --port 0
./udp_echo client --port <P> --count 3 --size 512
./udp_echo client --port <P> --count 2 --size 4096 --connect
./sockopt_probe
```

编译环境注意：Debian 13 的 glibc `<netinet/tcp.h>` 是较旧的副本，
缺少 `tcpi_snd_wnd` / `tcpi_bytes_acked` / `tcpi_segs_out` 等新字段，
因此源码统一使用内核 UAPI 头 `#define _NETINET_TCP_H 1` + `#include <linux/tcp.h>`。

## 5. 逐步实验步骤

`run.sh` 会自动完成下面 7 步；本节说明每一步在验证什么。

| 步骤 | 做什么 | 验证什么 |
| --- | --- | --- |
| 0 | 起 `tcp_echo_server`（epoll LT，`--port 0`） | `ss -lnt` 的 `Recv-Q/Send-Q` 与 `somaxconn` |
| 1 | 4×4096B 收发，不开 `TCP_NODELAY` | `TCP_INFO` 基础字段、Nagle 生效时的段数 |
| 2 | 8×65536B 收发，开 `TCP_NODELAY` | 大包吞吐、`snd_wnd` 增长到 ~1 MB、MSS 被 TSO 放大 |
| 3 | 设置 64 KB 收发缓冲 + `--sleep-after 3` | **加倍语义**、连接存续期的 `ss -tnie`/`ss -tme` |
| 4 | 短连接 + 立即关闭 | 主动关闭方进入 TIME_WAIT（`ss -tan state time-wait`） |
| 5 | UDP 无连接 / 连接态收发；向未监听端口发包 | UDP 路径、`NoPorts` 计数、`ECONNREFUSED` |
| 6 | `sockopt_probe` 全量探针 | 默认值、加倍表、内存记账、`SO_REUSE*`、其它选项 |
| 7 | 收尾检查 | 确认没有遗留监听/连接 |

## 6. 真实预期输出

以下全部是 **2026-10-12 在本机实跑**（`./run.sh`，退出码 0）捕获的输出，
只做了行宽裁剪（端口号、吞吐量、RTT 每次运行都会不同，属于正常波动）。

完整原始日志保存在 `.out/final_run.log`（本次运行产物），可用
`./run.sh > .out/final_run.log 2>&1` 重新生成后与本节逐条比对。

### 6.1 启动与监听队列（步骤 0）

```
###### lab 01-socket-api-basics 实跑记录 ######
date      : 2026-10-12T22:53:35+08:00
kernel    : 6.12.107+deb13-amd64
gcc       : gcc (Debian 14.2.0-19) 14.2.0
uid/caps  : uid=1000 CapEff=0000000000000000
port range: 32768 60999

===== 0. TCP 服务端启动（epoll LT，端口由内核临时分配） =====
LISTENING 127.0.0.1:33955 trigger=lt pid=25
[22:53:35.238] listen fd=5 epfd=6 mode=lt
[run.sh] TCP 监听端口 = 33955

--- 观测 1: ss -lnt（LISTEN 状态，Recv-Q 是全连接队列当前长度，Send-Q 是 backlog 上限）---
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
LISTEN 0      128        127.0.0.1:33955      0.0.0.0:*

--- 观测 2: ss -lntp / 内核 backlog 上限 ---
State  Recv-Q Send-Q Local Address:Port  Peer Address:PortProcess
LISTEN 0      128        127.0.0.1:33955      0.0.0.0:*    users:(("tcp_echo_server",pid=25,fd=5))
/proc/sys/net/core/somaxconn = 4096
```

**解读**：`listen(fd, 128)` 的 128 被内核采纳为 `sk_max_ack_backlog`（Send-Q=128），
因为 `min(128, somaxconn=4096) = 128`。`Recv-Q=0` 说明 accept 队列为空。
`--port 0` 让内核从临时端口范围（`32768 60999`）里挑端口，避免实验脚本端口冲突。

### 6.2 `TCP_INFO` 基础（步骤 1：4×4096B，无 `TCP_NODELAY`）

```
connect() -> 127.0.0.1:33955  count=4 size=4096 nodelay=0
connect ok in 0.080 ms; 本地端口 = 51340 (来自 ip_local_port_range)
  SO_SNDBUF(实测)  = 2626560
  SO_RCVBUF(实测)  = 131072
  TCP_NODELAY(实测)= 0
  TCP_MAXSEG(实测) = 32741
round-trips=4 bytes_sent=16384 bytes_recv=16384 elapsed=0.714 ms throughput=21.89 MB/s
  --- TCP_INFO (内核 struct tcp_info) ---
  tcpi_state          = 1
  tcpi_ca_state       = 0
  tcpi_retransmits    = 0  (当前正在重传的报文数)
  tcpi_total_retrans  = 0  (连接生命周期累计重传)
  tcpi_snd_cwnd       = 10  (拥塞窗口，单位 MSS)
  tcpi_snd_ssthresh   = 2147483647
  tcpi_snd_wnd        = 102016  (对端通告窗口，字节)
  tcpi_rcv_space      = 65495  (本端接收自动调优空间)
  tcpi_rtt            = 56 us
  tcpi_rttvar         = 39 us
  tcpi_rto            = 204000 us
  tcpi_sacked         = 0  (SACK 标记的报文数)
  tcpi_lost           = 0
  tcpi_unacked        = 0
  tcpi_snd_mss        = 51008
  tcpi_rcv_mss        = 4096
  tcpi_pmtu           = 65535
  tcpi_advmss         = 65483
  tcpi_segs_out       = 7
  tcpi_segs_in        = 6
  tcpi_bytes_acked    = 16385
  tcpi_bytes_received = 16384
  tcpi_notsent_bytes  = 0
```

**解读**：

- `tcpi_state = 1` 即 `TCP_ESTABLISHED`；
- `tcpi_snd_cwnd = 10` 是 Linux 的初始窗口 `TCP_INIT_CWND = 10`（`include/net/tcp.h`）；
- `tcpi_snd_ssthresh = 2147483647` 即 `TCP_INFINITE_SSTHRESH`，说明还没经历过丢包；
- `tcpi_rto = 204000 us` ≈ 204 ms，由 `tcp_set_rto()` 算出，受
  `net.ipv4.tcp_rto_min`（本机实测下限 200 ms）约束；
- `tcpi_snd_mss = 51008` 远大于以太网 1460：这是 **TSO/GSO 视角**的分片大小，
  因为回环 MTU 是 65535（`tcpi_pmtu = 65535`）；
- `tcpi_rcv_space = 65495` 是内核准备通告给对端的接收空间，
  它由 `sk_rcvbuf` 经 `tcp_win_from_space()` 折算而来，
  与下面 `SO_RCVBUF=131072` 不是同一个量纲。

### 6.3 大包 + `TCP_NODELAY`（步骤 2：8×65536B）

```
connect() -> 127.0.0.1:33955  count=8 size=65536 nodelay=1
  TCP_NODELAY(实测)= 1
  TCP_MAXSEG(实测) = 32741
round-trips=8 bytes_sent=524288 bytes_recv=524288 elapsed=1.001 ms throughput=499.33 MB/s
  tcpi_snd_wnd        = 1011200  (对端通告窗口，字节)
  tcpi_snd_mss        = 65483
  tcpi_rcv_mss        = 65483
  tcpi_segs_out       = 28
  tcpi_segs_in        = 26
  tcpi_bytes_acked    = 524289
  tcpi_bytes_received = 524288
```

**解读**：开了 `TCP_NODELAY` 后 `tcpi_rcv_mss` 从 4096 跳到 65483，
说明对端（server，`TCP_NODELAY=1`）以整 MSS 灌数据；`segs_out` 只有 28
而送了 524288 字节，平均每段 ≈ 18 KB —— 这正是 TSO 把多个 MSS 合并进一个
skb 的效果。`snd_wnd` 涨到 ~1 MB，说明对端接收缓冲被自动调优放大了。

### 6.4 缓冲加倍与 `ss` 观测（步骤 3：请求 64 KB 缓冲 + 挂住 3 秒）

```
===== 3. 客户端显式设置缓冲区：观察内核加倍语义 + 连接存续期的 ss -tni =====
--- 观测 3: ss -tnie 连接级 TCP_INFO（-e 打印 skmem 记账、-i 打印 cwnd/rtt/retrans）---
ESTAB      0      0          127.0.0.1:51368       127.0.0.1:33955 uid:1000 ino:316487 sk:d00b ...
	 ts sack cubic wscale:7,1 rto:204 rtt:0.014/0.001 ato:40 mss:65483 pmtu:65535 rcvmss:4096
	 advmss:65483 cwnd:10 bytes_sent:819200 bytes_acked:819201 bytes_received:819200
	 segs_out:204 segs_in:203 data_segs_out:200 data_segs_in:200
ESTAB      0      0          127.0.0.1:33955       127.0.0.1:51368 uid:1000 ino:311837 sk:d00d ...
	 ts sack cubic wscale:1,7 rto:208 rtt:5.147/10.269 ato:40 mss:50948 pmtu:65535 rcvmss:4096
	 advmss:65483 cwnd:10 bytes_sent:819200 bytes_acked:819200 bytes_received:819200
	 segs_out:202 segs_in:204 data_segs_out:200 data_segs_in:20

--- 观测 3b: ss -tm（socket memory，对应 sk_mem_charge/sk_forward_alloc 记账）---
	 skmem:(r0,rb131072,t0,tb483840,f0,w0,o0,bl0,d0)
ESTAB      0      0          127.0.0.1:51368       127.0.0.1:33955
	 skmem:(r0,rb131072,t0,tb131072,f0,w0,o0,bl0,d0)
	 skmem:(r0,rb2637248,t0,tb2626560,f0,w0,o0,bl0,d0)
ESTAB      0      0          127.0.0.1:33955       127.0.0.1:51368
	 skmem:(r0,rb3210017,t0,tb2626560,f0,w0,o0,bl0,d0)

--- 观测 3c: ss -tln（LISTEN 的 Recv-Q/Send-Q 与 backlog）---
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
LISTEN 0      128        127.0.0.1:33955      0.0.0.0:*

--- 客户端自述（getsockopt 实测值）---
  SO_SNDBUF(实测)  = 131072
  SO_RCVBUF(实测)  = 131072
  TCP_NODELAY(实测)= 0
  TCP_MAXSEG(实测) = 32741
  [注意] 请求 SO_SNDBUF=65536，内核实测=131072（加倍语义）
  [注意] 请求 SO_RCVBUF=65536，内核实测=131072（加倍语义）
--sleep-after 3.00s：保持连接 ESTABLISHED，供外部 ss -tni / /proc 抓快照
```

**解读**：

- **加倍语义成立**：请求 65536 → 实测 131072。内核 `sock_setsockopt()` 把值乘 2
  存入 `sk_sndbuf`/`sk_rcvbuf`，`getsockopt` 再除 2 呈现；超过
  `net.core.wmem_max`/`rmem_max`（本机 212992）的请求会被 clamp 后乘 2。
- `skmem` 字段含义（iproute2 的命名）：`r=sk_rcvbuf`、`rb=sk_rmem_alloc`、
  `t=sk_wmem_queued`、`tb=sk_sndbuf`、`f=sk_forward_alloc`、`w=sk_wmem_alloc`、
  `o=sk_omem_alloc`、`bl=sk_backlog.len`、`d=sk_drops`。
  注意 `rb`/`tb` 容易记反，本机输出 `rb131072` 就是接收队列已用字节。
- 服务端那条连接的 `tb=2626560`（≈2.5 MB）不是我们设置的 64 KB，
  而是 TCP **发送缓冲自动调优**（`tcp_sndbuf_expand()`）的结果；
  客户端因为显式设了 `SO_SNDBUF`，`sk_userlocks` 带上了 `SOCK_SNDBUF_LOCK`，
  所以被固定在 131072 —— 这就是「显式设置会关闭自动调优」的直接证据。
- 服务端 `rtt:5.147/10.269` 明显高于客户端 `rtt:0.014`，
  因为教学用服务器是单线程、边收边回写，ACK 被处理延迟了；
  这类「应用层造成的 RTT 虚高」在真实服务中同样常见。

### 6.5 TIME_WAIT 与 `/proc`（步骤 4）

```
--- 观测 4: ss -tan state time-wait ---
Recv-Q Send-Q Local Address:Port    Peer Address:Port
0      0          127.0.0.1:51354      127.0.0.1:33955
0      0          127.0.0.1:51340      127.0.0.1:33955
0      0          127.0.0.1:51368      127.0.0.1:33955
0      0          127.0.0.1:60862      127.0.0.1:33955

--- 观测 4b: /proc/net/netstat TcpExt 的 TW/TWRecycled/TWKilled ---
  TW                       = 2502
  TWRecycled               = 0
  TWKilled                 = 0

--- 观测 5: /proc/net/tcp（十六进制，只取前 10 行）---
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000 65534        0 7464 ...
   1: 0100007F:B5DD 00000000:0000 0A 00000000:00000002 00:00000000 00000000  1000        0 308773 ...
   7: 0100007F:EAEC 0100007F:0C08 01 00000000:00000000 02:00000032 00000000  1000        0 285957 ...

--- 观测 6: /proc/net/snmp 的 Tcp 行 ---
Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors
Tcp: 1 200 120000 -1 7161 1466 118 790 515 19504473 18744688 2361 6 1404 0
```

**解读**：

- 4 条 `127.0.0.1:51340/51354/51368/60862 → 127.0.0.1:33955` 就是本实验
  四次短连接的主动关闭方（客户端）残留的 TIME_WAIT，说明客户端是主动关闭方。
- `/proc/net/tcp` 里 `0100007F:B5DD` 是十六进制小端：`0100007F` = 127.0.0.1，
  `B5DD` = 46557；`st=0A` = LISTEN。
- `/proc/net/snmp` 的 `Tcp:` 行：`ActiveOpens=7161`、`PassiveOpens=1466`、
  `RetransSegs=2361`、`OutSegs=18744688` → 重传率约 0.0126%（本机还有其它流量）。
- 服务端退出时打印的最后一条 `TCP_INFO on-peer-fin state=CLOSE_WAIT(8)`：
  对端（客户端）已 `close()`，服务端读到 EOF 时内核状态是 CLOSE_WAIT，
  这一帧是「被动关闭方读走 EOF」的直接证据。

### 6.6 UDP：无连接 vs 连接态（步骤 5）

```
UDP_ECHO_LISTENING 127.0.0.1:33212 rcvbuf=425984 pid=57
--- 观测 7: ss -lun（UDP 监听，Recv-Q 为接收队列中排队的 skb 字节数）---
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
UNCONN 0      0          127.0.0.1:33212      0.0.0.0:*

  #0 sent=512 recv=512 rtt=0.148 ms payload="PING0uuu"
  #1 sent=512 recv=512 rtt=0.109 ms payload="PING1uuu"
  #2 sent=512 recv=512 rtt=0.113 ms payload="PING2uuu"
UDP client done: 3/3 成功往返

connected UDP: send()/recv() 现在可用，且能收到 ECONNREFUSED
  #0 sent=4096 recv=4096 rtt=0.172 ms payload="PING0uuu"
  #1 sent=4096 recv=4096 rtt=0.119 ms payload="PING1uuu"
UDP client done: 2/2 成功往返

--- 观测 8: UDP 统计 /proc/net/snmp 的 Udp 行 ---
Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors
Udp: 200226 349 0 215513 0 3 0 107 0

--- 观测 8b: 向未监听端口发包触发 ICMP port unreachable ---
recv #0 failed: Connection refused
UDP client done: 0/1 成功往返
--- 再次读统计（NoPorts 增长）---
Udp: 200226 350 0 215514 0 3 0 107 0
```

**解读**：

- `UDP_ECHO_LISTENING ... rcvbuf=425984`：请求 256 KB 的 `SO_RCVBUF`，
  回读 425984 ≈ 416 KB —— 注意 UDP 的 `SO_RCVBUF` 也走同一套加倍 + clamp 逻辑。
- `ss -lun` 的 `Recv-Q` 是 UDP 接收队列里**排队的字节数**，与 TCP 的
  「连接个数」语义完全不同。
- 向未监听端口发 UDP 并 `connect()` 后，`recv()` 返回 `Connection refused`：
  这是内核把 ICMP port unreachable 通过 `sk_error_report` 投递回 socket 的结果；
  同时 `/proc/net/snmp` 的 `Udp: NoPorts` 从 349 涨到 350 —— 两个观测面互相印证。
- 非连接态 UDP 收不到这个错误（没有 `sk_error_report` 的目标），
  只有连接态才能感知。

### 6.7 `sockopt_probe` 全量输出（步骤 6）

```
=== sockopt_probe: 用 getsockopt 与 /proc 反推内核缓冲与选项语义 ===

[1] 默认缓冲区 / sysctl 上限
  sysctl net.core.rmem_default = 212992
  sysctl net.core.wmem_default = 212992
  sysctl net.core.rmem_max     = 212992
  sysctl net.core.wmem_max     = 212992
  新 TCP socket 实测 SO_SNDBUF=16384 SO_RCVBUF=131072
  → 新建 SOCK_STREAM 时 sk_sndbuf/sk_rcvbuf 取自 net.core.wmem_default/rmem_default，
    再被 tcp_wmem/tcp_rmem 的自动调优区间约束（见 net/ipv4/tcp.c: tcp_init_sock()）。

[2] SO_SNDBUF / SO_RCVBUF 加倍语义实测
   请求值 | SO_SNDBUF 回读       | SO_RCVBUF 回读
        1024 | 4608 (被截断)       | 2304 (被截断)
        4096 | 8192 (=2x)             | 8192 (=2x)
       65536 | 131072 (=2x)           | 131072 (=2x)
      100000 | 200000 (=2x)           | 200000 (=2x)
      200000 | 400000 (=2x)           | 400000 (=2x)
     1048576 | 425984 (被截断)     | 425984 (被截断)
```

**解读**（这部分是本章最反直觉的地方）：

- **小值也被改写**：请求 1024 得到 4608（发送）/2304（接收）。
  因为 `tcp_init_sock()` 把 `sk_sndbuf` 初始化为 `tcp_wmem[1]`、
  `sk_rcvbuf` 初始化为 `tcp_rmem[1]`，`sock_setsockopt()` 的 clamp 逻辑
  保证不会低于一个最小值 —— 所以「请求 1 KB」不可能真的生效。
- **大值被上限截断**：请求 1 MB 得到 425984 = 2 × 212992（`wmem_max`）。
  即先 clamp 到 212992，再乘 2 存进内核，回读时除 2 又乘 2 呈现为 425984。
- **`net.core.rmem_max` 才是 `setsockopt` 的天花板**，而
  `net.ipv4.tcp_rmem[2]` 是**未显式设置时**自动调优的天花板。
  两者混用是最常见的调优错误。

```
[3] 缓冲区与 /proc/net/sockstat 内存记账
  基线：
  /proc/net/sockstat  TCP inuse=489 mem=2827 pages
  已推送 491117 字节到被动方 sk_receive_queue（未 recv）后：
  /proc/net/sockstat  TCP inuse=492 mem=2827 pages
  主动方 SO_SNDBUF=425984  被动方 SO_RCVBUF=425984 (请求 2097152，受 rmem_max/wmem_max=212992 限制)
  关闭连接后：
  /proc/net/sockstat  TCP inuse=489 mem=2827 pages
```

**解读（重要，避免误读）**：`TCP: mem=2827 pages` 在推送 491 KB 前后**没有变化**。
原因有两层：

1. `mem` 的单位是 page（4 KiB），2827 pages ≈ 11 MB，其中绝大部分是
   **本机其它进程**（编辑器、浏览器、本 harness 自身的连接）的 TCP 内存，
   本实验的 ~0.5 MB 被淹没在噪声里；
2. 更关键的是，`sk_mem_charge()` 记的是 `sk_forward_alloc`（socket 私有预分配），
   **按协议汇总到 `proto->memory_allocated` 是异步/批量的**，
   单次瞬时采样看不到稳定增量。

**正确做法**：要么用 `ss -tme` 看单个 socket 的 `rb`（`sk_rmem_alloc`）——
我们在 §6.4 已经看到 `rb3210017` 这样直观的数字；
要么在大量连接/大流量下做长时间的差分统计。
本 lab 保留这个「看起来没变化」的输出，正是为了演示**只看全局汇总计数会得出错误结论**。

```
[4] SO_REUSEADDR 与 SO_REUSEPORT 行为对比
  A. 两个 socket 都不设置任何选项：
    第一个监听已 bind+listen 到 127.0.0.1:56973
    bind(SO_REUSEADDR=0 SO_REUSEPORT=0) -> Address already in use
    bind(SO_REUSEADDR=1 SO_REUSEPORT=0) -> Address already in use
    bind(SO_REUSEADDR=0 SO_REUSEPORT=1) -> Address already in use
    bind(SO_REUSEADDR=1 SO_REUSEPORT=1) -> Address already in use
  B. 两个 socket 都设置 SO_REUSEPORT：应能同时 bind+listen（内核做负载均衡）
    第一个监听 127.0.0.1:38775 (REUSEADDR+REUSEPORT)
    bind(SO_REUSEADDR=1 SO_REUSEPORT=1) -> 成功（可共存/可抢占）
      listen() -> 成功
```

**解读**（这份输出精确刻画了两者的边界）：

- A 组说明：**只要第一个 socket 没有设置 `SO_REUSEPORT`**，
  后面无论怎么设置都无法绑定同一 `(127.0.0.1, port)` —— 因为
  `inet_csk_bind_conflict()` 要求**双方**都满足条件。
  同时它也证明 **`SO_REUSEADDR` 不能解决「两个 LISTEN 抢同一端口」**，
  它只解决 TIME_WAIT 占用与通配/具体地址共存。
- B 组说明：双方都设 `SO_REUSEPORT` 时，同一个 `(addr, port)` 可以同时
  `bind` + `listen`，由内核按四元组哈希把新连接分给其中一个 socket。

```
[5] 其它常用选项的读写回显
  TCP_NODELAY          = 0
  TCP_MAXSEG           = 536
  TCP_KEEPIDLE         = 7200
  TCP_SYNCNT           = 6
  TCP_USER_TIMEOUT     = 0
  SO_KEEPALIVE         = 0
  SO_LINGER(struct)    = {l_onoff=0, l_linger=0}
  SO_REUSEADDR         = 0
  SO_REUSEPORT         = 0
  SO_SNDBUF            = 16384
  SO_RCVBUF            = 131072
  SO_SNDTIMEO(struct)  = {tv_sec=0, tv_usec=0}
```

**解读**：`TCP_MAXSEG = 536` 是**未连接** socket 的默认值（IPv4 最小 MSS），
连接后才会被真正的 MSS 替换（我们在 §6.2 看到 `TCP_MAXSEG(实测) = 32741`）；
`TCP_KEEPIDLE = 7200` 说明内核把 `net.ipv4.tcp_keepalive_time` 直接反映在
未设置的 socket 上（`keepalive_time_when()` 的回退逻辑）；
`SO_SNDBUF=16384` = `tcp_wmem[1]`，`SO_RCVBUF=131072` = `tcp_rmem[1]`
（本机 `/proc/sys/net/ipv4/tcp_wmem = 4096 16384 4194304`、
`tcp_rmem = 4096 131072 6291456`）。

### 6.8 收尾检查（步骤 7）

```
===== 7. 收尾：确认无遗留监听/连接 =====
Netid State  Recv-Q Send-Q                    Local Address:Port  Peer Address:Port
udp   UNCONN 0      0                               0.0.0.0:46510      0.0.0.0:*
udp   UNCONN 0      0                               0.0.0.0:5353       0.0.0.0:*
tcp   LISTEN 0      128                             0.0.0.0:22         0.0.0.0:*
tcp   LISTEN 0      4096                          127.0.0.1:20241      0.0.0.0:*
```

**解读**：本实验创建的监听端口与连接全部消失（`run.sh` 的 `trap cleanup`
负责杀进程），只剩系统自带的服务 —— 说明无损退出、没有污染环境。

## 7. 观察与思考题

1. 步骤 1 与步骤 2 的 `tcpi_rcv_mss` 分别是 4096 和 65483，为什么差这么多？
   它是由哪一方、在哪个函数里决定的？
2. `tcpi_snd_cwnd = 10` 的 10 从哪来？什么条件下它会涨到 20、40、80？
3. 请求 `SO_RCVBUF=1024` 为什么回读 2304 而不是 2048？请说出内核里
   参与决策的至少两个常量/sysctl。
4. 为什么服务端 socket 的 `sk_sndbuf`（`ss -tme` 的 `tb`）会自己涨到 2.5 MB，
   而客户端被固定在 131072？请指出 `sk_userlocks` 里具体的位。
5. 把 `tcp_echo_server` 的 `--trigger` 从 `lt` 改成 `et` 再跑步骤 2。
   观察服务端的 `in=/out=` 日志：ET 模式下 `epoll_wait` 的返回次数是否变少？
   如果把 `conn_handle_read()` 里的 `continue` 改成 `break`（即不读到 `EAGAIN`），
   会出现什么现象？请实际改一下并给出结论。
6. `/proc/net/sockstat` 的 `TCP: mem` 在推送 0.5 MB 后没有变化。
   请设计一个能稳定观测到它变化的实验（提示：用 `ss -tme` 的 `rb` 与
   多连接/大 `SO_RCVBUF`），并解释为什么单个连接的瞬时变化会被掩盖。
7. `SO_REUSEADDR` 在本实验中没能让两个 LISTEN 共存，那它到底在什么场景下有用？
   请构造一个实验证明「TIME_WAIT 挡住 bind，加 `SO_REUSEADDR` 后成功」。
   （提示：`timewait_pair_demo.py` 做了这件事，先自己想再看。）
8. 连接态 UDP 收到 `ECONNREFUSED`，非连接态收不到。请从内核角度解释：
   这个错误是谁产生、经由哪个回调、存到了哪里、`recv` 从哪里取出来？
9. `/proc/net/tcp` 的 `tx_queue:rx_queue` 与 `ss -tme` 的 `skmem` 分别反映什么？
   为什么两者不可互相替代？
10. 如果这一套程序跑在只有 IPv6 `::1` 的机器上，哪些地方需要改？
    （提示：`AF_INET6`、`/proc/net/tcp6`、`IPV6_V6ONLY`、地址 hex 解析。）

## 8. 清理

```bash
cd labs/01-socket-api-basics
make clean                 # 删除 4 个可执行文件
rm -rf .out                # 删除本次运行的日志目录（.out/ 只是运行产物）
pkill -f tcp_echo_server   # 仅在 run.sh 异常中断、残留进程时手动执行
pkill -f udp_echo
```

`run.sh` 内置 `trap cleanup EXIT INT TERM`，正常与异常退出都会回收
`tcp_echo_server`/`udp_echo` 后台进程；端口用 `--port 0` 由内核分配，
不会与系统服务冲突。
