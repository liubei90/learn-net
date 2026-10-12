# 07 · eBPF、XDP 与可编程数据面

> 在 XDP 出现之前，想在内核里改一个包的处理逻辑，只有两条路：**改内核源码重编译**，
> 或者**写 netfilter 模块**（仍然是内核态代码，改一次要 insmod 一次）。
> eBPF 把这条路变成了「**把一段经过验证器检查的字节码挂到指定位置**」——
> 不用重编译内核、不用重启、崩溃也不会拖着内核一起死。
>
> 本章要讲清三件事：**eBPF 到底是什么**、**它能挂在协议栈的哪些位置**、
> **XDP 为什么是最早的那个点**。配套实验：
> [labs/08-ebpf-xdp](../labs/08-ebpf-xdp/)（含一个**不需要 clang/libbpf** 的
> 手写指令示例，让你看清 eBPF 就是一段受限指令流）。

**定位**：可编程数据面（横切 ③~⑨ 层的多个挂载点）。前置：
[docs/01 全景](01-Linux网络子系统全景与数据流.md)、[docs/06 netfilter](06-netfilter-conntrack与NAT.md)
（对比「声明式规则」与「可编程」两种数据面扩展方式）。

---

## 0. 本章定位

```
应用层                     socket filter（cBPF/eBPF）↘
────────────────────────────────────────────────────────
套接字层   sockops / sk_msg / sk_skb / sk_reuseport   ← eBPF 挂载点
传输层     sock_ops / tcp 拥塞控制（struct_ops）       ← eBPF 挂载点
网络层     tc ingress/egress（SCHED_CLS/ACT）、kprobe/tracepoint（观测）
────────────────────────────────────────────────────────
链路层     ★ XDP：驱动收到包、**还没分配 skb** 时就能处理
驱动/网卡   XDP native / offload（网卡硬件执行）
────────────────────────────────────────────────────────
```

**XDP 的特殊性**：它挂在**驱动层最早的位置**——DMA 把包写进 ring buffer 之后、
内核**还没有为它分配 `sk_buff`** 之前。所以：

- 它能做到 **`XDP_DROP` 直接把包丢掉，成本几乎为零**（对 DDoS 清洗极有价值）；
- 它**看不到** `sk_buff`（只有 `struct xdp_md` + 原始数据指针）；
- 它**不能用**依赖 skb 的 helper；
- 它比 netfilter 早得多：`XDP → tc → netfilter(PRE_ROUTING) → 路由 → ...`。

```
包到达网卡
   │
   ├─ XDP（native，驱动里）：XDP_DROP / PASS / TX / REDIRECT
   │        ↓ PASS
   ├─ 分配 skb（napi_alloc_skb）
   ├─ tc ingress（cls_bpf / act_bpf）
   ├─ netfilter PRE_ROUTING（conntrack / DNAT / filter）
   ├─ 路由判定 → LOCAL_IN / FORWARD
   └─ 交付 socket → （可再挂 socket filter / sk_msg）
```

---

## 1. 核心概念与设计权衡

### 1.1 eBPF 是什么：一段受限的、可验证的指令流

**最小认知**：eBPF 程序就是**一组 64 位指令**（`struct bpf_insn` 数组），
提交给内核 `bpf(BPF_PROG_LOAD, ...)`，由内核：

1. **验证（verifier）**：控制流可达性、寄存器类型与边界、内存访问范围、
   是否可能无限循环、helper 调用是否与程序类型匹配……
   **不通过就拒绝加载**——这是 eBPF 敢在内核里跑的核心原因。
2. **JIT 编译**成原生机器码（`bpf_jit_enable`）。
3. **挂载**到某个挂载点（socket / tc / XDP / kprobe / tracepoint / cgroup…）。
4. 运行时通过 **map** 与用户态交换数据，通过 **helper** 调用内核提供的能力。

```c
/* include/uapi/linux/bpf.h —— 权威定义（本机头文件实测） */
struct bpf_insn {
    __u8  code;      /* opcode：BPF_CLASS | BPF_OP | BPF_SRC ... */
    __u8  dst_reg:4; /* 目的寄存器（低 4 位） */
    __u8  src_reg:4; /* 源寄存器（高 4 位） */
    __s16 off;       /* 跳转偏移 / 栈偏移 */
    __s32 imm;       /* 立即数 */
};
```

寄存器约定（10 个 64 位通用寄存器）：

| 寄存器 | 用途 |
|---|---|
| `r0` | 返回值（helper 调用后也是 r0 传回） |
| `r1`–`r5` | 调用 helper 时的参数 |
| `r6`–`r9` | 被调用者保存（跨 helper 调用保留） |
| `r10` | 只读的**栈帧指针**（512 字节栈） |

指令编码（`include/uapi/linux/bpf_common.h` + `bpf.h`，本机实测）：

