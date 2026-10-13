# learn-net · Linux 网络子系统系统学习教程

> 一套「**从内核数据结构到 HTTP 解析**」的中文教程 + 可直接运行的实验代码。
> 目标不是罗列知识点，而是让你**看到整个 Linux 网络协议栈的全貌**：
> 一个包从网卡到 `recv()` 经过哪些函数、每一层有哪些可观测点、出问题时该去哪一层看。

---

## 这套教程解决什么问题

多数「Linux 网络」资料要么停在 `socket()` API 层面，要么一上来就钻进单个函数的实现细节，
中间**缺一张能把用户态与内核态连起来的地图**。本教程补的就是这一层：

- **有主干**：六条核心调用链（发送 / 接收 / 握手 / 挥手 / 转发 / 丢包），把 100+ 个内核函数串成一张图。
- **有锚点**：每个机制都给出真实源码路径（`net/ipv4/tcp_input.c` 等），并说明该看哪个函数。
- **有验证**：每个结论都配一个能跑的实验或一条能观测的命令——不靠「我觉得」。
- **有边界**：明确区分「本机可直接跑」与「需要 root / 需要内核源码」的部分，不假装全能。

---

## 30 秒快速开始

```bash
cd code/learn-net

make            # 编译全部实验（需要 root 的实验会打印 SKIP，不影响构建）
make check      # 质量门禁：9 个 lab 全部从零重建，并断言「零 warning / 零 error」
make test       # 跑可离线运行的单元测试（labs/07 的 2.7 万条断言）
make env        # 环境自检：打印本机能跑哪些实验的能力矩阵
make list       # 列出全部文档与实验

# 最推荐的第一站：内核观测工具箱（无需 root）
cd labs/09-observability && make run

# 第二站：亲手写一个 HTTP/1.1 解析器（无需 root，2.7 万断言）
cd ../07-http-parser && ./run.sh

# 想直接看内核子模块实操（编译零警告；加载需 root，但产物信息可验证）
cd labs/06-kernel-module-netfilter && make && /sbin/modinfo netlab.ko
# 注意 /sbin 可能不在你的 PATH 里（本机就是），所以这里写了绝对路径
```

> `make check` 是这套教程的质量门禁：它会把 9 个 lab 全部**从零重建**，
> 并断言「零 warning / 零 error」——因为教程承诺所有代码在 `-Wall -Wextra` 下零警告。

---

## 全景图（一页纸版本）

```
用户态   socket() connect() send() recv() epoll_wait() sendfile()
         │
 ① 系统调用      net/socket.c            struct socket
 ② 协议族 AF_INET net/ipv4/af_inet.c     struct sock
 ③ 传输层 TCP/UDP net/ipv4/tcp*.c udp.c tcp_sock：窗口/拥塞/状态机
 ④ 网络层 IP+路由 net/ipv4/ip_*.c route.c   FIB trie / rtable / PMTU
 ⑤ netfilter     net/netfilter/*        5 个 hook：过滤/NAT/conntrack
 ⑥ 邻居 ARP      net/core/neighbour.c   IPv4↔MAC / NUD 状态机
 ⑦ qdisc 流控    net/sched/*            排队/整形
 ⑧ 驱动与网卡    drivers/net/**         ndo_start_xmit / DMA ring
 ⑨ 软中断 NAPI   net/core/dev.c         napi_schedule → net_rx_action
 ⑩ 协议分发      ptype_base → ip_rcv    ┐
 ⑪ 回到传输层    tcp_v4_rcv → 唤醒       ┘ 接收路径
```

完整版（含每一层的函数调用链、刹车点、并发上下文说明）见
**[docs/01-Linux网络子系统全景与数据流.md](docs/01-Linux网络子系统全景与数据流.md)** —— 建议从这里开始读。

---

## 文档导览（docs/）

