# labs/08 · eBPF / XDP

> 这个实验的回答一个具体问题：**eBPF 程序到底是什么？**
> 不用 clang、不用 libbpf、不引入任何第三方库，只用 `syscall(__NR_bpf, ...)`
> 加上一段**手写的指令数组**，把程序加载起来——这样你就不会再觉得 eBPF 神秘了。

**权限说明（先说清楚）**：加载 eBPF 需要 `CAP_BPF`（或 root）。本机
`CapEff=0` 且 `kernel.unprivileged_bpf_disabled=2`（永久禁用），
所以**本机跑不出「统计递增」的结果**。本实验因此做到：

| 本机 ✅ 能验证 | 本机 ❌ 不能验证（需 root/CAP_BPF） |
|---|---|
| 手写指令的编码正确（编译零警告，code 值全部来自 UAPI） | 程序能否通过验证器 |
| 加载所需的 syscall 序列与 `bpf_attr` 字段 | `SO_ATTACH_BPF` 后统计是否真的递增 |
| 失败时的 errno 与**确切原因** | XDP 程序的编译（需 clang）与挂载（需 CAP_NET_ADMIN） |

---

## 1. 实验目的

1. 亲手写出 **eBPF 指令数组**，理解 `struct bpf_insn` 的 4 个字段与指令编码规则。
2. 走通完整的 `bpf()` 系统调用序列：`BPF_MAP_CREATE` → `BPF_PROG_LOAD` → `SO_ATTACH_BPF` → `BPF_MAP_LOOKUP_ELEM`。
3. 看清 **XDP 与 socket filter 的位置差异**（一个没有 skb、一个在收包路径末端）。
4. 学会一种工程习惯：**环境不支持时，打印确切原因并优雅退出，而不是伪造成功**。

## 2. 前置知识

- 读完 [docs/07-eBPF-XDP与可编程数据面.md](../../docs/07-eBPF-XDP与可编程数据面.md)
- 了解 [docs/05 链路层/NAPI](../../docs/05-链路层-网卡驱动-NAPI与中断.md) 的收包流程（XDP 在哪个位置）
- C 语言：结构体指定初始化、位域

## 3. 文件清单

| 文件 | 作用 | 本机能否构建 |
|---|---|---|
| `bpf_insns.h` | 手写指令的编码宏（**每个编码都注明来自哪个 UAPI 头文件**） | ✅ |
| `bpf_socket_filter.c` | 建 map、加载手写程序、挂到 socket、读回统计 | ✅ 编译并运行（加载被拒 → SKIP） |
| `xdp_drop.h` | BPF 侧与用户态侧**共享的约定**（丢弃端口、程序名、map 名、目标文件名）。刻意做成**零依赖的纯宏头文件**（不 include 任何头、不用任何类型），因为 BPF 目标（`clang -target bpf`）能用的头文件极其有限 | ✅（被两边包含） |
| `xdp_drop.bpf.c` | 标准 libbpf 风格 XDP 程序（含边界检查与 IHL 解析） | ❌ 需 clang |
| `xdp_drop.c` | XDP 用户态加载器（attach/detach + 读 map） | ❌ 需 libbpf |
| `Makefile` | `all`/`xdp`/`run`/`clean`/`check-toolchain`；缺工具链时打印 SKIP 并退出 0 | — |
| `run.sh` | 一键跑：自检 → 构建 → 运行 → 小结 | — |

> **为什么要有 `xdp_drop.h`**：BPF 程序和用户态 loader 必须就
> 「端口号 / 程序名 / map 名」达成一致，但两边是**用不同编译器、不同头文件环境**分别编译的。
> 各写一份常量迟早会不一致，症状是"程序挂上了、统计却永远是 0"这种最难查的静默故障。
> 集中到一个头文件里是标准做法；而它必须零依赖，才能在 `-target bpf` 下被包含。
>
> 另外 `Makefile` 里给 clang 加了
> `-I/usr/include/$(shell $(CC) -dumpmachine)`：Debian/Ubuntu 把
> `asm/types.h` 放在 multiarch 目录下，而 `clang -target bpf` **不会**像 gcc 那样
> 自动搜索该目录，不加这行会直接报 `asm/types.h: No such file or directory`。

## 4. 逐步实验步骤

### 第 1 步：看指令编码（`bpf_insns.h`）

eBPF 指令就是 8 个字节：

```c
struct bpf_insn {          /* include/uapi/linux/bpf.h，本机实测 */
    __u8  code;            /* opcode = 类别 | 操作 | 源操作数 */
    __u8  dst_reg:4;       /* 目的寄存器（低 4 位） */
    __u8  src_reg:4;       /* 源寄存器（高 4 位）—— 两个字符合占 1 字节 */
    __s16 off;             /* 跳转偏移（相对**下一条**指令）或栈偏移 */
    __s32 imm;             /* 立即数 */
};
```