| 类别 | 值 | 说明 |
|---|---|---|
| `BPF_LD` / `BPF_LDX` / `BPF_ST` / `BPF_STX` | 0x00/0x01/0x02/0x03 | 载入 / 载入到寄存器 / 存储 / 寄存器存储 |
| `BPF_ALU` / `BPF_ALU64` | 0x04 / **0x07** | 32 位 / 64 位算术 |
| `BPF_JMP` / `BPF_JMP32` | 0x05 / **0x06** | 64 位 / 32 位跳转 |
| `BPF_RET` / `BPF_MISC` | 0x06 / 0x07（**仅 cBPF 类空间**：定义在 `uapi/linux/bpf_common.h`） | 注意这两个值与 eBPF 的 `BPF_JMP32`/`BPF_ALU64` **数值相同但含义完全不同**——同一个字节在 cBPF 和 eBPF 里解释不一样，这就是「cBPF 必须先被 `bpf_convert_filter()` 迁移成 eBPF」的原因。eBPF 用 `BPF_EXIT`(0x90) 返回 |
| `BPF_MOV` | 0xb0 | 寄存器搬移（配 `BPF_ALU64` 得 64 位 mov） |
| `BPF_ARSH` | 0xc0 | 算术右移 |
| `BPF_END` | 0xd0 | 字节序转换（配 `BPF_TO_LE`/`BPF_TO_BE`） |
| `BPF_JEQ`/`BPF_JNE`/`BPF_JGT`/`BPF_JGE`/`BPF_JSGT`/`BPF_JSGE` | 0x10/0x50/0x20/0x30/0x60/0x70 | 比较跳转 |
| `BPF_CALL` | 0x80 | 调 helper（`imm` = helper 编号） |
| `BPF_EXIT` | 0x90 | 返回 |
| `BPF_K` / `BPF_X` | 0x00 / 0x08 | 立即数 / 寄存器作为第二操作数 |

> **看懂了这张表，你就看懂了 eBPF 的本质。** [labs/08](../labs/08-ebpf-xdp/) 里
> `bpf_socket_filter.c` 用**纯 C + `syscall(__NR_bpf, ...)`** 手写指令去统计并放行包，
> 完全没有 clang/libbpf 参与——README 里有逐条指令的解释。

### 1.2 挂载点（程序类型）与它们的分工

| 程序类型 | 挂载方式 | 能看到什么 | 典型用途 |
|---|---|---|---|
| `BPF_PROG_TYPE_SOCKET_FILTER` | `setsockopt(SO_ATTACH_BPF)` | 该 socket 的包 | 过滤（cBPF/eBPF） |
| `BPF_PROG_TYPE_SCHED_CLS` / `SCHED_ACT` | `tc filter add ... bpf` | skb、可改包 | 数据面处理、转发、限速 |
| `BPF_PROG_TYPE_XDP` | `ip link set dev X xdp obj ...` | `xdp_md` + 原始数据 | 丢弃/重定向/LB/DDoS |
| `BPF_PROG_TYPE_KPROBE` / `TRACEPOINT` / `RAW_TRACEPOINT` | perf/`bpf_link` | 内核任意函数/事件 | 可观测 |
| `BPF_PROG_TYPE_PERF_EVENT` | perf event | 采样 | 性能剖析 |
| `BPF_PROG_TYPE_CGROUP_SKB` | cgroup | 该 cgroup 的包 | 容器级网络策略 |
| `BPF_PROG_TYPE_SK_MSG` / `SK_SKB` | sockmap | 流式收/发 | 加速代理（sockmap） |
| `BPF_PROG_TYPE_SOCK_OPS` | `setsockopt(TCP_BPF_...)` | TCP 事件 | 自定义 TCP 行为 |
| `BPF_PROG_TYPE_SK_REUSEPORT` | `SO_ATTACH_REUSEPORT_EBPF` | 新连接 | 自定义负载均衡 |
| `BPF_PROG_TYPE_STRUCT_OPS` | 注册内核操作集 | 如拥塞控制 | TCP 拥塞算法（如 BBR 替代实现） |

程序类型枚举值（本机 UAPI 实测）：
`UNSPEC=0, SOCKET_FILTER=1, KPROBE=2, SCHED_CLS=3, SCHED_ACT=4, TRACEPOINT=5,
XDP=6, PERF_EVENT=7, CGROUP_SKB=8, ...`

### 1.3 XDP 的三种模式

| 模式 | 在哪执行 | 性能 | 前提 |
|---|---|---|---|
| **native（driver）** | 网卡驱动内部，DMA 之后、skb 分配之前 | 最高 | 驱动支持（`net_device_ops.ndo_bpf`） |
| **generic（skb）** | 协议栈里，skb 已分配之后 | 较低（但比 netfilter 早） | 任何设备都可 |
| **offload** | 网卡硬件 | 最高 | 网卡固件支持 |

```c
/* include/uapi/linux/bpf.h —— XDP 返回码（本机实测枚举值） */
enum xdp_action {
    XDP_ABORTED  = 0,  /* 出错：触发 tracepoint 并丢弃 */
    XDP_DROP     = 1,  /* 丢弃 */
    XDP_PASS     = 2,  /* 交给正常协议栈 */
    XDP_TX       = 3,  /* 从同一网卡发回 */
    XDP_REDIRECT = 4,  /* 重定向到别的网卡（如 AF_XDP 或 devmap） */
};
```

`XDP_DROP` 之所以快，是因为它**连 skb 都不用分配**——省掉 `napi_alloc_skb`、
省掉整套协议栈遍历。这就是「用 XDP 做 DDoS 清洗」的物理基础。