| 章节 | 一句话 | 读完能回答 |
|---|---|---|
| [00 环境准备与学习方法](docs/00-环境准备与学习方法.md) | 路线图、工具安装、读内核源码的方法 | 我该按什么顺序学？环境怎么搭？ |
| [01 网络子系统全景与数据流](docs/01-Linux网络子系统全景与数据流.md) | **全书之锚**：分层图 + 收发全链路 + sk_buff | 一个包从网卡到 `recv()` 经过哪些函数？ |
| [02 套接字层与用户态接口](docs/02-套接字层与用户态接口.md) | `socket`/`sock` 双层结构、缓冲区记账、epoll 唤醒 | `socket()` 到 `connect()` 内核做了什么？ |
| [03 传输层 TCP 与 UDP 实现](docs/03-传输层-TCP与UDP实现.md) | 状态机、滑动窗口、拥塞控制、TIME_WAIT | cwnd/rtt/window 在内核哪里维护？ |
| [04 网络层 IP、路由与邻居子系统](docs/04-网络层-IP路由与邻居子系统.md) | ip_rcv/ip_output、FIB trie、ARP/NUD | 路由查找怎么做？ARP 什么时候发？ |
| [05 链路层、网卡驱动、NAPI 与中断](docs/05-链路层-网卡驱动-NAPI与中断.md) | NAPI、软中断、ring buffer、GRO/GSO、qdisc | 为什么会丢包/软中断饿死？ |
| [06 netfilter、conntrack 与 NAT](docs/06-netfilter-conntrack与NAT.md) | 5 个 hook、连接跟踪、SNAT/DNAT、k8s 网络 | iptables 规则在哪条路径生效？ |
| [07 eBPF、XDP 与可编程数据面](docs/07-eBPF-XDP与可编程数据面.md) | 验证器/JIT/map、XDP 三个模式、与 netfilter 的关系 | 可编程点在协议栈的哪个位置？ |
| [08 HTTP 协议解析与高性能服务](docs/08-HTTP协议解析与高性能服务实现.md) | 消息边界、解析器状态机、HTTP/1.1→2→3 | 内核不提供消息边界，那谁来提供？ |
| [09 内核网络观测与性能调优](docs/09-内核网络观测与性能调优.md) | 观测漏斗、计数器含义、sysctl 调优表、丢包定位 | 丢包/延迟到底丢在哪一层？ |
| [10 源码阅读地图与实验手册](docs/10-源码阅读地图与实验手册.md) | 目录地图、现象→源码速查、六条主干调用链 | 我想看某个机制的代码，从哪下手？ |

---

## 实验导览（labs/）

每个实验都包含 `README.md`（含**真实运行输出**）、`Makefile`、源码，部分带 `run.sh`。
约定：需要 root 的实验，其 `Makefile all` **必须编译通过并退出码 0**（打印 SKIP 说明）。

| 实验 | 主题 | 本机权限 | 关键内核入口 |
|---|---|---|---|
| [01-socket-api-basics](labs/01-socket-api-basics/) | socket API、缓冲区记账、epoll LT/ET、`TCP_INFO` | ✅ 可跑 | `net/core/sock.c`、`net/ipv4/af_inet.c` |
| [02-tcp-state-machine](labs/02-tcp-state-machine/) | TCP 状态机、accept 队列溢出、TIME_WAIT | ✅ 可跑 | `net/ipv4/tcp_input.c`、`tcp_minisocks.c` |
| [03-netlink-rtnetlink](labs/03-netlink-rtnetlink/) | 手写 rtnetlink dump 链路/地址/路由 | ✅ 可跑 | `net/core/rtnetlink.c` |
| [04-raw-socket-arp](labs/04-raw-socket-arp/) | AF_PACKET 原始帧、ARP 构造与解析 | ✅ 可在 `unshare -Urn` 内跑 | `net/packet/af_packet.c`、`net/ipv4/arp.c` |
| [05-tun-tap-userspace-stack](labs/05-tun-tap-userspace-stack/) | TUN 设备 + 用户态最小协议栈（能 ping 通） | ⚠️ 需 root + `/dev/net/tun` | `drivers/net/tun.c` |
| [06-kernel-module-netfilter](labs/06-kernel-module-netfilter/) | **内核模块**：netfilter hook + per-CPU 统计 + procfs | ⚠️ 编译✅ / 加载需 root | `net/netfilter/core.c` |
| [07-http-parser](labs/07-http-parser/) | **HTTP/1.1 增量解析器** + epoll 教学服务器 | ✅ 可跑 | `net/ipv4/tcp.c`（`tcp_recvmsg`） |
| [08-ebpf-xdp](labs/08-ebpf-xdp/) | eBPF/XDP：手写指令 / libbpf 示例 | ⚠️ 编译✅ / 加载需 root | `net/core/filter.c`、`kernel/bpf/` |
| [09-observability](labs/09-observability/) | 观测工具箱：`ss`/`/proc/net`/sock_diag/丢包定位 | ✅ 大部分可跑 | `net/ipv4/inet_diag.c` |

