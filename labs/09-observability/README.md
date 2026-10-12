# labs/09-observability —— 内核网络观测实验

本目录是 [`docs/09-内核网络观测与性能调优.md`](../../docs/09-内核网络观测与性能调优.md)
的配套实验。全部脚本遵循同一个契约：

> **不需要 root。跑不了的实验打印 `SKIP` 并以退出码 0 结束，绝不中断。**

这样设计的原因是：本教程的开发环境是一台"有内核能力、但没有任何特权"的机器
（详见 [`scripts/env-check.sh`](../../scripts/env-check.sh) 的输出与
`docs/00-环境准备与学习方法.md`）。我们希望这些实验在**受限环境里也能真跑出结果**，
而不是让读者卡在权限上。

---

## 1. 总览

| 脚本 | 主题 | 需要 root？ | 本机实测 |
|---|---|---|---|
| `01-socket-overview.sh` | `ss` 的宏观视角：总量、监听、内部指标、Unix socket | 否 | ✅ 真跑通过 |
| `02-proc-net-snapshot.sh` | `/proc/net/*` 逐文件快照与解读 | 否 | ✅ 真跑通过 |
| `03-softnet-and-drops.sh` | `softnet_stat` 逐列解析 + 网卡层丢包 | 否 | ✅ 真跑通过 |
| `04-snmp-counters.sh` | `/proc/net/snmp`、`/proc/net/netstat` 计数器解析 | 否 | ✅ 真跑通过 |
| `05-socket-diag.py` | **手写 NETLINK_SOCK_DIAG**，绕开 `ss` 读 socket 明细 | 否 | ✅ 真跑通过 |
| `06-tracepoint-templates.md` | perf / ftrace / bpftrace 模板 | **是**（且需 tracefs 可写 + 装工具） | ⚠️ 模板，本机不可运行 |
| `07-watch-tcp-state.sh` | 轮询 `/proc/net/tcp` 生成状态时间序列 | 否 | ✅ 真跑通过 |

**本机实测环境**：Debian GNU/Linux 13 (trixie)，内核 `6.12.107+deb13-amd64`，
Python `3.13.5`，`uid=1000`，`CapEff=0x0000000000000000`。

### 快速开始

```bash
cd labs/09-observability

make all            # 语法检查：bash -n 每个 .sh + python3 -m py_compile 每个 .py
make run            # 依次跑本机可跑的脚本（01 02 03 04 05 07）
make verify-states  # 受控实验：验证 sock_diag 的 idiag_states 位约定
make clean          # 清理 __pycache__
make help           # 帮助
```

`make all` 的实际输出：

```
==============================================================
 语法检查 (make all)
==============================================================
  ✅ bash -n 通过: 01-socket-overview.sh
  ✅ bash -n 通过: 02-proc-net-snapshot.sh
  ✅ bash -n 通过: 03-softnet-and-drops.sh
  ✅ bash -n 通过: 04-snmp-counters.sh
  ✅ bash -n 通过: 07-watch-tcp-state.sh
  ✅ py_compile 通过: 05-socket-diag.py
--------------------------------------------------------------
 全部语法检查通过。
==============================================================
```

### 统一约定