### 1.4 eBPF vs netfilter：两种数据面扩展方式

| 维度 | netfilter（iptables/nftables） | eBPF |
|---|---|---|
| 扩展方式 | **声明式**规则（表/链/匹配/动作） | **命令式**程序（写代码） |
| 表达能力 | 受限于已有 match/target | 图灵完备（有界循环、map、helper） |
| 复杂度 | 简单规则很容易写 | 需要工具链、调试更难 |
| 执行位置 | 固定 5 个 hook 点 | 多个挂载点，**可以更早（XDP）** |
| 有状态处理 | conntrack 内置 | 需自己在 map 里维护状态 |
| 性能 | 规则线性匹配（nft 可用 set 优化） | JIT 后近似原生代码 |
| 安全模型 | 规则配置错误影响面有限 | 验证器保证不崩内核，但**逻辑 bug 仍会丢包** |
| 观测能力 | 计数器 | 全栈 kprobe/tracepoint，能力更强 |

**结论（实践建议）**：过滤/NAT 优先用 nftables（成熟、可维护）；
需要**更早丢弃**、**自定义转发**、**极致性能**、**深度观测**时才上 eBPF/XDP。

### 1.5 map：eBPF 与用户态的接口

| map 类型 | 值（本机实测） | 特点 |
|---|---|---|
| `BPF_MAP_TYPE_HASH` | 1 | 通用哈希 |
| `BPF_MAP_TYPE_ARRAY` | 2 | 定长数组，索引 O(1)，**每 CPU 独立**时无锁 |
| `BPF_MAP_TYPE_PERF_EVENT_ARRAY` | 4 | 向用户态推送事件 |
| `BPF_MAP_TYPE_PERCPU_HASH` / `PERCPU_ARRAY` | — | 每 CPU 一份，避免竞争 |
| `BPF_MAP_TYPE_LPM_TRIE` | 11 | 最长前缀匹配（路由/ACL 场景） |
| `BPF_MAP_TYPE_DEVMAP` | 14 | XDP_REDIRECT 的目标设备表 |
| `BPF_MAP_TYPE_XSKMAP` | 17 | AF_XDP 零拷贝套接字表（注意别和 `SOCKMAP`=15、`CPUMAP`=16 记混） |
| `BPF_MAP_TYPE_RINGBUF` | 27 | 高效事件环形缓冲（取代 perf buffer） |

**per-CPU map 的意义**：XDP 跑在 NAPI（软中断）上下文，**不能加锁等待**。
用 per-CPU map 让每个 CPU 写自己的副本，用户态再聚合——这与
[labs/06](../labs/06-kernel-module-netfilter/) 里内核模块用 `this_cpu_inc()` 是同一个思路。

### 1.6 AF_XDP 与 DPDK

| 方案 | 内核参与度 | 性能 | 生态/易用性 |
|---|---|---|---|
| 普通 socket | 完整协议栈 | 基准 | 最好 |
| AF_XDP | XDP 把包直接送到用户态 ring，**零拷贝** | 高 | 需要自己实现协议栈 |
| DPDK | 完全绕过内核（轮询 + 用户态驱动） | 最高 | 独占 CPU/网卡，与内核生态割裂 |

AF_XDP 是「**既不放弃内核，又要高吞吐**」的折中：内核只负责把包搬进用户态 ring，
协议处理交给用户态。这也是 XDP 生态里最有商业价值的一块（如用户态 TCP/IP、负载均衡器）。

### 1.7 JIT：eBPF 从「解释执行」到「原生机器码」

eBPF 程序加载后有两种执行方式：**解释器**（`kernel/bpf/core.c` 里的
`___bpf_prog_run()`）和 **JIT**（x86-64 上是 `arch/x86/net/bpf_jit_comp.c`）。
生产环境一律用 JIT——解释执行一次要走一个巨大的 switch，性能差一个数量级。

控制 JIT 的是一组**全局变量**（不是模块参数），声明在 `include/linux/filter.h`：

```c
extern int  bpf_jit_enable;      /* 0 = 关闭 JIT（只解释），1 = 开启           */
extern int  bpf_jit_harden;      /* 0 = 关；1 = 只对非特权程序做常量盲化；2 = 全部 */
extern int  bpf_jit_kallsyms;    /* 1 = 把 JIT 代码登记到 kallsyms（便于 perf 符号化） */
extern long bpf_jit_limit;       /* JIT 可执行内存总量上限                      */
extern long bpf_jit_limit_max;   /* 上限的硬顶（只读）                          */
```

它们通过 sysctl 暴露为 `/proc/sys/net/core/bpf_jit_*`。**本机实测**：

```console
$ ls -l /proc/sys/net/core/bpf_jit_*
-rw-r--r-- 1 nobody nogroup 0 ... bpf_jit_enable      # 0644：普通用户可读
-rw------- 1 nobody nogroup 0 ... bpf_jit_harden      # 0600：仅 root
-rw------- 1 nobody nogroup 0 ... bpf_jit_kallsyms    # 0600：仅 root
-rw------- 1 nobody nogroup 0 ... bpf_jit_limit       # 0600：仅 root
$ cat /proc/sys/net/core/bpf_jit_enable
1
$ grep -E '^CONFIG_(BPF_JIT|BPF_JIT_DEFAULT_ON|HAVE_EBPF_JIT)' /boot/config-$(uname -r)
CONFIG_HAVE_EBPF_JIT=y
CONFIG_BPF_JIT=y
CONFIG_BPF_JIT_DEFAULT_ON=y
```