另附：
- [scripts/env-check.sh](scripts/env-check.sh)：一条命令打印内核版本、capabilities、工具可用性、
  关键 sysctl 快照，以及「本机能跑哪些实验」的结论表。
- [scripts/netns-veth-demo.sh](scripts/netns-veth-demo.sh)：**无需 root** 的真实网卡路径实验
  —— 自动进入用户命名空间建 netns + veth、把 MTU 调到 1280，实测出
  `FragOKs 0→1 / FragCreates 0→3` 与接收侧 `ReasmReqds=3 / ReasmOKs=1`，
  并用 `lo`（MTU 65536）做对照证明「环回永远不分片」。这是理解 MTU/PMTU 最直观的一课。

---

## 环境与已知限制（请先读）

本教程在下列**受限环境**中编写并验证（Debian 13 / Linux 6.12，uid=1000 非 root）：

| 能力 | 状态 | 影响 |
|---|---|---|
| gcc 14 / make / python3 | ✅ 有 | 用户态实验与内核模块编译都没问题 |
| 内核头文件（`/lib/modules/$(uname -r)/build`） | ✅ 有 | 外部内核模块可**编译**，`netlab.ko` 可产出 |
| `ip` / `ss` / `nc` / `socat` / `curl` / `ping` | ✅ 有 | 观测与手工造报文够用 |
| **`unshare -Urnmpf --mount-proc`（用户+网络+PID+挂载命名空间）** | ✅ **可用**（实测） | **能建 netns/veth、改 MTU、用 AF_PACKET 原始套接字**，见下方说明 |
| 初始命名空间里的 `CAP_NET_RAW` / `CAP_NET_ADMIN` / `CAP_BPF` | ❌ 全无（`CapEff=0`） | 不能在**初始命名空间**里抓包/改配置；但进 userns 后 `CapEff=000001ffffffffff` |
| `sudo` / `setcap` | ❌ 无效 | `NoNewPrivs=1` 且 `CapBnd=0`，file capability 与空边界集求交仍为空 |
| `/dev/net/tun` | ❌ 不存在 | labs/05 只能编译 + 给 root 步骤（userns 内 `mknod` 返回 EPERM） |
| `clang` / `libbpf` / `bpftool` / `tcpdump` / `perf` / `tc` / `ethtool` | ❌ 未安装 | labs/08 的 libbpf 示例会 SKIP；追踪模板只能给命令 |
| `sysctl` / `modprobe` / `modinfo` / `lsmod` | ⚠️ 在 `/usr/sbin`、`/sbin`（不在本 shell 的 `PATH`） | 直接敲会报「未找到命令」，**但命令其实都在**（procps 提供 `/usr/sbin/sysctl`，kmod 提供 `/sbin/modprobe`→`/bin/kmod`）。教程统一用 `cat /proc/sys/...`，用绝对路径调用即可 |
| `/sys/kernel/debug`、`/sys/kernel/tracing` | ❌ 不可读 | ftrace/perf 模板需 root，已在文档中标注 |

> **关于 `unshare -Urnmpf`（实测结论，很重要）**
> 虽然初始 `CapEff=0`，但创建**用户命名空间**后在新命名空间内拥有完整 capability，
> 因此可以在里面创建网络命名空间、建 veth 对、改 MTU、使用 `AF_PACKET`：
>
> ```bash
> # /proc 会变成新 netns 的视图（InReceives 从 0 开始）；必须带 -p 才能重挂 /proc
> unshare -Urnmpf --mount-proc bash
>   ip link add v0 type veth peer name v1
>   ip addr add 10.7.7.1/24 dev v0 && ip link set v0 up mtu 1280
>   ip link show v0          # 注意：验证 MTU 要用 ip，不能用 /sys（见下）
> ```
>
> 两个限制：① `/sys` **无法重新挂载**（`mount -t sysfs` 返回 EPERM），所以
> `ls /sys/class/net` 看到的仍是宿主机接口，统计要看 `/proc/net/dev` 或 `ip -s link`；
> ② `ip netns add` 需要写 `/run/netns`（本机只读），改用两个 `unshare -n` 进程 + `nsenter`。
> 这也意味着 **labs/04 的原始套接字/ARP 实验在本机是真能跑的**（见其 README 的双 netns 配方），
> 而 labs/05 因为缺 `/dev/net/tun` 设备节点仍然跑不了。