- **`NO_COLOR`**：所有 shell 脚本都尊重 [`NO_COLOR`](https://no-color.org/) 约定。
  设置 `NO_COLOR=1` 可得到不含 ANSI 转义的纯文本，便于重定向到文件或贴进文档。
  （本 README 中的输出都是在 `NO_COLOR=1` 下采集的。）
- **退出码**：`0` = 成功或"因环境不支持而跳过"；非 `0` 只表示脚本自身有 bug。
  因此 `make run` 在受限环境下也必然返回 0。
- **`SKIP` 语义**：脚本检测到前置条件不满足时，打印以 `SKIP:` 开头的一行说明原因，
  然后 `exit 0`。

---

## 2. `01-socket-overview.sh` —— `ss` 的宏观视角

### 目的

观测漏斗的第一层永远是"先看规模和分布"。本脚本把 `ss` 的几种全局用法串起来，
并对每段输出做中文解读标注。

### 真实输出（节选）

```
===================================================================
 socket 总览观测 —— 主机: host  内核: 6.12.107+deb13-amd64
===================================================================

===== ① 汇总统计：ss -s =====
Total: 1672
TCP:   514 (estab 483, closed 13, orphaned 2, timewait 12)

Transport Total     IP        IPv6
RAW	  1         0         1
UDP	  7         3         4
TCP	  501       495       6
INET	  509       498       11
FRAG	  0         0         0

===== ② 监听中的端口：ss -lntup =====
Netid State  Recv-Q Send-Q                    Local Address:Port  Peer Address:PortProcess
udp   UNCONN 0      0                               0.0.0.0:46510      0.0.0.0:*
udp   UNCONN 0      0                               0.0.0.0:5353       0.0.0.0:*
udp   UNCONN 0      0      [fe80::8c:5974:c0d1:e608]%wlan0:546           [::]:*
tcp   LISTEN 0      128                             0.0.0.0:22         0.0.0.0:*
tcp   LISTEN 0      4096                          127.0.0.1:631        0.0.0.0:*
tcp   LISTEN 0      511                           127.0.0.1:3080       0.0.0.0:*
tcp   LISTEN 0      1                             127.0.0.1:41329      0.0.0.0:*
```

### 如何解读

- `ss -s` 的 `Total` 是所有协议族的 socket 总数。**先看这里**：如果 TIME-WAIT
  上万，排查方向直接锁定为端口耗尽/短连接风暴。
- ② 中 `Send-Q` 列对 **LISTEN** socket 而言不是"发送缓冲"，而是
  **已完成三次握手、正在等待 `accept()` 的队列长度**（内核里的全连接队列）。
  上面 `127.0.0.1:41329` 的 `Send-Q=1` 就是一个刚握完手还没被 accept 的连接。
  这一列持续 >0 且波动 = 应用 `accept()` 太慢。
- ③ `ss -tni` 的 `i` 会输出内核 TCP 栈内部变量（`cubic`、`rtt`、`cwnd`、`retr`、
  `rto`、`unacked` 等）。这些来自 `struct tcp_info`，`/proc/net/tcp` 里看不到。

### 对应的内核数据结构与源码入口

| 观测项 | 内核数据结构 / 函数 | 源码位置（以 6.12 为准） |
|---|---|---|
| `ss` 的总量统计 | 遍历各协议 hash 表 | `net/core/sock_diag.c` |
| LISTEN 的 `Send-Q` | `struct inet_connection_sock.icsk_accept_queue` | `include/net/inet_connection_sock.h` |
| `ss -tni` 的内部字段 | `struct tcp_info` | `include/uapi/linux/tcp.h`、`net/ipv4/tcp_diag.c` |
| TCP 状态码 | `enum { TCP_ESTABLISHED = 1, ... }` | `include/net/tcp_states.h` |

> 核对地址：`https://elixir.bootlin.com/linux/v6.12/source/<上面的路径>`

---

## 3. `02-proc-net-snapshot.sh` —— `/proc/net` 逐文件快照

### 目的

`/proc/net/*` 是内核网络栈最"素"的对外接口，全部只读、非特权可读。在精简容器
镜像里（没有 `ss`、没有 `ethtool`）它往往是**唯一**还能用的观测手段。

### 真实输出（节选）

```
===== ① /proc/net/dev（每张网卡的收发统计） =====
Inter-|   Receive                                                |  Transmit
 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
    lo: 1598654604 17402026    0    0    0     0          0         0 1598654604 17402026    0    0    0     0       0          0
eth0:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0
wlan0: 1797999012 2812581    0    1    0     0          0         0 1161163197 1877285    0    7    0     0       0          0
docker0:       0       0    0    0    0     0          0         0        0       0    0  106     0     0       0          0

===== ⑤ /proc/net/tcp（前 8 行） =====
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:A171 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 321919 1 ...
   1: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000 65534        0 7464 1 ...
   6: 0100007F:EAEC 0100007F:0C08 01 00000000:00000000 02:00000025 00000000  1000        0 285957 2 ...

===== ⑦ /proc/net/softnet_stat（每 CPU 一行，共 20 行） =====
000a5e9f 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
...
```

### 如何解读（三个最容易踩的坑）

1. **`/proc/net/tcp` 全是十六进制，且 IP 是小端。**
   `0100007F:A171` = 按小端读 IP `0100007F` → `127.0.0.1`，端口 `0xA171` = 41329。
   上面第 6 行的 `0100007F:0C08`，`0x0C08 = 3080` —— 正好对应 `ss` 输出里
   监听的 `127.0.0.1:3080`，可以互相验证。
   `st` 列同样是十六进制：`0A=LISTEN`、`01=ESTABLISHED`、`06=TIME_WAIT`。
   `tx_queue:rx_queue` 就是 `ss` 的 `Send-Q:Recv-Q`。
2. **`/proc/net/snmp` 与 `/proc/net/netstat` 是"名字行 + 值行"成对出现。**
   直接 `cat` 会看到奇数行全是字段名、偶数行全是数字，不是表格。
3. **`/proc/net/sockstat` 的 `mem` 单位是"页"而不是字节**
   （本机页大小 4096）。这是最常见的误读。

### 对应的内核数据结构与源码入口

| 文件 | 内核实现 | 源码位置 |
|---|---|---|
| `/proc/net/dev` | `dev_seq_show()` | `net/core/net-procfs.c` |
| `/proc/net/snmp` | `snmp4_ipstats_list` 等字段表 | `net/ipv4/proc.c` |
| `/proc/net/netstat` | `TcpExt` 等扩展表 | `net/ipv4/proc.c` |
| `/proc/net/tcp` | `tcp4_seq_show()` | `net/ipv4/tcp_ipv4.c` |
| `/proc/net/sockstat` | `sockstat_seq_show()` | `net/ipv4/proc.c` |
| `/proc/net/softnet_stat` | `softnet_seq_show()` | `net/core/net-procfs.c` |

---

## 4. `03-softnet-and-drops.sh` —— `softnet_stat` 逐列解析

### 目的

回答一个具体问题：**"包到底丢在哪一层？"**
网卡 ring buffer、softnet backlog、socket 队列是三个不同的丢弃点，
本脚本把前两层的计数器拉出来对比。

### 真实输出（节选）

```
===== ① 文件形态自检 =====
  每行字段数（列数）: 15
  行数（应等于可能的 CPU 数）: 20
  真机在线 CPU 数    : 20

===== ③ softnet_stat 逐 CPU 渲染（十六进制 -> 十进制） =====
  CPU     processed    dropped  time_squeeze    recv_rps*  flow_lim*          col12+*
  ---- ------------ ---------- ------------- ------------ ---------- ----------------
  0          686595          0             0            0          0                0
  1         3208360          0             0            0          0                0
  4         4115208          0             0            0          0                0
  ...
  19          15006          0             0            0          0                0
  合计     20109385          0             0

===== ④ 判读 =====
  ✅ 没有任何 CPU 出现 softnet backlog 丢包（第 2 列全为 0）
  ✅ 没有 CPU 报告 time_squeeze（收包压力未触及预算上限）
  ℹ️  所有 CPU 累计处理报文 20109385 个

===== ⑤ 网卡层统计（/sys/class/net/*/statistics/） =====
  接口              rx_dropped tx_dropped  rx_errors  rx_missed_err      rx_bytes
  ---------------- ---------- ---------- ---------- -------------- ------------
  lo                        0          0          0            n/a   1598654604
  wlan0                    1          7          0            n/a   1797999012
  docker0                   0        106          0            n/a            0
```

### 如何解读

- **每一列都是 8 位十六进制**（内核用 `%08x` 输出）。`000a5e9f` 是 0x000a5e9f
  = 679583，**不是**十进制的一千万。这是本文件最大的坑。
- **第 1 列 `processed`**：该 CPU 处理的报文数（本机合计 2010 万）。
- **第 2 列 `dropped`**：backlog 队列满而丢弃的报文数。**这是丢包排查第一现场。**
- **第 3 列 `time_squeeze`**：一次 NAPI poll 用光预算 `netdev_budget` 仍没处理完、
  被迫让出 CPU 的次数。持续增长说明单 CPU 收包压力过大。
- `rx_missed_errors` 显示 `n/a` 是**正常的** —— 本机网卡驱动没有上报这个计数器。
  它是"ring buffer 溢出"的经典信号，但只有部分驱动支持。

### ⚠️ 关于列数：本机是 15 列，不是常见的 7 列

本机实测 `/proc/net/softnet_stat` **每行 15 列**，而网上大量资料只讲 7 列。
本脚本因此**不写死列数**：它先 `awk '{print NF}'` 读出真实列数，只有当列数为 15 时
才使用标注好的列名，否则打印警告提醒读者去看自己内核版本的源码。

对第 4 列之后的语义，本脚本用 `[已确认]` / `[待核对]` 明确区分置信度，
并把"如何自己验证"写进了下面的第 8 节 —— **不臆测内核行为**。

### 三层丢包位置对照

| 层 | 观测点 | 计数器 |
|---|---|---|
| ① 网卡 ring buffer | `/sys/class/net/<if>/statistics/` | `rx_missed_errors`、`rx_dropped` |
| ② 内核 softnet backlog | `/proc/net/softnet_stat` | 第 2 列 `dropped` |
| ③ 内核 → 应用 | `ss` 的 `Recv-Q`、`/proc/net/netstat` | `TcpExt TCPRcvQDrop` |

排查顺序应当**从下往上**（网卡 → backlog → socket → 应用）。

---

## 5. `04-snmp-counters.sh` —— SNMP / TcpExt 计数器

### 目的

把"名字行 + 值行"这种别扭的格式解析成可读表格，并计算派生指标（重传率）。

### 真实输出（节选）

```
===== ① TCP 基础计数器（/proc/net/snmp 的 Tcp: 组） =====
  ActiveOpens                    7393
  PassiveOpens                   1531
  AttemptFails                   122
  EstabResets                    829
  CurrEstab                      483
  InSegs                         19770750
  OutSegs                        18964484
  RetransSegs                    2813
  InErrs                         6
  OutRsts                        1582
  InCsumErrors                   0

===== ③ TcpExt 扩展计数器（/proc/net/netstat 的 TcpExt: 组） =====
  ── 连接建立/队列相关 ──
  ListenOverflows                58
  ListenDrops                    58
  TCPReqQFullDrop                0
  TCPReqQFullDoCookies           0

  ── 重传/超时相关 ──
  TCPTimeouts                    2112
  TCPLostRetransmit              1738
  TCPTimeWaitOverflow            0

  ── 缓冲区/内存压力相关 ──
  TCPBacklogDrop                 0
  TCPRcvQDrop                    0
  TCPAbortOnMemory               0

  ── 乱序/快速路径相关 ──
  TCPOFOQueue                    83984
  TCPHPHits                      838810
  TCPPureAcks                    8497970
  DelayedACKs                    5206
  TW                             2598

===== ④ 派生指标：重传率 =====
  发出报文段总数 OutSegs      = 19000172
  重传报文段数   RetransSegs  = 2912
  重传率 RetransSegs/OutSegs  = 0.0153%
  入向错误率 InErrs/InSegs    = 0.000030%

  ✅ 重传率 < 1%，属于正常范围（公网通常在 0.1%~1% 之间波动）
```

### 如何解读

- 这些全是**累计计数器**，单次读数没有意义，必须间隔采样做差看增量。
- 本机 `ActiveOpens(7393) >> PassiveOpens(1531)`，说明这台机器主要作为
  **客户端**发起连接。
- 本机 `ListenOverflows = ListenDrops = 58` —— 这是一个**真实存在的**
  accept 队列溢出记录，说明历史上确实发生过"三次握手完成但 accept 队列满"。
  排查手法见 `docs/09` 第 6 节 case 7。
- 所有计数器**名字都经过本机实测核对**（`awk 'NR==1' /proc/net/netstat` 的真实表头），
  不是从博客抄的。本机 TcpExt 共有 132 个计数器。

### 对应的内核数据结构与源码入口

| 计数器组 | 内核枚举 / 字段表 | 源码位置 |
|---|---|---|
| `Tcp:` | `TCP_MIB_*` | `include/net/tcp.h`、`net/ipv4/proc.c` |
| `TcpExt:` | `LINUX_MIB_TCPEXT_*` | `include/linux/snmp.h`、`net/ipv4/proc.c` |
| `Udp:` | `UDP_MIB_*` | `include/linux/snmp.h` |

各计数器的 `++` 位置分散在 `net/ipv4/tcp_input.c`、`tcp_output.c`、`tcp_timer.c` 等，
注释里通常直接写明是哪个 MIB。

---

## 6. `05-socket-diag.py` —— 手写 NETLINK_SOCK_DIAG

### 目的

`ss` 不是读 `/proc/net/tcp`，而是通过 **`NETLINK_SOCK_DIAG`** 这个专门的 netlink
协议族向内核索取 socket 明细。本脚本**手写 netlink 报文**，绕开 `ss`，
自己完成请求构造与响应解析。

这样做能真正理解"内核如何把内部数据结构导出成稳定 ABI"，也是容器里没装
`iproute2` 时的诊断手段。

### 一个本机特性：Python 标准库没有 `NETLINK_SOCK_DIAG`

Python 3.13 的 `socket` 模块**只有** `NETLINK_ROUTE`（值 0），
**没有** `NETLINK_SOCK_DIAG`。所以脚本里手动定义：

```python
NETLINK_SOCK_DIAG = 4   # 标准库没有这个常量，手动定义
```

### 真实输出（节选）

```
====================================================================================================
NETLINK_SOCK_DIAG 直连实验 —— 绕开 ss，直接向内核索取 socket 明细
====================================================================================================
内核      : 6.12.107+deb13-amd64
Python    : 3.13.5
CapEff    : 0x0000000000000000  (root 需要 CAP_NET_ADMIN 才能看到**别的进程**的 socket)
权限结论  : 本脚本只 dump 自己所在的 netns，非特权即可运行 ✅

① TCP/IPv4 — 所有状态（idiag_states = 0xFFFFFFFF）
STATE         LOCAL                    REMOTE                   TIMER     RQ      WQ      INODE
ESTABLISHED   127.0.0.1:3080           127.0.0.1:57952          -/0       0       0       287775

② TCP/IPv4 — 只要 LISTEN（idiag_states = 1<<10，等价 ss -lnt）
STATE         LOCAL                    REMOTE                   TIMER     RQ      WQ      INODE
LISTEN        0.0.0.0:22               *:*                      -/0       0       128     7464
LISTEN        127.0.0.1:631            *:*                      -/0       0       4096    241208
LISTEN        127.0.0.1:3080           *:*                      -/0       0       511     186924
LISTEN        127.0.0.1:41329          *:*                      -/0       0       1       321919

⑤ TCP/IPv6 — 所有状态
ESTABLISHED   2408:8220:b20:d5c1::1b9:48096 2603:1061:14:186::1:443  keepalive/0 0       0       303961
LISTEN        :::22                    :::0                     -/0       0       128     7466

── TCP 明细（来自 INET_DIAG_INFO / struct tcp_info，ss -tni 的同类数据）──
STATE         LOCAL                    CONG         RTT(us)   CWND    LOST    RETRANS
ESTABLISHED   2408:8220:b20:d5c1::1b9:48096 ?            112569    10      0       0

汇总：本 netns 内 TCP/IPv4 socket 状态分布
  ESTABLISHED    483
  TIME-WAIT        8
  LISTEN           5
  CLOSE-WAIT       5
  TOTAL          501
```

### ⚠️ 本实验最重要的一个发现：`idiag_states` 的位序号 = TCP 状态编号

这是本项目里最容易出错、且**错了不会报错**的地方。

`inet_diag_req_v2.idiag_states` 是一个位掩码。直觉上会以为"第 N 个状态对应
第 N-1 位"（即 `1 << (state - 1)`）。**这是错的。** 实际约定是：

```
位序号 == TCP 状态编号本身
  TCP_ESTABLISHED == 1  ->  bit 1  (mask = 2)
  TCP_TIME_WAIT   == 6  ->  bit 6  (mask = 64)
  TCP_LISTEN      == 10 ->  bit 10 (mask = 1024)
```

如果按 `1 << (state - 1)` 填，内核**不会返回错误**，而是安静地返回 0 条记录 ——
这是最难查的一类 bug。作者在实现本脚本时先踩了这个坑（最初写的就是
`1 << (state-1)`，结果所有过滤查询都返回空），才通过实验定位到真相。

**本机可复现的验证**：

```bash
make verify-states
```

真实输出：

```
verify-states：验证 idiag_states 的位序号约定
已建立受控 socket：LISTEN 端口 43803 + 一对 ESTABLISHED 连接

bit    mask     内核返回的 idiag_state            条数       是否命中我们的测试 socket
----------------------------------------------------------------------------------------------------
1      2        1                            481      ✅ 一致（bit == state）  ← 命中测试 socket
2      4        2                            3        ✅ 一致（bit == state）
3      8        -                            0        （该状态本机无 socket）
4      16       4                            1        ✅ 一致（bit == state）
5      32       -                            0        （该状态本机无 socket）
6      64       6                            11       ✅ 一致（bit == state）
7      128      -                            0        （该状态本机无 socket）
8      256      8                            5        ✅ 一致（bit == state）
9      512      -                            0        （该状态本机无 socket）
10     1024     10                           7        ✅ 一致（bit == state）  ← 命中测试 socket
11     2048     11                           1        ✅ 一致（bit == state）
12     4096     -                            0        （该状态本机无 socket）

补充验证：bit 0 (mask=1) 返回 0 条。
  因为 TCP 状态从 1 开始编号，bit 0 永远对应不到任何状态 —— 这反过来证明位序号就是状态编号本身，而不是『状态编号-1』。
```

**源码依据**（已在本机内核头文件中核对，
`/usr/src/linux-headers-6.12.107+deb13-common/include/net/tcp_states.h`）：

```c
enum {
	TCP_ESTABLISHED = 1,
	TCP_SYN_SENT,
	...
	TCP_LISTEN,
	...
};

enum {
	TCPF_ESTABLISHED = (1 << TCP_ESTABLISHED),
	...
	TCPF_LISTEN	 = (1 << TCP_LISTEN),
	...
};
```

因为 `TCP_LISTEN == 10`，所以 `TCPF_LISTEN == 1 << 10 == 1024`。
—— 与上面的实验结果完全一致。

> 也可以在线核对：
> `https://elixir.bootlin.com/linux/v6.12/source/include/net/tcp_states.h`

### 协议结构速查（均已在 6.12 头文件中核对）

```c
struct inet_diag_req_v2 {          /* 56 字节 */
    __u8  sdiag_family;
    __u8  sdiag_protocol;
    __u8  idiag_ext;
    __u8  pad;
    __u32 idiag_states;
    struct inet_diag_sockid id;    /* 48 字节 */
};

struct inet_diag_sockid {          /* 48 字节 */
    __be16 idiag_sport;
    __be16 idiag_dport;
    __be32 idiag_src[4];           /* 16 字节，IPv4 只用前 4 字节 */
    __be32 idiag_dst[4];           /* 16 字节 */
    __u32  idiag_if;
    __u32  idiag_cookie[2];
};

struct inet_diag_msg {             /* 72 字节 */
    __u8   idiag_family;
    __u8   idiag_state;
    __u8   idiag_timer;
    __u8   idiag_retrans;
    struct inet_diag_sockid id;    /* 48 字节 */
    __u32  idiag_expires;
    __u32  idiag_rqueue;
    __u32  idiag_wqueue;
    __u32  idiag_uid;
    __u32  idiag_inode;
};
```

两个实现上的坑（作者都实际踩过）：

1. `idiag_src` / `idiag_dst` 在结构体里**恒为 16 字节**
   （`__be32[4]`，为了兼容 IPv6）。用 Python `struct` 时如果写成 `4s`
   会把整个报文长度算错（56 → 32 字节），并且后续所有字段偏移全部错位。
   **必须写 `16s`。**
2. `struct inet_diag_msg` 里 5 个 `__u32` 从**偏移 52** 开始（不是 56）：
   `4 (四个 u8) + 48 (sockid) = 52`。

### 对应的内核源码入口

| 内容 | 源码位置（以 6.12 为准） |
|---|---|
| 协议族注册与分发 | `net/core/sock_diag.c` |
| `inet_diag_msg` 填充 | `net/ipv4/inet_diag.c`（`inet_sk_diag_fill` / `inet_twsk_diag_fill` / `inet_diag_dump_icsk`） |
| TCP handler | `net/ipv4/tcp_diag.c` |
| UDP handler | `net/ipv4/udp_diag.c` |
| uapi 结构 | `include/uapi/linux/inet_diag.h`、`include/uapi/linux/sock_diag.h` |
| `struct tcp_info` | `include/uapi/linux/tcp.h` |

---

## 7. `06-tracepoint-templates.md` —— perf / ftrace / bpftrace 模板

### 为什么本机跑不了

| 检查项 | 本机实测 | 影响 |
|---|---|---|
| `CapEff` | `0x0` | ❌ 无 `CAP_BPF`/`CAP_PERFMON`/`CAP_SYS_ADMIN` |
| `/sys/kernel/tracing` | 已挂载但 **`ro`** 且无读权限 | ❌ 无法 enable 事件 |
| `/sys/kernel/debug` | 同样 **`ro`** | ❌ 无法用 debugfs tracing |
| `unprivileged_bpf_disabled` | **`2`** | ❌ 值为 2 = 已禁用且**运行时无法重新启用** |
| `bpftrace`/`bcc`/`perf`/`trace-cmd` | 全部未安装 | ❌ 无工具 |
| `CONFIG_FTRACE`/`CONFIG_KPROBE_EVENTS`/`CONFIG_BPF_SYSCALL` | 均为 `y` | ✅ 内核**支持**，只是没权限/没工具 |

### 做了什么来补偿

1. 所有模板的**字段名都经过核对** —— 对照本机
   `/usr/src/linux-headers-6.12.107+deb13-common/include/trace/events/*.h`
   （**与运行内核完全同版本**的头文件），而不是抄博客。
2. 文件开头明确写出不可运行的原因、前置条件清单（`mount -o remount,rw`、
   装包命令等）。
3. 每个模板都注明"用途"和"输出示例格式"。

详见 [`06-tracepoint-templates.md`](./06-tracepoint-templates.md)。

---

## 8. `07-watch-tcp-state.sh` —— TCP 状态时间序列

### 目的

单次 `ss` 只看到一个瞬间。很多问题（连接泄漏、accept 慢）只有在**时间维度**上
才看得出来。本脚本用轮询 + 差分画出一个小小的状态时间序列表。

### 真实输出

```
TCP 状态时间序列 —— 内核: 6.12.107+deb13-amd64
 采样 4 次，间隔 1s，数据源: /proc/net/tcp /proc/net/tcp6

  [1/4] 22:58:15 采样完成，共 504 条 TCP socket 记录
  [2/4] 22:58:16 采样完成，共 504 条 TCP socket 记录
  [3/4] 22:58:17 采样完成，共 504 条 TCP socket 记录
  [4/4] 22:58:18 采样完成，共 504 条 TCP socket 记录

===== ① 时间序列表（每行一个状态，每列一个采样点） =====
  STATE \ 采样点       #1       #2       #3       #4     变化
  -------------- -------- -------- -------- -------- ----------
  ESTABLISHED         478      478      478      478          0
  SYN_SENT              8        8        8        8          0
  FIN_WAIT1             1        1        1        1          0
  TIME_WAIT            10        8        8        8         -2
  CLOSE_WAIT            5        5        5        5          0
  LISTEN                8        8        8        8          0
  CLOSING               1        1        1        1          0
  TOTAL               511      509      509      509         -2

===== ② 自动判读 =====
  起始总 socket 记录数: 511
  结束总 socket 记录数: 509
  ✅ CLOSE_WAIT 没有增长（5 -> 5），未发现明显的连接未关闭泄漏
  ℹ️  总记录数下降（-2）。多为短连接自然回收，属正常。
```

### 如何解读

- **`CLOSE_WAIT` 单调增长几乎总是应用层 bug**：对端已关闭连接，本端应用没有
  调用 `close()`，socket 就一直挂着。这是本脚本最想帮你发现的问题。
- `TIME_WAIT` 增长本身是正常现象（主动关闭方必然留下），只有数量达到数万
  并伴随 `Cannot assign requested address` 才是问题。
- **固有局限（很重要）**：`/proc/net/tcp` 是**轮询快照**而不是事件流。
  两次采样之间"建立又立刻关闭"的短连接会被完全漏掉。所以本表适合发现
  "堆积/泄漏"，**不适合**发现"高频短连接"。要做事件级观测必须用
  `sock:inet_sock_set_state` 这类 tracepoint（见 `06-tracepoint-templates.md`）。

---

## 9. 附：如何自己验证 `softnet_stat` 的列数（本机真实操作记录）

因为本机没有内核源码树、没有网络、也没有 root，作者用了一个"土办法"从
**正在运行的内核镜像本身**里取出权威答案。这个方法在任何 Linux 上都能用，
记录在此供读者复现：

```bash
# 1) 确认内核用什么压缩算法
grep CONFIG_KERNEL_ /boot/config-$(uname -r)
#   本机输出: CONFIG_KERNEL_ZSTD=y

# 2) 定位压缩数据在 vmlinuz 中的偏移（找 zstd 魔数 28 b5 2f fd）
python3 -c "
data=open('/boot/vmlinuz-$(uname -r)','rb').read()
print(data.find(bytes.fromhex('28b52ffd')))"
#   本机输出: 21196

# 3) 从该偏移解压出真正的 vmlinux（zstd 会因尾随数据报错，可忽略）
tail -c +21197 /boot/vmlinuz-$(uname -r) | zstd -dc > /tmp/vmlinux.bin 2>/dev/null

# 4) 直接搜出 softnet_seq_show() 的格式串
strings -a /tmp/vmlinux.bin | grep '%08x' | sort -u
```

本机真实结果（这就是"15 列"这个结论的来源）：

```
%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x
```

数一下：**正好 15 个 `%08x`**，与 `/proc/net/softnet_stat` 实际的 15 列完全对应，
并且确认了每列都是**十六进制、宽度 8、零填充**。

> **这个方法的边界**：它证明了"有 15 列、且都是 `%08x`"，
> 但**不能**证明每一列对应哪个结构体字段 —— 那需要读
> `net/core/net-procfs.c` 里 `softnet_seq_show()` 传给 `seq_printf` 的**实参顺序**。
> 本机拿不到该文件（无源码树、无网络），所以脚本里对第 4 列之后统一标注为
> `[待核对]`，并给出核对地址，而不是编造。
>
> 在线核对：`https://elixir.bootlin.com/linux/v6.12/source/net/core/net-procfs.c`

---

## 10. 权威资料

- 内核源码（本教程统一以 6.12 为准）：`https://elixir.bootlin.com/linux/v6.12/source/`
- 本机**同版本**头文件（uapi 与 tracepoint 定义都在这里，最可靠）：
  `/usr/src/linux-headers-6.12.107+deb13-common/include/`
- 官方文档：`Documentation/networking/`、`Documentation/admin-guide/sysctl/net.rst`、
  `Documentation/trace/`、`Documentation/bpf/`
- tracepoint 字段的**最终权威**（任何有 root 的机器上）：
  `/sys/kernel/tracing/events/<group>/<event>/format`