几个要点：

- **`bpf_jit_harden` 是安全开关**：JIT 会把常量直接编码进生成的机器码，
  攻击者可以借此在可执行内存里"拼"出指令（JIT spraying）。开启 harden 后，
  常量会被盲化（用随机数异或后再还原），代价是程序变长、性能略降。
- **`bpf_jit_kallsyms=1` 让 JIT 代码出现在 `/proc/kallsyms`**，
  这样 perf/bpftrace 才能把采样落到具体 BPF 程序名上；代价是泄漏内核地址，
  所以它默认只在 `CAP_SYS_ADMIN` 视角可见。
- **`bpf_jit_limit`** 是 JIT 可执行内存的总闸门（`bpf_jit_charge_modmem()` 记账），
  防止用户把内核内存耗光。
- 本机 `CONFIG_BPF_JIT_DEFAULT_ON=y`，所以 `bpf_jit_enable` 开机就是 1。

### 1.8 BTF 与 CO-RE：一次编译，跨内核版本运行

**问题**：BPF 程序要读内核结构体字段（比如 `struct sock` 的 `sk_state`），
但不同内核版本、不同配置下字段偏移完全不同。老办法（BCC）是**把 C 代码字符串
带到目标机器上现场编译**，依赖目标机装好内核头文件——重、慢、还要在线上装编译器。

**BTF（BPF Type Format）** 是内核自带的一份"类型描述"数据，可以理解成
被大幅精简过的调试信息（DWARF 的替代品）。它由 `kernel/bpf/btf.c` 解析，
在 `/sys/kernel/btf/` 下暴露：

```console
$ ls -l /sys/kernel/btf/vmlinux          # 本机实测
-r--r--r-- 1 nobody nogroup 4964214 ... /sys/kernel/btf/vmlinux
$ ls /sys/kernel/btf/ | wc -l
194                                       # vmlinux + 193 个模块各自的 BTF
$ grep -E '^CONFIG_DEBUG_INFO_BTF' /boot/config-$(uname -r)
CONFIG_DEBUG_INFO_BTF=y
CONFIG_DEBUG_INFO_BTF_MODULES=y
```

**CO-RE（Compile Once – Run Everywhere）** 建立在这份 BTF 之上：

1. **编译期**：clang 在 `__builtin_preserve_access_index()` 标记的字段访问处，
   记录"我要访问 `struct sock` 的 `sk_state`"这种**类型+路径**信息，
   生成重定位记录（放在 ELF 的 `.BTF.ext` 段），而不是写死偏移。
2. **加载期**：libbpf 读**目标内核的 BTF**，按类型路径重新算出偏移，
   改写指令里的立即数。相关实现在 `tools/lib/bpf/`（`relo_core.c` 等）。

于是同一个 `.bpf.o` 可以在 5.15、6.1、6.12 上直接跑，不需要重编译。

而写 BPF 程序时用的 `vmlinux.h` 就是 BTF 的 C 形式：

```console
$ bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

> **本机的真实限制**：`/sys/kernel/btf/vmlinux` **存在且可读**（4964214 字节），
> 说明 CO-RE 的数据前提是满足的；但本机**没有 bpftool**（生成 `vmlinux.h` 用不了）
> 也**没有 clang / libbpf**，所以 CO-RE 这条路在本机走不通。
> 反过来也说明：**BTF 是"内核能力"，工具链是"用户态能力"，两者要分开看。**

`vmlinux.h` 只描述内核类型，**不包含 libc/系统头文件**——BPF 程序里不能随手
`#include <stdio.h>`，这也是 BPF C 代码看起来"怪"的原因。

### 1.9 eBPF 的能力边界（写代码前必须知道）

| 边界 | 具体限制 | 依据（本机同版本头文件实测） |
|---|---|---|
| **不能睡眠** | XDP/tc/socket filter 跑在软中断或原子上下文：不能调用可能睡眠的 helper，不能用可能阻塞的操作。这是「XDP 里没有 `kmalloc(GFP_KERNEL)`」的根本原因 | 验证器 + 各程序类型的 `func_proto` |
| **指令数上限** | 单个程序 `BPF_MAXINSNS = 4096` 条 | `include/uapi/linux/bpf_common.h` |
| **验证复杂度上限** | `BPF_COMPLEXITY_LIMIT_INSNS = 1000000`（源码注释就写着 "yes. 1M insns"） | `include/linux/bpf.h` |
| **栈只有 512 字节** | `MAX_BPF_STACK = 512`，且栈偏移必须是负的、按访问宽度对齐 | `include/linux/filter.h` |
| **循环必须有界** | 验证器必须能证明循环会终止（有界循环支持从 5.3 起）；无界循环直接拒绝 | `kernel/bpf/verifier.c` |
| **变量大小内存受限** | `BPF_MAX_VAR_SIZ = (1 << 29)` | `include/linux/bpf_verifier.h` |
| **map 大小与内存记账** | map 内存受 rlimit / memcg 记账约束；JIT 内存受 `bpf_jit_limit` 总闸门约束 | `kernel/bpf/syscall.c`、`kernel/bpf/core.c` |
| **强内核版本相关** | 新 helper、新 map 类型只存在于较新内核；枚举值只增不改（所以**别硬编码**跨版本语义） | `include/uapi/linux/bpf.h` |
| **CO-RE 需要目标内核有 BTF** | 没有 BTF 的内核只能退回 BCC（运行时编译）或手工适配 | `kernel/bpf/btf.c`、`CONFIG_DEBUG_INFO_BTF` |
| **权限门槛** | 需要 `CAP_BPF`（tracing 还要 `CAP_PERFMON`，挂 XDP 还要 `CAP_NET_ADMIN`）；`kernel.unprivileged_bpf_disabled` 一旦为 **2** 就**不可逆** | `kernel/bpf/syscall.c` |
| **验证器只保证"不崩内核"** | 它**不保证业务逻辑正确**。一个写错的 `XDP_DROP` 条件可以直接把线上流量打死 | 设计取舍，不是 bug |

