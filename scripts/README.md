# scripts/ —— 环境自检与辅助脚本

本目录存放教程级别的辅助脚本，不属于任何单个 lab。

| 脚本 | 用途 | 需要 root？ |
|---|---|---|
| [`env-check.sh`](./env-check.sh) | 环境自检：回答"我的机器能做什么实验、哪些参数当前是多少" | 否（纯只读探测） |
| [`netns-veth-demo.sh`](./netns-veth-demo.sh) | 无需 root 的真实网卡路径实验：建 netns+veth、改 MTU、复现 IP 分片与重组 | 否（自动进入用户命名空间） |

---

## `env-check.sh`

### 用途

一条命令回答三个问题：

1. **我的机器具备什么能力？** —— 内核/发行版/工具链版本、capabilities、
   `/dev/net/tun`、网络命名空间、`/proc/net` 与 `/proc/sys` 可读性、
   `NETLINK_ROUTE` / `NETLINK_SOCK_DIAG` 是否可用、内核头文件与模块构建环境。
2. **关键网络参数当前是多少？** —— 40+ 个 `sysctl` 的只读快照。
3. **本教程 labs 01~09 哪些我能直接跑？** —— 打印一张带 ✅/⚠️/❌ 的结论表，
   并给出补齐环境所需的安装命令（Debian/Ubuntu 与 RHEL 两套）。

### 用法

```bash
cd scripts
./env-check.sh                 # 人类可读输出
NO_COLOR=1 ./env-check.sh      # 无 ANSI 转义，便于重定向 / 贴进文档
./env-check.sh > env.txt       # 保存一份报告
```

### 设计要点

- **不需要 root**，纯只读探测。退出码**恒为 0**：本脚本的职责是"如实报告"，
  任何一个探测项失败（比如某个文件不可读）都不应该让整个自检失败。
  因此它**故意不使用 `set -e`**（只用了 `set -uo pipefail`）。
- **不依赖 `sysctl(8)`**：读参数一律直接 `cat /proc/sys/...`。
  不是洁癖，而是踩过坑：本教程开发机上 `sysctl` **其实装了**（`procps` 提供
  `/usr/sbin/sysctl`），但它所在目录不在非登录 shell 的 `PATH` 里，直接敲 `sysctl`
  会报「未找到命令」，很容易被误判成「机器没装」。
  直接读 `/proc/sys/...` 没有任何这类歧义：不依赖 `PATH`、不依赖 root、任何环境都能用。
- **capability 解码**：读取 `/proc/self/status` 的 `CapEff` 十六进制位图，
  用 bash 内建位运算判断 `CAP_NET_ADMIN`(12)、`CAP_NET_RAW`(13)、
  `CAP_SYS_ADMIN`(21)、`CAP_PERFMON`(38)、`CAP_BPF`(39) 是否存在。
  能力编号来自 `include/uapi/linux/capability.h`。
- **trcacefs 判定不只看挂载**：还会检查 `tracing_on` 是否**可写** ——
  真正决定 ftrace 能否用的是这个，而不是目录是否存在。
- **中英文混排对齐**：结论表刻意用"每个 lab 一个小块"的排版，
  因为中文字符是双宽字符，`printf '%-30s'` 会按字符数填充而错位。

### 真实输出（本教程开发机，Debian 13 / 内核 6.12.107）

以下是本机实测输出的节选（完整输出见
[`docs/00-环境准备与学习方法.md`](../docs/00-环境准备与学习方法.md) 第 3 节）。

```
===== 2. Capabilities 与特权能力 =====
  CapEff (raw)                       0x0000000000000000  =  0
  ❌ CAP_NET_ADMIN (bit 12) 缺失 —— 无法建 netns/veth、改 sysctl、配 qdisc/nftables
  ❌ CAP_NET_RAW (bit 13) 缺失 —— 无法创建 AF_PACKET 原始套接字（抓包、ping）
  ❌ CAP_SYS_ADMIN (bit 21) 缺失 —— 无法挂载 tracefs、执行大量管理操作
  ❌ CAP_PERFMON (bit 38) 缺失 —— 无法调用 perf_event_open（perf/bpftrace 追踪）
  ❌ CAP_BPF (bit 39) 缺失 —— 无法加载 BPF 程序（bpftrace/bcc）

===== 3. 网络相关内核接口可用性 =====
  ❌ /dev/net/tun 不存在 —— labs/05 的 TUN/TAP 实验无法在本机运行
  ⚠️ unshare --net 直接失败（初始命名空间无 CAP_SYS_ADMIN）—— 但不代表 netns 实验做不了
  ✅ unshare -Urnmpf --mount-proc 可用 —— 可在用户命名空间内建 netns/veth/改 MTU/AF_PACKET
  ✅ NETLINK_ROUTE 非特权查询可用（ip -j link show 正常）
  ✅ /proc/net/* 全部可读（16/16 个关键文件）—— 观测类实验基础完备
  ✅ /proc/sys/net/* 可读（只读）—— 可以读参数快照
  ✅ NETLINK_SOCK_DIAG 非特权可用（labs/09 的 05-socket-diag.py 可以跑）

===== 7. 关键网络参数只读快照（直接读 /proc/sys） =====
  net/core/somaxconn                             = 4096
  net/ipv4/tcp_max_syn_backlog                   = 2048
  net/ipv4/tcp_syncookies                        = 1
  net/ipv4/ip_local_port_range                   = 32768	60999
  net/ipv4/tcp_tw_reuse                          = 2
  net/core/rmem_max                              = 212992
  net/ipv4/tcp_rmem                              = 4096	131072	6291456
  net/ipv4/tcp_congestion_control                = cubic
  net/ipv4/tcp_available_congestion_control      = reno cubic
  net/core/default_qdisc                         = fq_codel
  net/core/netdev_max_backlog                    = 1000
  net/core/netdev_budget                         = 300
  net/core/netdev_budget_usecs                   = 8000
  net/core/dev_weight                            = 64
  net/ipv4/ip_forward                            = 1
  net/ipv4/conf/all/rp_filter                    = 0
  kernel/unprivileged_bpf_disabled               = 2
  kernel/perf_event_paranoid                     = 3

===== 8. 结论：本教程 labs 01~09 在本机的可运行性 =====
  ✅  lab/01  socket API 基础 (C 语言)
  ✅  lab/02  TCP 状态机实验
  ✅  lab/03  NETLINK_ROUTE / rtnetlink
  ⚠️  lab/04  原始套接字与 ARP（在 `unshare -Urn` 内可获得 CAP_NET_RAW，实测真跑通过）
  ❌  lab/05  TUN/TAP 用户态协议栈
  ⚠️  lab/06  内核模块与 netfilter
  ✅  lab/07  HTTP 解析器
  ❌  lab/08  eBPF / XDP
  ✅  lab/09  内核网络观测与调优
```