自己算一遍这几个常用编码（对照 `bpf_common.h`）：

| 想做的事 | 组合 | 结果 |
|---|---|---|
| `r0 = -1`（64 位立即数搬移） | `BPF_ALU64(0x07) \| BPF_MOV(0xb0) \| BPF_K(0x00)` | **0xb7** |
| `r1 += 1` | `BPF_ALU64(0x07) \| BPF_ADD(0x00) \| BPF_K` | **0x07** |
| `*(u32*)(r10-4) = 0` | `BPF_ST(0x02) \| BPF_MEM(0x60) \| BPF_W(0x00)` | **0x62** |
| `r1 = *(u32*)r0` | `BPF_LDX(0x01) \| BPF_MEM(0x60) \| BPF_W` | **0x61** |
| `if r0 == 0 goto +3` | `BPF_JMP(0x05) \| BPF_JEQ(0x10) \| BPF_K` | **0x15** |
| 调用 helper | `BPF_JMP(0x05) \| BPF_CALL(0x80)` | **0x85** |
| 返回 | `BPF_JMP(0x05) \| BPF_EXIT(0x90)` | **0x95** |
| 加载 map fd（伪指令，占 2 槽） | `BPF_LD(0x00) \| BPF_DW(0x18) \| BPF_IMM(0x00)` + `src_reg=BPF_PSEUDO_MAP_FD(1)` | **0x18** |

### 第 2 步：读懂那 13 条指令（`bpf_socket_filter.c`）

```c
[0]  r6 = -1                       // 返回值先存到被调用者保存寄存器（helper 会覆盖 r0..r5）
[1]  *(u32 *)(r10 - 4) = 0         // key = 0 放在栈上
[2]  r2 = r10                      // helper 第 2 个参数 = key 指针
[3]  r2 += -4
[4]  r1 = <map 指针>               // BPF_PSEUDO_MAP_FD：加载器把 imm 替换成 map 指针
[5]  (占位，64 位立即数的第二个槽)
[6]  call 1                        // BPF_FUNC_map_lookup_elem → r0 = 指向 value 的指针
[7]  if (r0 == 0) goto +3           // 查不到就走“返回”
[8]  r1 = *(u32 *)r0               // 读计数
[9]  r1 += 1
[10] *(u32 *)r0 = r1               // 写回计数
[11] r0 = r6                       // 恢复返回值
[12] exit                          // socket filter 返回“保留多少字节”，-1 = 全部保留
```

**两个必须记住的细节**：

1. **跳转偏移是相对「下一条指令」**的，不是相对当前指令。写错一位就是从别的路径返回，
   验证器不一定报错，但语义全错。
2. **helper 调用会破坏 `r0`–`r5`**。所以返回值必须放在 `r6`–`r9`，这也是为什么第 [0] 条
   是 `r6 = -1` 而不是 `r0 = -1`。

### 第 3 步：构建与运行

```bash
make            # 构建（bpf_socket_filter 一定编译；XDP 部分缺工具链则 SKIP）
make run        # 或 ./run.sh 跑完整流程
make check-toolchain
```

### 第 4 步：在 root 环境下真正跑起来

```bash
sudo ./bpf_socket_filter          # 无需任何额外依赖
sudo bpftool prog show            # 观察已加载的程序
sudo bpftool map dump id <id>     # 观察 map
```

XDP 部分需要额外依赖：

```bash
sudo apt-get install -y clang llvm libbpf-dev     # Debian/Ubuntu
make xdp
sudo ./xdp_drop lo                # 把 XDP 程序挂到 lo 上（Ctrl-C 卸载）
# 或（等价效果，不需要自己写加载器）：
sudo ip link set dev lo xdp obj xdp_drop.bpf.o sec xdp
sudo ip link set dev lo xdp off
```

> **务必记得卸载**：XDP 程序会一直挂在网卡上，直到你 detach 或重启。
> 这也是 `xdp_drop.c` 里专门用 `SIGINT` + `bpf_xdp_detach()` 的原因。

## 5. 真实运行输出（本机实测，退出码 0）

### 5.1 工具链与权限自检

```
━━━ 1. 工具链与权限自检 ━━━
clang            : <缺失>
bpf_helpers.h    : <缺失>
libbpf 运行库     : <缺失>
内核 bpf.h       : 存在
unprivileged_bpf_disabled: 2
  CapEff                          : 0000000000000000
  /sys/kernel/tracing 可读        : no（权限不够）
  bpftrace / bpftool              : 缺失 / 缺失
```

### 5.2 构建：零警告 + XDP 优雅 SKIP（退出码 0）