> **本机为什么是 `unprivileged_bpf_disabled = 2`（一条完整的因果链，全部实测）**：
> `kernel/bpf/syscall.c` 里写的是
> ```c
> int sysctl_unprivileged_bpf_disabled __read_mostly =
>         IS_BUILTIN(CONFIG_BPF_UNPRIV_DEFAULT_OFF) ? 2 : 0;
> ```
> 而本机 `/boot/config-6.12.107+deb13-amd64` 里 `CONFIG_BPF_UNPRIV_DEFAULT_OFF=y`，
> 所以开机即为 **2（永久禁用）**——这是有意的加固默认值，`sysctl -w` 改不回 0，
> 想改只能改内核启动参数（`bpf_unpriv_disabled=0`）并重启。

### 1.10 工具链选择：libbpf / BCC / bpftrace / bpftool

| 工具 | 是什么 | 编译时机 | 适合 | 代价 |
|---|---|---|---|---|
| **libbpf** | 内核源码树里的官方 C 库（`tools/lib/bpf/`） | **编译期**（clang 编出 `.bpf.o`，libbpf 只负责加载/重定位） | 生产级数据面、要发布二进制、要 CO-RE | 需要自己搭构建（vmlinux.h + skeleton），学习曲线陡 |
| **BCC** | Python/Lua 前端 + 内嵌 clang | **运行期**（在目标机现场编译） | 快速写运维脚本、临时排障 | 目标机要装 clang + 内核头文件；启动慢；每台机器都要编译 |
| **bpftrace** | 类 awk 的高级追踪语言（底层用 libbpf） | 运行期（把一句话编译成 BPF） | **一行命令**定位问题，最省事 | 表达力受语言限制，不适合做数据面 |
| **bpftool** | 内核源码树里的自省工具（`tools/bpf/bpftool/`） | — | `prog show` / `map dump` / `btf dump` / `gen skeleton` | 只是查看与脚手架，不写业务 |

**选择建议**：
- 只是**看一眼问题** → `bpftrace`
- 写**一次性运维脚本** → BCC
- 做**要长期跑、要发布、要跨版本**的东西 → **libbpf + CO-RE**
- 做 **XDP 数据面** → 一定是 libbpf 路线（`xdp_drop.bpf.c` 就是这种形态）

---

## 2. 关键内核数据结构与接口

### 2.1 XDP 程序看到的「包」

```c
/* include/uapi/linux/bpf.h（简化） */
struct xdp_md {
    __u32 data;          /* 数据起始偏移（相对 frame） */
    __u32 data_end;      /* 数据结束偏移 */
    __u32 data_meta;     /* 元数据区起始（可 writable 传信息给后续程序） */
    __u32 ingress_ifindex;
    __u32 rx_queue_index;
    __u32 egress_ifindex;   /* 较新版本加入 */
};

/* XDP 程序里访问包数据必须做边界检查，否则验证器直接拒绝：
 *   void *data = (void *)(long)ctx->data;
 *   void *data_end = (void *)(long)ctx->data_end;
 *   struct ethhdr *eth = data;
 *   if ((void *)(eth + 1) > data_end) return XDP_PASS;   // 必须这样检查
 */
```

> **验证器为什么这么严格**：XDP 跑在驱动上下文，越界读会直接崩溃整台机器。
> 所以「先判边界、再访问」不是风格问题，是**加载前提**。

### 2.2 挂载点相关的内核侧入口