### 注意事项

- 参数快照是**瞬时值**，不是"内核默认值"。不同发行版、不同内核编译配置下
  这些值可能完全不同。本脚本只报告**实测值**，不做任何"应该是什么"的断言。
- `CapEff=0` 并不等于"什么都不能做"。本机实测表明：`/proc/net/*` 与
  `/proc/sys/net/*` 全部可读，`NETLINK_ROUTE` 与 `NETLINK_SOCK_DIAG` 的
  **非特权查询**都可正常工作 —— `ss`、`ip -j link show` 这类工具本身就依赖它们。
  这就是 `labs/09-observability` 能把观测实验做完整的原因。

---

## `netns-veth-demo.sh`

### 用途

回答一个很实际的问题：**为什么"本机测得通、上线就出问题"？**
因为教程里绝大多数实验跑在环回（`lo`）上，而 `lo` 的 MTU 是 **65536**，
所以**永远不会分片**。本脚本用 veth + 小 MTU + 两个网络命名空间复现真实网卡路径。

`unshare` 会自动重新 exec 进用户命名空间，因此**不需要 root**：

```bash
bash scripts/netns-veth-demo.sh
```

### 它验证了什么

| 实验段 | 观察到的现象 |
|---|---|
| 小包 ICMP | `ping -s 100` 通过（108 字节 ≪ MTU 1280） |
| 大包 + DF | `ping -M do -s 2000` 返回 `sendmsg: 消息过长`（EMSGSIZE） |
| 3000 字节 UDP 走 veth (MTU 1280) | 发送侧 `FragOKs 0→1`、`FragCreates 0→3`；接收侧 `ReasmReqds=3`、`ReasmOKs=1` |
| 同样的 3000 字节走 lo (MTU 65536) | `FragOKs`/`FragCreates` **完全不变** |

### 真实输出（本机实测，退出码 0）

```
━━━ 1. 环境确认 ━━━
  uid=0  CapEff=000001ffffffffff  （在新用户命名空间内）
  当前网络命名空间的接口：
    lo  MTU=65536
  注意：/sys 无法重挂（mount -t sysfs 返回 EPERM），所以 MTU 要用 ip 读

━━━ 4. 核心实验 A：3000 字节 UDP 经过 MTU 1280 的 veth —— 分片与重组 ━━━
  发送前(父ns) InReceives=1  ReasmReqds=0  ReasmOKs=0  FragOKs=0  FragFails=0  FragCreates=0
  发送后(父ns) InReceives=2  ReasmReqds=0  ReasmOKs=0  FragOKs=1  FragFails=0  FragCreates=3
  接收侧(子ns) InReceives=4  ReasmReqds=3  ReasmOKs=1

━━━ 5. 核心实验 B（对照）：同样的 3000 字节走 lo（MTU 65536）—— 不分片 ━━━
  lo 发送前  FragOKs=1  FragCreates=3
  lo 发送后  FragOKs=1  FragCreates=3
  FragOKs / FragCreates 都没变 —— 这就是「环回测不出分片问题」的直接证据。
```

### 两个反直觉的实测结论（都被写进了脚本输出）

1. **DF 被拒时 `FragFails` 并不增加**。应用确实收到 EMSGSIZE，但 `Ip: FragFails` 保持为 0
   —— 「计数器名字 ≠ 计数器语义」，这类事情只能靠实测确认。
2. **`/sys` 在用户命名空间里是指向宿主机的**。`mount -t sysfs` 返回 EPERM，
   因此 `ls /sys/class/net` 看到的仍是宿主机接口；接口 MTU 与统计必须用
   `ip link show` 和 `/proc/net/dev`（`/proc/net/*` 是 per-netns 的）。

### 已知限制

- `ip netns add` 需要写 `/run/netns`，本机只读 → 脚本改用"另起一个 `unshare -n` 进程 +
  `nsenter -t <pid> -n`"的方式把 veth 一端放进另一个 netns。
- 无法加载内核模块、无法加载 eBPF、没有 `/dev/net/tun`（userns 内 `mknod` 被拒），
  这三件事仍然必须要有 root。

---

## 相关文档

- [`docs/00-环境准备与学习方法.md`](../docs/00-环境准备与学习方法.md) —— 环境搭建三档、学习路线、读源码方法
- [`docs/04-网络层-IP路由与邻居子系统.md`](../docs/04-网络层-IP路由与邻居子系统.md) —— 分片/重组与 PMTU 的内核实现
- [`labs/09-observability/README.md`](../labs/09-observability/README.md) —— 观测实验