```
cc -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings -o bpf_socket_filter bpf_socket_filter.c
SKIP: 无法构建 XDP/libbpf 示例，缺少:
        · clang（用于把 C 编译成 eBPF 字节码，-target bpf）
        · /usr/include/bpf/bpf_helpers.h（libbpf-dev）
        · libbpf 运行库（libbpf1 / libbpf-dev）
      安装（Debian/Ubuntu）: sudo apt-get install -y clang llvm libbpf-dev
      装好后执行: make xdp && sudo ./xdp_drop <ifname>
  构建退出码：0
```

### 5.3 手写程序的加载被拒（**这就是本机应有的结果**）

```
=== labs/08 · 手写 eBPF socket filter（不依赖 clang / libbpf）===

  环境事实：
    kernel.unprivileged_bpf_disabled = 2
    kernel.perf_event_paranoid      = 3

  [BPF_MAP_CREATE] 失败: Operation not permitted (errno=1)

═══════════════════════════════════════════════════════════════
 SKIP: 本机无法加载 eBPF 程序（errno=1: Operation not permitted）
═══════════════════════════════════════════════════════════════
 这是**预期结果**，不是代码错误。原因（本机实测）：
   · CapEff = 0 —— 没有 CAP_BPF(bit 39) / CAP_PERFMON(bit 38)
   · kernel.unprivileged_bpf_disabled = 2（永久禁用非特权 BPF，运行时无法改回）
   · 即使 unshare -Urn 进入用户命名空间，BPF 校验仍在 init userns 上判断，依然失败
```

注意：失败发生在 **`BPF_MAP_CREATE`**，连 map 都建不了——**权限检查在第一步就拦住了**，
根本走不到验证器。这一点很多人搞错，以为「加载失败就是代码写错了」。

### 5.4 网卡上确实没有 XDP 程序

```
━━━ 4. 当前网卡上是否已挂 XDP 程序 ━━━
  没有任何网卡挂 XDP 程序（符合预期，本机无法加载）
```

## 6. XDP 与 socket filter 的差别（对照两个文件读）

| 维度 | `bpf_socket_filter.c`（本机可编译） | `xdp_drop.bpf.c`（需 clang） |
|---|---|---|
| 程序类型 | `BPF_PROG_TYPE_SOCKET_FILTER`(1) | `BPF_PROG_TYPE_XDP`(6) |
| 上下文 | `struct __sk_buff`（可读 `len` 等） | `struct xdp_md`（只有 `data`/`data_end` 等） |
| 挂载方式 | `setsockopt(SO_ATTACH_BPF)` | `bpf_xdp_attach()` / `ip link set ... xdp obj` |
| 执行位置 | 收包路径**末端**（TCP/UDP 已定位到 socket） | 驱动里**最早**，skb 还没分配 |
| 返回值 | 保留的字节数（0 = 丢） | `XDP_ABORTED/DROP/PASS/TX/REDIRECT` |
| 看到的包起点 | **IP 头**（TCP/UDP socket；L2 已剥） | **以太网头** |
| 必须做的检查 | 一般不需要 | **每次访问包数据前都要 `data_end` 边界检查** |
| 改动网卡状态 | 不需要 | 需要 `CAP_NET_ADMIN`，**必须记得卸载** |

> `xdp_drop.bpf.c` 里特意演示了两个新手最常犯的错：
> ① 不检查 `data_end` 就访问数据（验证器直接拒绝加载）；
> ② 用固定的 20 字节当 IP 头长度（真实 IP 头长度由 `iph->ihl * 4` 决定）。

## 7. 观察与思考题

1. `struct bpf_insn` 的 `dst_reg` 和 `src_reg` 是各占 4 位的位域，为什么这样设计？
   （提示：8 字节指令要装下 10 个寄存器 + 操作码）
2. 为什么第 [0] 条指令要写 `r6 = -1` 而不是 `r0 = -1`？如果把返回值放在 `r0` 会怎样？
3. `BPF_LD_MAP_FD` 为什么要占**两个**指令槽？（提示：64 位立即数 + 8 字节指令）
4. 本机失败在 `BPF_MAP_CREATE` 而不是 `BPF_PROG_LOAD`，这说明权限检查发生在**哪一步**？
5. 为什么 `xref` 里 XDP 程序每次访问包数据前都要比较 `data_end`？如果不做会怎样？
6. `xdp_drop.bpf.c` 为什么要用 `iph->ihl * 4` 而不是 20？不这么做会漏掉什么情况？
7. 为什么 XDP_DROP 丢掉的包**不会**出现在 `ip -s link` 的 dropped 统计里？
   那你怎么知道丢了多少？（提示：自己用 map 计数）