| 层 | 内核入口 | 源码 |
|---|---|---|
| socket filter 挂载 | `sk_attach_filter` / `SO_ATTACH_BPF` → `bpf_prog_get_type` | `net/core/filter.c`、`net/core/sock.c` |
| 包经过 socket filter | `sk_filter_trim_cap()` | `net/core/filter.c` |
| tc eBPF | `cls_bpf_classify` / `tcf_classify` | `net/sched/cls_bpf.c` |
| XDP 驱动入口 | `ndo_bpf` / `netdev_bpf`；驱动里 `bpf_prog_run_xdp()` | `include/linux/netdevice.h`、各驱动；`bpf_prog_run_xdp()` 本身是 `include/net/xdp.h` 里的 `static __always_inline` |
| XDP generic | `do_xdp_generic()` / `netif_receive_generic_xdp()` | `net/core/dev.c` |
| 程序加载 | `__sys_bpf` → `bpf_prog_load` → `bpf_check`（验证器）→ `bpf_prog_select_runtime`（JIT） | `kernel/bpf/syscall.c`、`kernel/bpf/verifier.c`、`kernel/bpf/core.c` |
| map 创建 | `map_create()` → `bpf_map_alloc` | `kernel/bpf/syscall.c`、`kernel/bpf/hashtab.c` 等 |
| helper 表 | `bpf_base_func_proto` / 各类型的 `func_proto` | `kernel/bpf/helpers.c`、`net/core/filter.c` |

### 2.3 权限模型（本机为什么跑不了）

| 检查 | 说明 |
|---|---|
| `CAP_BPF`（bit 39） | 加载需要 `BPF_PROG_TYPE_*` 的大多数类型 |
| `CAP_PERFMON`（bit 38） | perf event / tracing 相关 |
| `CAP_NET_ADMIN` | XDP/tc 挂载到网卡 |
| `kernel.unprivileged_bpf_disabled` | 本机 = **2**（永久禁用；值 2 表示运行时**无法再启用**） |
| `kernel.perf_event_paranoid` | 本机 = 3（最严） |

本机实测：`CapEff=0`、`unprivileged_bpf_disabled=2`、无 clang/libbpf/bpftool，
因此 **eBPF 程序无法加载**；[labs/08](../labs/08-ebpf-xdp/) 的做法是：
**手写指令的程序真编译通过 + 运行时打印 SKIP 与确切原因 + 给出 root 下的完整步骤**。

---

## 3. 数据流：一个包在可编程数据面里的完整旅程

```
① 网卡 DMA 写入 RX ring
② 硬中断 → napi_schedule
③ 软中断 net_rx_action → napi_poll
④ 驱动 poll：为每个包
     ├─ 【XDP native 挂载点】bpf_prog_run_xdp(prog, xdp)
     │      · XDP_DROP     → 立即回收 page，零成本丢弃（DDoS 清洗）
     │      · XDP_PASS     → 继续下面的正常路径
     │      · XDP_TX       → 从同一网卡发回（如 LB 的回包）
     │      · XDP_REDIRECT → 到 devmap（转发）或 xskmap（AF_XDP 用户态）
     ├─ napi_alloc_skb（分配 skb —— 到这里才进入「传统协议栈」）
     └─ napi_gro_receive
⑤ netif_receive_skb → __netif_receive_skb_core
     ├─ 【tc ingress / cls_bpf 挂载点】sch_handle_ingress()
     ├─ rx_handler（bridge/bonding）
     └─ ptype_base → ip_rcv
⑥ ip_rcv → 【netfilter PRE_ROUTING（conntrack/DNAT/filter）】
⑦ 路由 → LOCAL_IN【netfilter INPUT】→ tcp_v4_rcv → socket
⑧ 【socket filter 挂载点】sk_filter_trim_cap()  ← tcpdump 用的就是这里（cBPF）
⑨ recv() 把数据交给应用
```

**顺序记住一句话**：`XDP（无 skb）→ tc → netfilter → socket filter`，
越靠前拦截，代价越小。

---

## 4. 可观测点与验证方法

| 目的 | 命令 | 前提 |
|---|---|---|
| 查看已加载的 BPF 程序 | `bpftool prog show` | 需 bpftool + 权限 |
| 查看 map 内容 | `bpftool map dump id N` | 同上 |
| 查看 XDP 挂载情况 | `ip link show dev eth0`（出现 `xdp` 标记）、`bpftool net show` | CAP_NET_ADMIN |
| 观察丢包原因 | `bpftrace -e 'tracepoint:skb:kfree_skb { @[args->reason] = count(); }'` | root + bpftrace |
| 观察 TCP 重传 | `bpftrace -e 'tracepoint:tcp:tcp_retransmit_skb { @[kstack] = count(); }'` | root |
| 观察连接状态迁移 | `bpftrace -e 'tracepoint:sock:inet_sock_set_state { printf("%d -> %d\n", args->oldstate, args->newstate); }'` | root |
| 确认 JIT 是否开启 | `cat /proc/sys/net/core/bpf_jit_enable`（若存在） | 只读 |
| 列出可用 tracepoint | `ls /sys/kernel/tracing/events/tcp/` | 需 tracefs 可读 |

**本机实测的限制**（如实说明，避免读者误判）：
`/sys/kernel/tracing` 与 `/sys/kernel/debug` 均**不可读**（权限不够），
`unprivileged_bpf_disabled=2`（永久禁用），且没有 bpftrace/bpftool/perf。
因此上面的命令在本机**都无法执行**；它们需要 root + 可写 tracefs + 安装工具。
完整的命令模板见
[labs/09-observability/06-tracepoint-templates.md](../labs/09-observability/06-tracepoint-templates.md)。

---

## 5. 用户态 API ↔ 内核实现对应表