**因此本教程的取舍是**：纯用户态能验证的（socket、netlink、HTTP 解析、内核计数器观测、内核模块编译）
一律**真跑真贴输出**；需要特权的部分提供**可编译代码 + root 执行步骤 + 明确原因**，绝不虚构运行结果。

同样的代码在你自己的机器上（有 root）能获得更完整的体验，各 lab 的 README 里都写了对应的 root 步骤。

---

## 约定

- **文档**：`docs/NN-主题.md`，固定结构（定位 → 概念 → 数据结构 → 数据流 → 观测 → API 对照 → 陷阱与自测 → 小结）。
- **代码**：C11，`-Wall -Wextra -Wshadow` 下**零警告**；Python 3；关键处中文注释解释「为什么」。
- **不写行号**：内核代码行号随版本变化，本教程只给文件与函数名，核对入口为
  [elixir.bootlin.com/linux/v6.12](https://elixir.bootlin.com/linux/v6.12/source/)。
- **每个 lab 的 `all` 退出码必须为 0**，否则会中断顶层 `make`。

---

## 建议阅读顺序

- **系统路线（8 周）**：`00 → 01 → [02 + labs/01,02] → [03 + labs/02] → [04 + labs/03,04] →
  [05 + labs/05] → [06 + labs/06] → [07 + labs/08] → [08 + labs/07] → [09 + labs/09] → 10`，
  每周「读一章 + 做一个实验」，详细周计划与产出要求见
  [docs/00-环境准备与学习方法.md](docs/00-环境准备与学习方法.md) 第 1 节。
- **速通路线（4 周）**：压缩为「全景 → 观测 → 传输层 → HTTP 解析」四步，见 docs/00 同节。
- **1 天极速版**：`01`（全景）→ `labs/09`（学会观测）→ `08` + `labs/07`（HTTP 解析）→ `10`（速查表）。
- **只关心内核子模块**：`01` → `05` → `06` + `labs/06`（写一个 netfilter 内核模块）→ `07` + `labs/08`。

---

## 参考

- 《Understanding Linux Network Internals》(Benvenuti)、《深入理解 Linux 网络：架构设计与内核实现》
- 《TCP/IP 详解 卷1》、《Linux 内核设计与实现》、《Linux 高性能服务器编程》
- RFC 7230 / 9112（HTTP/1.1）、RFC 9113（HTTP/2）、RFC 9000（QUIC）
- 内核源码在线交叉引用：<https://elixir.bootlin.com/linux/v6.12/source/>
- `man 7 tcp`、`man 7 socket`、`man 2 sendfile`、`man 2 splice`、`man 8 ss`

> 本教程以 **Linux 6.12** 为基准。函数名与文件路径在 6.x 内总体稳定，细节会演进；
> 遇到对不上的地方，请在 elixir 上核对，**不要相信任何写死的行号**。

---

## 许可证

[MIT](LICENSE)。示例代码可自由用于学习、二次开发与商业项目；文档转载请注明出处。

## 质量门禁

本仓库的代码不是「写完就交」，而是由两道可自动执行的关卡守住：

```bash
make check   # 9 个 lab 全部从零重建，断言「零 warning / 零 error」
make test    # HTTP 解析器 27057 条断言（整包 / 逐字节 / 穷举切分点）
```

当前状态：`make check` 9/9 通过、`make test` 断言 0 失败、183 个文档相对链接 0 断链。
各 lab 的 `run.sh` 会在自己目录下生成 `.out/`（本机真实运行日志）。
该目录**不随仓库提供**（见 `.gitignore`）——运行一次实验即可得到，用它和文档里的输出逐条对照。