8. 同样的统计需求，用 `BPF_PROG_TYPE_KPROBE` 挂在 `tcp_retransmit_skb` 上能不能做？
   和 XDP 相比各自的视野边界在哪？
9. 为什么 `unprivileged_bpf_disabled=2` 是「不可逆」的？这个设计出于什么考虑？
10. 如果要在**不改内核**的情况下实现一个「按源 IP 黑名单丢包」的功能，
    你会选 XDP、tc-bpf 还是 nftables？分别说明理由。

<details>
<summary>参考答案要点</summary>

1. 10 个寄存器只需 4 位编码（0–10），两个寄存器正好 1 字节；剩下 1 字节给操作码。
   这样一条指令紧凑地是 8 字节（1+1+2+4），对齐友好、访存高效。
2. 因为 helper 调用（`BPF_CALL`）只保证 `r6`–`r9` 与 `r10` 不被破坏，`r0`–`r5` 会被覆盖。
   如果放 `r0`，`map_lookup_elem` 返回后 `r0` 已经变成「指向 value 的指针」，
   你最后 `exit` 返回的会是这个指针（验证器通常会直接拒绝这种返回值）。
3. 因为它是 64 位立即数（`BPF_DW`），一条指令只有 32 位 `imm` 字段，
   所以要两条：第一条装低 32 位（并带 `BPF_PSEUDO_MAP_FD` 标记），第二条装高 32 位。
4. 说明**权限检查在 `bpf()` 系统调用的入口就做了**（`bpf_cmd` 分发前的 capable 检查），
   早于验证器、早于 JIT。所以「没权限」和「程序写错」是两类完全不同的问题，
   报错位置就能区分开。
5. 因为 XDP 跑在驱动上下文，包数据位于 DMA buffer，长度由硬件描述符决定；
   越界读会读到不属于该包的内存（甚至读进别的 ring 条目），轻则数据错乱，重则崩溃内核。
   验证器强制要求「先证明访问在 `[data, data_end)` 内」，否则拒绝加载。
6. IP 头长度是**可变的**（`ihl` 以 4 字节为单位，通常 5，带选项时可达 15）。
   固定写 20 会在「带 IP 选项」的包上把 TCP/UDP 头解析到错误位置，
   结果是过滤器行为不正确（也常被用来绕过过滤）。
7. 因为包从来没成为「网卡收到的包」——它没有分配 `sk_buff`、没进协议栈，
   驱动的常规统计在更后面的环节才累加。想知道丢了多少，必须让 XDP 程序自己往 map 里计数。
8. 能。kprobe 挂 `tcp_retransmit_skb` 可以精确统计重传（包含调用栈、地址等丰富信息），
   因为它能看到完整内核上下文。但 XDP 看不到重传（重传在发送方向的协议栈内部发生），
   反之 kprobe 也无法像 XDP 那样在收包最早期丢弃——**视野不同，用途不同**。
9. 因为一旦设为 2，内核就不再接受把该值改回 0/1 的写入（`proc_dointvec_minmax` 的
   边界被固定为 `[2,2]`）。这是为了防止「先临时放开、再被攻击者利用」，
   属于加固默认：要么在启动时就允许非特权 BPF，要么永久关闭。
10. 都可以，取决于位置与需求：
    · **nftables**：最简单可维护，规则可读、可计数、可热更新，适合常规 ACL（推荐首选）；
    · **tc-bpf**：比 netfilter 早（skb 已分配但没有路由/连接跟踪开销），可编程、可按接口作用，
      适合需要自定义逻辑又要看 skb 的场景；
    · **XDP**：最早、最省，适合大流量黑名单/清洗，代价是拿不到 skb 与高层状态，
      且要自己解析以太网/IP 头、写边界检查。
</details>

## 8. 清理

```bash
make clean        # 删除 bpf_socket_filter / xdp_drop / xdp_drop.bpf.o / *.o
# 若曾经在网卡上挂过 XDP，务必卸载：
sudo ip link set dev <ifname> xdp off
```

## 9. 延伸阅读

- [docs/07-eBPF-XDP与可编程数据面.md](../../docs/07-eBPF-XDP与可编程数据面.md)（本章理论）
- 本机可读的权威头文件（直接对照最有价值）：
  `/usr/include/linux/bpf.h`、`/usr/include/linux/bpf_common.h`
- `Documentation/bpf/`（在 elixir.bootlin.com 上看）、Cilium 的 BPF & XDP Reference Guide
- 相关实验：[labs/06-kernel-module-netfilter](../06-kernel-module-netfilter/)（另一种内核态扩展方式：
  写真正的内核模块，对比「模块要重编译/加载」与「eBPF 可动态加载」的差别）、
  [labs/09-observability](../09-observability/)（tracepoint 观测模板）