| 用户态 | 内核入口 | 源码 | 要点 |
|---|---|---|---|
| `bpf(BPF_MAP_CREATE)` | `map_create()` | `kernel/bpf/syscall.c` | 参数在 `union bpf_attr` |
| `bpf(BPF_PROG_LOAD)` | `bpf_prog_load()` → `bpf_check()` | 同上 + `kernel/bpf/verifier.c` | 验证失败返回 `EACCES`/`EINVAL` 并打印 verifier 日志 |
| `setsockopt(SO_ATTACH_BPF)` | `sk_attach_bpf()` | `net/core/sock.c`、`net/core/filter.c` | 程序类型必须是 `SOCKET_FILTER` |
| `setsockopt(SO_ATTACH_FILTER)` | `sk_attach_filter()` | `net/core/filter.c` | 经典 cBPF（tcpdump 的 `-ddd` 输出就是它） |
| `tc filter add ... bpf obj` | `cls_bpf` 的 `->change()` | `net/sched/cls_bpf.c` | 需要 `SCHED_CLS` 类型 |
| `ip link set dev X xdp obj` | `dev_change_xdp_fd()` → `ndo_bpf()` | `net/core/dev.c` + 驱动 | native 模式；`xdpgeneric` 走 `do_xdp_generic()` |
| `BPF_MAP_LOOKUP_ELEM` | `bpf_map_lookup_elem()` | `kernel/bpf/syscall.c` | 用户态批量读统计常用 |
| `bpf_prog_run_xdp()` | — | `include/net/xdp.h` | `static __always_inline`，供驱动在 native 路径里直接调用 |
| bpftrace/bcc | libbpf → `bpf()` | 用户态库 | 最终都落到同一个系统调用 |

---

## 6. 常见陷阱、调优与自测题

### 6.1 陷阱清单

1. **验证器报错会被误读成「内核不支持」**。绝大多数是边界检查缺失、寄存器未初始化、
   或者用了当前程序类型不允许的 helper。**先看 verifier 日志**。
2. **XDP 里访问包数据必须做 `data_end` 边界检查**，否则加载被拒。
3. **XDP 跑在 NAPI 上下文，不能睡眠**：不能用 `bpf_ktime_get_ns()` 之外的重操作、
   不能做可能阻塞的 map 操作（如带 `BPF_F_LOCK` 的竞争），也不能调用会睡眠的 helper。
4. **XDP generic 模式比 native 慢**，而且它看到的 `data` 可能包含非线性数据，
   需要用 `bpf_xdp_adjust_head()` 等 helper 处理，不能直接假设线性。
5. **map 满了会静默失败**：`bpf_map_update_elem` 返回错误但你的程序可能忽略了它。
6. **per-CPU map 读出来是「每 CPU 一份」**，用户态必须自己聚合，否则看到的数字对不上。
7. **`unprivileged_bpf_disabled=2` 是不可逆的**：不要在调试时随意设成 2。
8. **改包要同步改校验和**：eBPF 不替你算，校验和错了包会被对端丢弃（或触发重传）。
9. **XDP_DROP 丢掉的包看不到常规统计**：网卡的 `rx_dropped` 不会增加，
   必须靠 XDP 程序自己的 map 计数，否则「丢了多少」你完全不知道。
10. **安全边界**：验证器保证「不会崩内核」，但**不保证你的逻辑正确**。
    XDP DROP 写错条件可以直接打挂业务——务必先在镜像流量上验证。

### 6.2 自测题（≥8）

1. XDP 与 netfilter 的 `PRE_ROUTING` 相比，**早在哪里**？这个「早」带来了什么能力与限制？
2. 为什么 `XDP_DROP` 比 iptables `DROP` 便宜得多？
3. eBPF 验证器至少会检查哪几类问题？为什么它是 eBPF 能进内核的前提？
4. XDP 的 generic 模式与 native 模式的区别是什么？什么时候必须用 native？
5. 为什么 XDP 程序里不能直接用 `skb`？`struct xdp_md` 给你什么？
6. per-CPU map 解决什么问题？用户态读的时候要注意什么？
7. 用 XDP 做负载均衡时，回包通常怎么处理？（提示：`XDP_TX` 与 `XDP_REDIRECT`）
8. AF_XDP 与 DPDK 的核心差别是什么？各自适合什么场景？
9. `BPF_PROG_TYPE_STRUCT_OPS` 能做什么？举一个网络领域的例子。
10. 你要在**不改内核**的前提下给某个 TCP 拥塞算法打点统计，选哪种程序类型 +
    哪个挂载点？为什么不用 XDP？

<details>
<summary>参考答案要点</summary>

1. 早**在 skb 分配之前**，也早于 tc 与整个协议栈。能力：可以极低成本丢弃、改包、重定向，
   在 DDoS 场景下省掉全部协议栈开销。限制：**没有 skb**，不能用依赖 skb 的 helper，
   看不到路由/连接跟踪状态，很多「高层语义」都拿不到。
2. `XDP_DROP` 时包还只是 ring buffer 里的 DMA buffer，驱动直接回收 page，
   **从未分配 sk_buff、从未进入协议栈**；iptables `DROP` 时已经走完 NAPI、分配了 skb、
   做过路由判定，账单已经付了。
3. 控制流可达性与无环（有界循环）、寄存器类型与初始化状态、内存访问边界、
   helper 与程序类型匹配、栈越界、指针泄漏到 map、可能的分除零等。
   因为内核不能容忍一段用户提供的代码随意执行——验证器把「可能崩溃」变成「拒绝加载」。
4. generic 在**协议栈里**执行（skb 已分配），所有设备都支持但开销大；
   native 在**驱动**里执行，性能最好但需要驱动实现 `ndo_bpf`。
   追性能/做 DDoS 清洗必须 native（或 offload）。
5. 因为 native XDP 执行时 skb 还不存在。`struct xdp_md` 只给原始数据指针
   （`data`/`data_end`/`data_meta`）与入接口/队列号，你得自己解析以太网帧与 IP 头。
6. 解决多 CPU 写同一 map 的竞争（XDP/tc 在软中断上下文不能阻塞等锁）。
   用户态读出来后要**把所有 CPU 的值加起来**才是总数。
7. 常见做法：`XDP_TX` 让包从**收到它的那块网卡原路发回**（回包路径对称、省一次路由）；
   或者用 `XDP_REDIRECT` 跳到 `devmap` 里的目标设备（跨网卡转发）。
   关键限制：XDP 程序只能**转发**，不能自己构造回包（除非配合 `bpf_xdp_adjust_head` 改写）。
8. AF_XDP 仍然**通过内核**把包搬进用户态 ring（零拷贝），与内核共享网卡；
   DPDK **完全绕过内核**、轮询独占网卡与 CPU 核。
   AF_XDP 适合「要在用户态做协议处理但仍需与内核共存」的场景；
   DPDK 适合纯用户态数据面、可独占资源的场景。
9. 允许用 eBPF 实现内核里以「操作集结构体」形式存在的可插拔接口，
   网络领域最典型的是 **TCP 拥塞控制**（`struct tcp_congestion_ops`），
   也可以用 `SO_ATTACH_REUSEPORT_EBPF` 做自定义分流。
10. 用 `BPF_PROG_TYPE_KPROBE` 或 `TRACEPOINT`（甚至 `RAW_TRACEPOINT`）挂在
    对应的内核函数/事件上（如 `tcp_retransmit_skb`）。不选 XDP 是因为：
    重传发生在**发送方向、协议栈内部**，XDP 在接收早期路径上，
    **根本看不到**重传事件，也拿不到 TCP 的上下文。
</details>

---

## 7. 本章小结

- eBPF = **受限指令流 + 验证器 + JIT + map + helper + 挂载点**；
  它的价值在于「不重编译内核就能改数据面行为」。
- 挂载点决定了能力的边界：**XDP 最早（无 skb）→ tc → netfilter → socket filter**。
- XDP 的核心优势是 `XDP_DROP` **零 skb 成本**与 `XDP_REDIRECT` 的灵活转发；
  代价是拿不到高层语义、必须自己做边界检查、不能用 skb 相关 helper。
- 与 netfilter 是**互补**而非替代：过滤/NAT 用 nftables，极致性能与深度观测用 eBPF/XDP。
- 本机无法加载 eBPF（`CapEff=0`、`unprivileged_bpf_disabled=2`、无 clang/libbpf），
  [labs/08](../labs/08-ebpf-xdp/) 因此提供「**手写指令 + 真编译 + root 步骤**」的路径，
  让你至少能把指令编码和加载流程看清楚。

## 8. 延伸阅读

- 内核文档：`Documentation/bpf/`（在 elixir 上看，含 `bpf_design_QA.rst`、`ringbuf.rst`）
- 权威头文件（本机即有，可直接读）：
  `/usr/src/linux-headers-$(uname -r)/include/generated/` 与
  `/usr/src/linux-headers-$(uname -r | sed 's/-[^-]*$//')-common/include/uapi/linux/bpf.h`。
  本文档实测机器的真实路径是
  `/usr/src/linux-headers-6.12.107+deb13-common/include/uapi/linux/bpf.h`
  与 `/usr/src/linux-headers-6.12.107+deb13-amd64/`
  （Debian 把内核头文件拆成 `<ver>-amd64`（架构相关，含 `Makefile`/`Module.symvers`）和
  `<ver>-common`（通用 `include/` 树）两个包，只装其中一个会编译失败）。
- 源码入口：`kernel/bpf/syscall.c`、`kernel/bpf/verifier.c`、`kernel/bpf/core.c`、
  `net/core/filter.c`、`net/sched/cls_bpf.c`
- 书籍/文档：BPF and XDP Reference Guide（Cilium）、`man 2 bpf`、`man 7 bpf-helpers`
- 相关章节：[docs/06 netfilter](06-netfilter-conntrack与NAT.md)（两种扩展方式对比）、
  [docs/05 链路层/NAPI](05-链路层-网卡驱动-NAPI与中断.md)（XDP 与 NAPI 的关系）、
  [docs/09 观测](09-内核网络观测与性能调优.md)（tracepoint/bpftrace 模板）

> **版本提示**：本章的枚举值与指令编码均取自本机同版本 UAPI 头文件，可直接复核。
> helper 编号、`bpf_attr` 字段会随版本增加，写代码时请以你机器上的 `bpf.h` 为准，
> 不要硬编码 helper 编号之外的东西。
