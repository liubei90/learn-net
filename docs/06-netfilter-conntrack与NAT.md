# 06 · netfilter、conntrack 与 NAT

> netfilter 是 Linux 网络栈里**唯一横切所有层**的框架：它不产生数据包，只在关键位置
> 提供「钩子」，让过滤、NAT、连接跟踪这些功能挂上去。理解了它的**挂载位置与优先级**，
> 你就能准确回答「我这条 iptables 规则到底在哪一步生效」「DNAT 之后路由会不会重查」
> 「为什么容器里的包看到的源地址不一样」这类问题。
>
> 配套实验：[labs/06-kernel-module-netfilter](../labs/06-kernel-module-netfilter/)
> —— 亲手写一个注册 netfilter hook 的内核模块（本机可真编译出 `netlab.ko`）。

**定位**：横切层（④ 网络层与 ⑥ 邻居层之间/之上）。前置：
[docs/01 全景](01-Linux网络子系统全景与数据流.md) 的收发路径、
[docs/04 网络层](04-网络层-IP路由与邻居子系统.md) 的路由与转发。

---

## 0. 本章定位

netfilter 的 hook 点**嵌在 IP 层的数据流里**，位置如下（把 docs/01 的收发路径收窄）：

```
                    ┌──────────────── 本机进程 ────────────────┐
                    │                                          │
                    │  recv()                          send()  │
                    ▲                                          ▼
              ┌─────┴──────┐                            ┌────────────┐
              │  LOCAL_IN  │ ①                          │ LOCAL_OUT  │ ④
              └─────▲──────┘                            └──────┬─────┘
                    │                                          │
   ┌──────────┐   ┌─┴──────────────┐                    ┌──────▼───────────┐
   │ 网卡/NAPI │──▶│  PRE_ROUTING   │ ②                │   POST_ROUTING   │ ⑤
   └──────────┘   └─┬────────────┬─┘                    └──────▲───────────┘
       ▲            │ 路由判定    │ 路由判定                     │
       │      ┌─────▼──────┐  ┌──▼─────────┐            ┌──────┴───────┐
       │      │ LOCAL_IN   │  │  FORWARD   │ ③          │  出接口/qdisc │
       │      └────────────┘  └──────┬─────┘            └──────────────┘
       │                             │
       └──────────── POST_ROUTING ───┘
```

五个 hook 点的**权威编号**（本机 `include/uapi/linux/netfilter_ipv4.h` 实测）：

| 宏 | 值 | 触发时机 | iptables 对应链 | nftables 对应 |
|---|---|---|---|---|
| `NF_IP_PRE_ROUTING` | 0 | 包刚进 IP 层，**还没做路由判定** | PREROUTING | `type filter hook prerouting` |
| `NF_IP_LOCAL_IN` | 1 | 路由判定为「给我的」之后 | INPUT | `hook input` |
| `NF_IP_FORWARD` | 2 | 路由判定为「要转发」 | FORWARD | `hook forward` |
| `NF_IP_LOCAL_OUT` | 3 | 本机发出，还没做路由查找 | OUTPUT | `hook output` |
| `NF_IP_POST_ROUTING` | 4 | 路由已定、即将出门 | POSTROUTING | `hook postrouting` |
| `NF_IP_NUMHOOKS` | 5 | （计数用） | — | — |

> **记住两个「反直觉」的点**：
> ① **PRE_ROUTING 早于路由判定**，所以在这里改目的地址（DNAT）会导致后续按新地址路由；
> ② **LOCAL_OUT 里的包还没有确定出接口**，所以这里只能按目的地址匹配，看不到 `-o eth0`。

---

## 1. 核心概念与设计权衡

### 1.1 hook 是怎么被调用的：`NF_HOOK` 宏

内核不会在 IP 层写 `if (iptables_has_rules) ...` 这种代码，而是在**路径上的固定位置**
放一个宏（`include/linux/netfilter.h`）：

```c
/* 简化后的语义：如果该 hook 点没有注册任何钩子，直接走 okfn（零开销） */
static inline int NF_HOOK(uint8_t pf, unsigned int hook, struct net *net,
                          struct sock *sk, struct sk_buff *skb,
                          struct net_device *in, struct net_device *out,
                          int (*okfn)(struct net *, struct sock *, struct sk_buff *))
{
    if (nf_hook_slow(...) ...)          /* 有钩子 → 进慢路径 */
        return 1;
    return okfn(net, sk, skb);          /* 没钩子 → 直调 */
}
```

**设计精髓**：`nf_hook_slow()` 里先查 `nf_hook_entries`，**没注册就返回 0（继续）**，
所以「没装 iptables 规则的机器」在 hook 点上几乎没有额外开销。这与「协议栈用注册表 + 函数指针」
的整体风格一致。

规则执行的控制流：

```
NF_HOOK → nf_hook_slow(net, pf, skb, state, hook_entries, ...)
   └─ 遍历该 hook 点按 priority 排序后的 ops 数组
        verdict = ops->hook(priv, skb, state)
        switch (verdict) {
        case NF_ACCEPT(1): 继续下一个 ops
        case NF_DROP(0):   丢弃 skb 并返回 0（后续 okfn 不再执行）
        case NF_STOLEN(2): 已接管，不再继续（例如 queue 到用户态）
        case NF_QUEUE(3):  交给 nfnetlink_queue（NFQUEUE）
        case NF_REPEAT(4): 重新执行当前 ops
        case NF_STOP(5):   无条件接受，跳过本 hook 点后续 ops
        }
```

**verdict 常量**（`include/uapi/linux/netfilter.h`）：

| 宏 | 值 | 含义 |
|---|---|---|
| `NF_DROP` | 0 | 丢弃，不再继续 |
| `NF_ACCEPT` | 1 | 放行，继续下一个钩子 |
| `NF_STOLEN` | 2 | 钩子自己接管了 skb（注意内存所有权） |
| `NF_QUEUE` | 3 | 排队到用户态（NFQUEUE / iptables `-j NFQUEUE`） |
| `NF_REPEAT` | 4 | 重新调用当前钩子 |
| `NF_STOP` | 5 | 立刻接受，**不再执行本 hook 点的其它钩子** |

### 1.2 优先级（priority）：同一 hook 点内部的执行顺序

这是 netfilter 里最容易出错、也最值得记住的一张表。**权威取值**来自本机
`include/uapi/linux/netfilter_ipv4.h`：

| 优先级 | 值 | 谁用 | 为什么在这个位置 |
|---|---|---|---|
| `NF_IP_PRI_FIRST` | `INT_MIN` | 头 |
| `NF_IP_PRI_RAW_BEFORE_DEFRAG` | −450 | raw 表（PREROUTING 里 defrag 之前） | 要让 NOTRACK 在重组前生效 |
| `NF_IP_PRI_CONNTRACK_DEFRAG` | −400 | 分片重组 | conntrack 需要完整包 |
| `NF_IP_PRI_RAW` | −300 | raw 表 | 最早期，可对连接打 NOTRACK |
| `NF_IP_PRI_SELINUX_FIRST` | −225 | SELinux | |
| `NF_IP_PRI_CONNTRACK` | **−200** | **conntrack** | 在做 NAT / 过滤之前先跟踪 |
| `NF_IP_PRI_MANGLE` | −150 | mangle 表 | 改 TTL/TOS/MARK |
| `NF_IP_PRI_NAT_DST` | **−100** | **DNAT**（PREROUTING/OUTPUT） | 在过滤之前改目的地址 |
| `NF_IP_PRI_FILTER` | **0** | **filter 表** | 中间的默认层 |
| `NF_IP_PRI_SECURITY` | 50 | SECURITY 表 | |
| `NF_IP_PRI_NAT_SRC` | **100** | **SNAT/MASQUERADE**（POSTROUTING） | 出门前最后改源地址 |
| `NF_IP_PRI_SELINUX_LAST` | 225 | SELinux | |
| `NF_IP_PRI_CONNTRACK_HELPER` | 300 | conntrack helper | |
| `NF_IP_PRI_CONNTRACK_CONFIRM` | `INT_MAX` | 确认连接 | 最后才入库 |

**三条推论**（比背数值更有用）：

1. **conntrack(−200) 早于 DNAT(−100) 早于 filter(0) 早于 SNAT(100)**。
   所以 filter 链里看到的**已经是 DNAT 之后的地址**。
2. 想「不过滤/不跟踪」某些流量：用 `raw` 表的 `-j CT --notrack`（在 −300 生效，早于 conntrack）。
3. 自己的模块要插在 filter 之前观察，就选一个在 (−100, 0) 之间的优先级；
   [labs/06](../labs/06-kernel-module-netfilter/) 用 `NF_IP_PRI_FIRST + 1` 是为了**尽早看到包**，
   代价是此时**还没有路由结果**（`skb_dst()` 可能为空，`state->out` 也可能为空）。

### 1.3 iptables 与 nftables：同一框架，两种前端

```
         ┌───────────────────────── netfilter 框架（不变） ─────────────────────────┐
         │  nf_hook_ops 注册表 / 优先级 / verdict / conntrack / NAT                 │
         └───────▲──────────────────────────────────────────────────▲──────────────┘
                 │                                                  │
    ┌────────────┴─────────────┐                    ┌───────────────┴───────────────┐
    │ xtables（iptables 前端）  │                    │ nftables 前端                  │
    │  net/ipv4/netfilter/      │                    │  net/netfilter/nf_tables_api.c │
    │  ip_tables.c: ipt_do_table│                    │  nf_tables_core.c: nft_do_chain│
    │  match/target 扩展 xt_*   │                    │  表达式（expression）组合       │
    │  setsockopt 传规则 blob   │                    │  netlink 传规则（增量、事务）   │
    └──────────────────────────┘                    └───────────────────────────────┘
```

| 维度 | iptables | nftables |
|---|---|---|
| 用户态→内核 | `setsockopt(IPT_SO_SET_REPLACE)` 整表替换 | netlink（`nf_tables` 家族），**增量、事务化** |
| 内核实现 | `ip_tables.c` 的 `ipt_do_table()`，规则线性遍历 | `nf_tables_core.c` 的 `nft_do_chain()`，支持 verdict map / set |
| 匹配效率 | 规则多则线性变慢 | 可用 set/map 做 O(1) 集合匹配 |
| 结构 | 表 / 链 / 规则三层固定 | 表 / 链（可自定义 hook 与 type）/ 表达式 |
| 语法 | `-A INPUT -p tcp --dport 80 -j ACCEPT` | `tcp dport 80 accept` |
| 现状 | 兼容层仍在（`iptables-nft` 其实已是 nft 后端） | 现代发行版默认 |

**关键认知**：多数现代发行版上 `iptables` 命令其实是 **nftables 的兼容前端**，
`iptables-save` 看到的规则与 `nft list ruleset` 是同一批对象的两种视图。
真正决定行为的是**框架里的 hook 注册与优先级**，而不是你用哪个命令。

### 1.4 连接跟踪（conntrack）：netfilter 的「状态数据库」

无状态过滤（只看单个包）无法表达「允许已建立的连接回包」。conntrack 为**每条连接**
在 `(协议, 源地址, 源端口, 目的地址, 目的端口)` 的**双向元组**上维护一个状态对象
（`struct nf_conn`）。

**状态机**（`/proc/net/nf_conntrack` 里 `state=` 显示的字符串）：

| 状态 | 含义 | 何时出现 |
|---|---|---|
| `NEW` | 只看到单向的第一个包 | SYN、第一个 UDP 包 |
| `ESTABLISHED` | 双向都见过包（`IPS_SEEN_REPLY`） | 回包之后 |
| `RELATED` | 由已有连接派生（如 FTP 数据连接、ICMP 差错关联） | helper 创建 |
| `INVALID` | 无法归类（如不在窗口内的包、校验和错） | 异常流量 |
| `UNTRACKED` | 被 `raw` 表 `NOTRACK` 明确排除 | 高性能旁路 |

`ip_conntrack_info` 的枚举值（本机 UAPI 实测）：
`IP_CT_ESTABLISHED=0`、`IP_CT_RELATED=1`、`IP_CT_NEW=2`、`IP_CT_IS_REPLY=3`、
`IP_CT_ESTABLISHED_REPLY=3`、`IP_CT_RELATED_REPLY=4`、`IP_CT_NUMBER=5`、`IP_CT_UNTRACKED=7`。
`ctinfo` 不是简单的「状态编号」，它与 `IP_CT_IS_REPLY` 位组合——读源码时别按直觉解析。

> **`INVALID` 不在这个枚举里**（别写成 `IP_CT_INVALID`，它不存在）：用户态看到的 `[INVALID]`
> 来自 `NF_CT_STATE_INVALID_BIT (1 << 0)`（uapi `nf_conntrack_common.h`）；内核侧则由
> `nf_conntrack_in()` 用**负返回值**把「非法」告诉 netfilter core（源码注释原文：
> *"Invalid: inverse of the return code tells the netfilter core what to do"*），
> 同时累加 `net->ct.stat->invalid`。表满/压力过大则是 `resolve_normal_ct()` 返回负值时
> 直接 `ret = NF_DROP` 并累加 `drop` —— 这就是「conntrack 表满丢包」的准确出处。

`struct nf_conn` 上还挂着一组状态位（`enum ip_conntrack_status`，本机 UAPI 实测）：
`IPS_EXPECTED`(bit0)、`IPS_SEEN_REPLY`(bit1)、`IPS_ASSURED`(bit2)、`IPS_CONFIRMED`(bit3)、
`IPS_SRC_NAT`(bit4)、`IPS_DST_NAT`(bit5)、`IPS_SEQ_ADJUST`(bit6)、
`IPS_SRC_NAT_DONE`(bit7)、`IPS_DST_NAT_DONE`(bit8)、`IPS_DYING`(bit9)、
`IPS_FIXED_TIMEOUT`(bit10)、`IPS_TEMPLATE`(bit11)、`IPS_UNTRACKED`(bit12)、
`IPS_HELPER`(bit13)、`IPS_OFFLOAD`(bit14)。

**注意 `IPS_SEQ_ADJUST`**：改地址还不够，TCP 序号也要修正，否则 NAT 后的连接会错乱——
这是 NAT 比想象中复杂的原因之一。

### 1.5 NAT：conntrack 的副产品

NAT **不是独立机制**，而是「根据 conntrack 记录改写后续包的地址/端口」：

```
第一个出去的包（NEW）→ NAT 决定映射 → 写入 nf_conn 的 nat 扩展（nf_conn_nat）
后续所有包（ESTABLISHED）→ 直接查 conntrack 拿到映射并改写 → 不再走规则匹配
```

| 类型 | hook / 优先级 | 行为 | 典型命令 |
|---|---|---|---|
| SNAT | POST_ROUTING / `NF_IP_PRI_NAT_SRC`(100) | 改**源**地址 | `-j SNAT --to-source 1.2.3.4` |
| MASQUERADE | POST_ROUTING / 100 | 按出接口地址动态 SNAT | `-j MASQUERADE`（拨号/容器出口） |
| DNAT | PREROUTING / `NF_IP_PRI_NAT_DST`(−100) | 改**目的**地址 | `-j DNAT --to-destination 10.0.0.5:8080` |
| REDIRECT | PREROUTING（`NF_IP_PRI_NAT_DST`）| DNAT 到本机 | `-j REDIRECT --to-ports 8080` |

**DNAT 之后必须重查路由**：因为目的地址变了，出接口和下一跳可能完全不同。
内核用 `ip_route_me_harder()` 重查（这也是很多「DNAT 后回包路径不对」问题的根源）。

### 1.6 扩展模型：`xt_match` / `xt_target` 与 iptables ↔ nftables 的兼容层

iptables 的「表 / 链 / 规则」并不是内核里的实体，而是 **x_tables 框架**提供的一层抽象。
一条规则在内核里是一段**紧凑结构体数组**：

```c
/* include/uapi/linux/netfilter_ipv4/ip_tables.h */
struct ipt_ip    { ... };   /* 标准匹配：源/目的/接口/... */
struct ipt_entry {          /* 一条规则 = 标准匹配 + 若干扩展匹配 + 一个 target */
    struct ipt_ip ip;
    __u16 target_offset;
    __u16 next_offset;      /* 下一条规则的偏移 —— 规则是"紧凑数组"，不是链表 */
};
struct ipt_replace { ... }; /* iptables 整表替换时下发的结构 */

/* uapi/.../x_tables.h：扩展匹配/动作的公共头；linux/netfilter/x_tables.h：内核侧接口 */
struct xt_entry_match { ... };   struct xt_entry_target { ... };
struct xt_match { ... };         struct xt_target { ... };
```

- **扩展注册**：匹配实现 `struct xt_match` + `xt_register_match()`；动作实现
  `struct xt_target` + `xt_register_target()`（两者都在 `net/netfilter/x_tables.c`）。
  你写的 `-p tcp --dport 80` 是 `xt_tcpudp` 这个 match，`-j SNAT` 是 `xt_nat` 这个 target。
- **求值**：`ipt_do_table()`（`net/ipv4/netfilter/ip_tables.c`）拿 skb **线性遍历**规则数组，
  逐个调 match 回调，命中就执行 target。规则越多越慢——ipset / nftables set 的存在理由。
- **整表替换**：`iptables -A` 走 `IPT_SO_SET_REPLACE`（`struct ipt_replace`）整表重下发，
  不是增量插入，所以规则多时每次改一条都很贵。

| 维度 | xtables（iptables） | nftables |
|---|---|---|
| 规则表示 / 下发 | `ipt_entry` 紧凑数组；`setsockopt` 整表替换 | 表达式链（字节码式）；netlink 事务化（`nf_tables_api.c`），可增量 |
| 求值函数 | `ipt_do_table()` | `nft_do_chain()`（`net/netfilter/nf_tables_core.c`） |
| 集合匹配 / 扩展 | 需 ipset；`xt_match` / `xt_target` | 原生 `set`/`map`；`nft_expr`（`nft_register_expr()`） |

两者挂在**同一批 hook 点和同一套 priority 排序**上，所以 nftables 更快不是因为 hook 位置不同，
而是因为**求值器效率**不同。

**兼容层**：现代发行版（含本机 Debian 13）的 `iptables` 多数是 **iptables-nft**——
命令只是翻译前端，把 xtables 请求翻成 nftables 事务；内核侧由
`net/netfilter/nft_compat.c` 把老 xt_* 模块包装成 nft 表达式
（`nft_match_init()` / `nft_target_init()` / `xt_request_find_match()`）。
所以在 iptables-nft 机器上，`iptables -A ...` 与 `nft list ruleset` 看到的是**同一批规则**；
`iptables --version` 出现 `(nf_tables)` 即 nft 后端。

> **别混淆两层 "compat"**：`nft_compat.c` + `nft_compat` 模块是
> **iptables 命令 ↔ nftables 求值器**的兼容；而 `xt_compat_*`
> （`net/netfilter/x_tables.c` 的 `xt_compat_match_from_user()` / `xt_compat_match_to_user()` /
> `xt_compat_init_offsets()` / `xt_compat_calc_jump()`）是
> **32 位用户态 ↔ 64 位内核**的结构体布局兼容（`struct xt_entry_match` 里有指针/`size_t`）。

### 1.7 两种「NAT 拓扑」与 untracked 优化

#### hairpin NAT（发夹 NAT / NAT 回环）

场景：内网客户端要访问**同一内网里**的服务，但服务是通过 DNAT 暴露的**公网地址**：
`client(10.0.0.9) → 公网IP:80 ──DNAT──→ 10.0.0.5:8080`。
问题在**回包**：`10.0.0.5` 看到源地址是 `10.0.0.9`（同网段），于是直接用内网路由
把回包发给 `10.0.0.9`，**绕过了做 NAT 的那台机器**。client 收到的包源地址是
`10.0.0.5:8080`，与自己发出的目标地址对不上，被内核当无效包丢掉。

解决办法是**让回包也经过 NAT 设备**：对「内网客户端 → 内网服务」这类连接
额外做一次 SNAT（或 MASQUERADE）`--to-source <NAT设备内网IP>`，
使服务端看到的源是 NAT 设备自己，回包回到 NAT 设备后再由 conntrack 反 NAT 还原。

**判据**：从内网访问公网映射地址不通、但从外网访问同一地址通 → 基本就是 hairpin 问题。

#### full-cone vs 对称 NAT（内核差异在哪）

这两个词来自 NAT 穿透（P2P/WebRTC）语境，说的是**外部端口映射的分配策略**：

| 类型 | 行为 | 内核侧含义 |
|---|---|---|
| **Full-cone** | 同一内网 `(src_ip, src_port)` 对任意外部目标复用**同一个**外部端口 | 映射键**不含**目的元组 |
| **Restricted cone** | 只有**收到过其包的**外部地址/端口才能回包 | 需维护"回包白名单"（`IPS_SEEN_REPLY` 等状态位） |
| **Symmetric** | 每换一个外部目标就分配**新的**外部端口 | 映射键**包含完整五元组** |

Linux 的 `MASQUERADE`/`SNAT` **默认行为接近对称 NAT**，具体由 `nf_nat_core.c` 的
`get_unique_tuple()` 决定：它综合源元组、目的元组、已有的 `nf_nat_bysource` 哈希
以及 `--persistent` 标志来选端口。实践含义：想更"友好"（更像 cone）就固定
`--to-source` 并用 `--persistent`；想更随机、更难预测就用 `--random`（端口随机）
或 `--random-fully`（连 IP 也随机）。这两组语义**不是开关**，而是选端口策略的结果——
所以调 NAT 穿透问题必须同时看内核版本和具体 NAT 参数。

#### untracked / `NOTRACK`：给 conntrack 减负

`raw` 表的 `-j CT --notrack`（nftables 里是 `notrack`）让指定流量**完全不进 conntrack 表**，
适合：纯转发大流量、上层自己处理的隧道流量、压测（避免 conntrack 成为瓶颈）。

`nf_conntrack_in()`（`net/netfilter/nf_conntrack_core.c`）开头就处理这件事：

```c
tmpl = nf_ct_get(skb, &ctinfo);
if (tmpl || ctinfo == IP_CT_UNTRACKED) {
        /* Previously seen (loopback or untracked)?  Ignore. */
        if ((tmpl && !nf_ct_is_template(tmpl)) ||
             ctinfo == IP_CT_UNTRACKED)
                return NF_ACCEPT;
        skb->_nfct = 0;
}
```

即看到 `IP_CT_UNTRACKED` 就**直接放行、不建表项**。不要写成"设置 `IPS_UNTRACKED`"——
`IPS_UNTRACKED`(bit12) 在 v6.12 源码注释里已标注 **"Obsolete and not used anymore"**。

**顺序很关键**：`raw` 的 `NF_IP_PRI_RAW`(−300) 必须**早于** conntrack 的
`NF_IP_PRI_CONNTRACK`(−200)，否则包已被跟踪，再 notrack 就是白费功夫——这是
hook 优先级设计里最经典的"顺序即语义"例子。

---

## 2. 关键内核数据结构

### 2.1 hook 注册：`struct nf_hook_ops`

```c
/* include/linux/netfilter.h（v6.12 原文，已用 labs/06 的可编译模块交叉验证） */
struct nf_hook_ops {
    /* User fills in from here down. */
    nf_hookfn        *hook;      /* unsigned int (*)(void *priv, struct sk_buff *skb,
                                                      const struct nf_hook_state *state) */
    struct net_device *dev;
    void             *priv;      /* 随 ops 传给 hook 的私有数据 */
    u8                pf;        /* 协议族：NFPROTO_IPV4 / NFPROTO_IPV6 / ... */
    enum nf_hook_ops_type hook_ops_type:8;  /* NF_HOOK_OP_UNDEFINED / NF_HOOK_OP_NF_TABLES / NF_HOOK_OP_BPF */
    unsigned int      hooknum;   /* NF_INET_* 五个点之一 */
    int               priority;  /* 越小越先执行（升序） */
};
```

> **两个最容易踩的坑（都是「老教程会写错」的地方）**：
> 1. **v6.12 的 `nf_hook_ops` 里没有 `struct list_head list;`**。注册后的 ops 会被
>    复制进 `nf_hook_entries` 自己的数组，不再靠链表挂链。照抄老代码多加一个 `list`
>    字段不会编译报错，但会让你对注册机制产生错误理解。
> 2. **钩子函数签名必须带 `void *priv` 作为第一个参数**（5.15 起）。
>    写成 `(struct sk_buff *, const struct nf_hook_state *)` 在 6.x 上会编译报错。
>    本教程的 [labs/06/netlab.c](../labs/06-kernel-module-netfilter/netlab.c) 用的是正确签名，
>    并且真的编译出了 `netlab.ko`，可直接对照。

注册与注销（现代内核都是 **per-netns** 的）：

```c
int  nf_register_net_hook(struct net *net, const struct nf_hook_ops *ops);
void nf_unregister_net_hook(struct net *net, const struct nf_hook_ops *ops);
/* 需要同时注册到多个 netns 时用 nf_register_net_hooks() */
```

**重要**：`nf_hook_entries` 会在注册/注销时**重建并按 priority 排序**，因此
**hook 注册是低频操作，数据路径上是一次数组遍历**——不要在数据路径里注册/注销。

### 2.2 conntrack 的核心结构

```c
/* include/net/netfilter/nf_conntrack_tuple.h（简化） */
struct nf_conntrack_tuple {
    struct nf_conntrack_man   src;    /* 源：地址 + 端口/ICMP id + 协议 */
    struct {
        union nf_inet_addr    u3;     /* 目的地址 */
        union { __be16 all; struct { __be16 port; } tcp; ... } u;
        u_int8_t              protonum;  /* L4 协议号 */
        u_int8_t              dir;       /* IP_CT_DIR_ORIGINAL / IP_CT_DIR_REPLY，
                                          * 定义在 uapi/linux/netfilter/nf_conntrack_tuple_common.h */
    } dst;
};

/* include/net/netfilter/nf_conntrack.h（按 v6.12 精简，字段顺序与真实定义一致） */
struct nf_conn {
    struct nf_conntrack      ct_general;      /* 引用计数 + rcu head */
    spinlock_t               lock;
    u32                      timeout;         /* jiffies32：视为死亡的时间点 */
#ifdef CONFIG_NF_CONNTRACK_ZONES
    struct nf_conntrack_zone zone;            /* 可选：conntrack zone */
#endif
    struct nf_conntrack_tuple_hash tuplehash[IP_CT_DIR_MAX];  /* 双向各一条，用于查表 */
    unsigned long            status;          /* IPS_* 状态位 */
    possible_net_t           ct_net;          /* 属于哪个 netns */
#if IS_ENABLED(CONFIG_NF_NAT)
    struct hlist_node        nat_bysource;    /* 注意：是 hlist_node，不是 hlist_nulls_node */
#endif
    struct nf_conn          *master;          /* expectation 的主连接 */
#if defined(CONFIG_NF_CONNTRACK_MARK)
    u_int32_t                mark;
#endif
    struct nf_ct_ext        *ext;             /* 扩展区（timeout/nat/helper/acct/...） */
    union nf_conntrack_proto proto;           /* 必须放最后：各协议的私有区 */
};
```

要点：

- **一个连接两条 tuple hash**（原始方向 + 回复方向），这样才能对回包直接命中。
- 元组哈希表 `nf_conntrack_hash`，桶数由 `nf_conntrack_buckets` 控制
  （本机实测 = 262144）。
- **扩展（extension）机制**：timeout、NAT、helper、accounting 等都以
  `nf_ct_ext` 的形式挂在 `nf_conn` 尾部，避免为所有连接分配用不到的内存。
- 每个 `struct nf_conn` 的内存开销约几百字节（取决于启用的扩展），
  所以 `nf_conntrack_max` 本质上是**内存上限**，不只是条目上限。

### 2.3 NAT 的结构

```c
/* include/net/netfilter/nf_nat.h —— v6.12 原文结构 */
struct nf_conn_nat {
    union nf_conntrack_nat_help help;   /* helper 相关的端口修正 */
#if IS_ENABLED(CONFIG_NF_NAT_MASQUERADE)
    int  masq_index;                    /* 只有编了 MASQUERADE 才有这个字段 */
#endif
};
```

NAT 的映射结果存在 conntrack 的 `nf_conn_nat` 扩展里，**后续包直接复用**，
所以「一万条规则只对第一个包生效」——这是 NAT 性能远好于想象的关键。

---

## 3. 数据流与函数调用链

### 3.1 一个入站包经过 netfilter 的完整路径

```
网卡 → NAPI → netif_receive_skb → ip_rcv → ip_rcv_core
  │
  ├─ NF_HOOK(NFPROTO_IPV4, NF_INET_PRE_ROUTING, ip_rcv_finish)
  │     └─ nf_hook_slow → 按 priority 依次执行：
  │          [−400] conntrack_defrag：ip_defrag() 先重组分片
  │          [−300] raw 表（iptable_raw / nft raw）：可 -j CT --notrack
  │          [−200] conntrack_in：查/建 nf_conn，设置 skb->_nfct
  │          [−150] mangle 表 PREROUTING（改 TTL/TOS/MARK）
  │          [−100] DNAT：nf_nat_inet_fn → 改写目的地址 + ip_route_me_harder()
  │          [   0] filter 表 PREROUTING（少见）
  │
  ├─ ip_rcv_finish → ip_rcv_finish_core（路由判定）
  │     ├─ 本机 → ip_local_deliver
  │     │     └─ NF_HOOK(NF_INET_LOCAL_IN, ip_local_deliver_finish)
  │     │          [−200] conntrack 已命中（不再新建）
  │     │          [   0] filter 表 INPUT ← 日常拒绝规则都在这里
  │     │          [ 300] conntrack_helper（如 FTP 需要开数据连接）
  │     │          └─ ip_local_deliver_finish → tcp_v4_rcv / udp_rcv
  │     └─ 转发 → ip_forward
  │           └─ NF_HOOK(NF_INET_FORWARD, ip_forward_finish)
  │                [   0] filter 表 FORWARD
  │                └─ ip_output → NF_HOOK(NF_INET_POST_ROUTING, ...)
  │                     [ 100] SNAT / MASQUERADE ← 改源地址
  │                     [ INT_MAX ] conntrack_confirm：确认并入库
  │                     └─ ip_finish_output → 出接口
```

**观测要点**：`state->hook` 与 `state->thresh` 在钩子里可以读到当前 hook 点，
`state->in`/`state->out` 给出入/出设备（在 PRE_ROUTING 时 `out` 还是 NULL）。
[labs/06](../labs/06-kernel-module-netfilter/) 的模块正是打印这些字段。

### 3.2 连接建立时 conntrack 与 NAT 的协作

```
① 第一个 SYN（NEW）
   conntrack_in：哈希表查不到 → nf_conntrack_alloc() 建 nf_conn
                 tuple[ORIGINAL] = (客户端:port → 服务端:80)
                 status 无 IPS_SEEN_REPLY，ctinfo = IP_CT_NEW
   DNAT(若有)：改 skb 目的地址，并把映射写入 nf_conn_nat
                （只在这一刻做决策，之后不再查规则）
   conntrack_confirm（POST_ROUTING，priority INT_MAX）：把 nf_conn 插入哈希表
                ⚠️ 只有 confirm 之后连接才真正"存在"；之前哈希表里查不到

② 服务端回包（SYN-ACK）
   conntrack_in：用 REPLY 方向元组查表 → 命中
                 status |= IPS_SEEN_REPLY，ctinfo = IP_CT_ESTABLISHED_REPLY
   反 NAT：按 nf_conn_nat 反向改写（DNAT 的逆操作 = 改回原目的地址）

③ 后续所有包：直接查表 + 按记录改写，O(1)，不再走规则匹配
```

**这解释了三件事**：

1. 为什么 `conntrack -L` 看不到「刚发出但还没 confirm」的连接。
2. 为什么 NAT 规则改动后**老连接仍然按老映射走**（映射已经固化在 `nf_conn` 里）。
3. 为什么表满时新连接会失败——`nf_conntrack_alloc()` 分配不到条目。

### 3.3 经典落地：docker 与 k8s 的 iptables 链

本机实测装了 docker + docker-compose，可见两个 bridge 及其路由（容器网络当前 DOWN）：

```console
$ ip -br addr
docker0          DOWN           172.17.0.1/16
br-0123456789ab  DOWN           172.18.0.1/16
$ ip route
172.17.0.0/16 dev docker0 proto kernel scope link src 172.17.0.1 linkdown
172.18.0.0/16 dev br-0123456789ab proto kernel scope link src 172.18.0.1 linkdown
```

`docker0` / `br-xxxx` 都是 Linux bridge（"虚拟交换机"），docker 给每个自定义网络建一个。

#### docker 默认创建的链（形态随版本略有差异）

| 表 | 链 | 作用 |
|---|---|---|
| nat | `DOCKER` / `DOCKER-USER` | `DOCKER` 做 **DNAT**（宿主机映射端口 → 容器 `IP:port`）；`DOCKER-USER` 是用户预留钩子，**总在最前面被调用**，自定义规则放这里 |
| filter | `DOCKER-ISOLATION-STAGE-1/2`、`FORWARD` | 隔离不同 bridge；放行容器流量 |
| nat | `POSTROUTING -s 172.17.0.0/16 ! -o docker0 -j MASQUERADE` | 容器出网 **SNAT** |

最后一条体现了 `MASQUERADE` 的本质：**按出接口地址动态 SNAT**。所以宿主机换 IP
（笔记本换 Wi-Fi）时容器出网不用改规则——这是它比写死 `SNAT --to-source` 更常用的原因。

#### k8s（kube-proxy iptables 模式）的典型链

| 链 | 作用 |
|---|---|
| `KUBE-SERVICES` | 总入口，挂在 `PREROUTING`/`OUTPUT`，按 `ClusterIP:port` 匹配 |
| `KUBE-SVC-XXXX` / `KUBE-SEP-XXXX` | Service 负载均衡层（`statistic` 按概率分流）→ Endpoint，最后一步 **DNAT** |
| `KUBE-MARK-MASQ` / `KUBE-MARK-DROP` | `-j MARK --set-xmark` 打标记，交给后续链决定 MASQUERADE / DROP |
| `KUBE-POSTROUTING` / `KUBE-FORWARD` / `KUBE-NODEPORTS` | 出口 MASQUERADE、转发链、NodePort 入口 |

**核心机制**：`KUBE-SVC-*` → `KUBE-SEP-*` 的概率分流**只对每个新建连接生效一次**，
之后由 conntrack 记住映射——这和 §1.5 的「NAT 只对第一个包做决策」是同一原理，
conntrack 表被当成"会话表"用。**这也解释了 iptables 模式为什么会被 IPVS/eBPF 取代**：
iptables 线性遍历，Service 数 N 增长时规则数近似 O(N)，且每次 kube-proxy 同步都是整表替换；
几千个 Service 时同步 CPU 与首次匹配延迟都会爆炸（IPVS 用哈希表，Cilium 直接用 eBPF map）。

#### 网桥上的 netfilter：`br_netfilter`

默认情况下**经过 bridge 的二层转发流量不走三层的 iptables 链**。要让 iptables 管到
桥接流量（docker/k8s 的常见需求），必须加载 `br_netfilter`
（源码 `net/bridge/br_netfilter_hooks.c`，注意**不在** `net/bridge/netfilter/` 下），
它通过 `br_nf_pre_routing(void *priv, ...)` 等 hook 把帧"喂"给三层。
加载后会在 `/proc/sys/net/bridge/` 注册这些开关（v6.12 `.procname` 实测，
**per-netns**，存在 `struct brnet` 里）：

| sysctl | 内核字段 | 作用 |
|---|---|---|
| `bridge-nf-call-iptables` | `call_iptables` | 桥接帧也过 IPv4 iptables |
| `bridge-nf-call-ip6tables` | `call_ip6tables` | 同上（IPv6） |
| `bridge-nf-call-arptables` | `call_arptables` | 同上（ARP） |
| `bridge-nf-filter-vlan-tagged` / `-pppoe-tagged` | `filter_vlan_tagged` / `filter_pppoe_tagged` | 是否处理带 VLAN tag / PPPoE 的帧 |
| `bridge-nf-pass-vlan-input-dev` | `pass_vlan_indev` | 把 VLAN 子接口作为 `-i` 传入 |

> **本机实测：`/proc/sys/net/bridge/` 不存在**，`/proc/modules` 里有 `bridge` 却**没有**
> `br_netfilter`。结论：**本机即使跑起容器，桥接流量也不会经过宿主机的 iptables 链**。
> 这就是 k8s 文档里那句"先 `modprobe br_netfilter` 并打开
> `net.bridge.bridge-nf-call-iptables`"的由来。另外 v6.12 里**没有** `br_netfilter_enable`
> 这个 sysctl（老文档写法已过时）。

#### Service 的 DNAT 与 conntrack 状态

kube-proxy 的 DNAT 规则**只对新建连接生效**，已建立连接全靠 conntrack 记住映射。
两个实用推论：

1. **后端 Pod 被删掉后，已建立的连接不会立刻断**（表项还在，直到超时）——
   这解释了"Pod 都删了怎么还有流量打到旧 IP"。
2. **短连接多时，`nf_conntrack_max` 才是节点真实瓶颈**：典型症状是 dmesg 出现
   `nf_conntrack: table full, dropping packet`，或 `nf_conntrack_count` 顶到 `nf_conntrack_max`。
   调法：调大 `nf_conntrack_max` 与 `nf_conntrack_buckets`（bucket 一般是 max 的 1/4~1/2，
   本机两者都是 262144），并把 `nf_conntrack_tcp_timeout_established` 从默认的
   432000 秒（5 天）调短——对高并发短连接场景 5 天明显过长。

---

## 4. 可观测点与验证方法（本机实测）

### 4.1 本机能读到的（真实输出）

```bash
# ① conntrack 当前条目数与上限（不需要 root）
cat /proc/sys/net/netfilter/nf_conntrack_count     # 283
cat /proc/sys/net/netfilter/nf_conntrack_max       # 262144
cat /proc/sys/net/netfilter/nf_conntrack_buckets   # 262144

# ② per-CPU conntrack 统计（不需要 root，非常有用）
head -3 /proc/net/stat/nf_conntrack
```

本机实测输出：

```
entries  clashres found new invalid ignore delete chainlength insert insert_failed drop early_drop icmp_error  expect_new expect_create expect_delete search_restart
0000011b  00000000 00000000 00000000 0000000a 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000  00000000 00000000 00000000 00000000
0000011b  00000000 00000000 00000000 00000008 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000  00000000 00000000 00000000 00000000
```

**逐列解读**（每行 = 一个 CPU，数值是十六进制）：

| 列 | 含义 | 什么时候该关心 |
|---|---|---|
| `entries` | 当前哈希表中的条目数（`0x11b` = 283，与 `nf_conntrack_count` 一致） | 持续增长接近 `nf_conntrack_max` |
| `clashres` | 哈希冲突重新插入次数 | 桶太少时增大 |
| `found` | 命中已有连接的次数 | 与 `new` 的比值可看出连接复用率 |
| `new` | 新建连接的次数 | |
| **`invalid`** | 被判为 INVALID 的包数 | 非 0 且持续增长 → 有畸形包/乱序/NAT 异常 |
| `ignore` | 被忽略（如 NOTRACK 或已 dying） | |
| `delete` | 删除条目数 | |
| `insert` | 插入哈希表次数 | |
| **`insert_failed`** | **插入失败 = 表满！** | 非 0 且增长 → 立刻查 `nf_conntrack_max` |
| **`drop`** | 因表满被丢弃 | 非 0 → 已经在丢包了 |
| `early_drop` | 提前回收旧条目以腾空间 | 持续增长说明容量吃紧 |
| `icmp_error` | 从 ICMP 差错反查连接 | |
| `expect_*` | 期望连接（helper 派生）相关 | FTP/SIP 等 helper 场景 |

> **实践价值**：`/proc/net/stat/nf_conntrack` 是**不需要 root** 就能看到的 conntrack 健康度指标。
> 生产上「丢包莫名其妙」时，先看这一行的 `insert_failed` / `drop` / `early_drop` 是否在涨。

```bash
# ③ 关键超时参数（本机实测值）
cat /proc/sys/net/netfilter/nf_conntrack_tcp_timeout_established   # 432000（5 天）
cat /proc/sys/net/netfilter/nf_conntrack_tcp_timeout_time_wait     # 120
cat /proc/sys/net/netfilter/nf_conntrack_udp_timeout               # 30
cat /proc/sys/net/netfilter/nf_conntrack_generic_timeout           # 600
```

### 4.2 本机**不能**读的（如实说明）

| 命令 | 本机结果 | 原因 | root 下该怎么做 |
|---|---|---|---|
| `cat /proc/net/nf_conntrack` | `权限不够` | 该文件仅 root 可读（内容含所有连接的五元组，属敏感信息） | `sudo conntrack -L` 或 `sudo cat /proc/net/nf_conntrack` |
| `iptables-save` / `nft list ruleset` | 命令未安装 | 本机未装 iptables/nftables | `sudo apt-get install iptables nftables` |
| `conntrack -L` | 命令未安装 | 未装 conntrack-tools | `sudo apt-get install conntrack` |
| `cat /proc/net/ip_tables_names` | `权限不够` | 同上，仅 root | `sudo iptables-save` |

> **注意**：`nf_conntrack_count/max/buckets` 与 `/proc/net/stat/nf_conntrack` **可读**，
> 而 `/proc/net/nf_conntrack`（每条连接的明细）**不可读**。
> 也就是说：非特权用户能看到「conntrack 表有多满、丢了多少」，但看不到「具体是哪些连接」。

### 4.3 观测命令模板（root 环境）

```bash
# 连接跟踪明细（含 NAT 映射；[UNREPLIED] 表示还没见过回包）
sudo conntrack -L
sudo conntrack -L -p tcp --state ESTABLISHED
sudo conntrack -E                     # 事件流：NEW/UPDATE/DESTROY 实时打印
sudo cat /proc/net/nf_conntrack       # 原始格式，字段顺序见下

# /proc/net/nf_conntrack 一行示例与字段含义：
# ipv4 2 tcp 6 431999 ESTABLISHED src=10.0.0.2 dst=10.0.0.5 sport=51234 dport=80
#      src=10.0.0.5 dst=1.2.3.4 sport=80 dport=51234 [ASSURED] mark=0 use=1
#  ↑    ↑ ↑   ↑   ↑      ↑           ↑
#  |    | |   |   剩余超时  状态
#  |    | |   L4 协议号
#  |    | L4 协议名
#  |    L3 协议号
#  netns/地址族
# 后面两组是「原始方向 / 回复方向」的元组；[ASSURED] 表示不会被提前回收

# 规则与表
sudo iptables-save -c                # 带计数器的规则（-c 看命中数，排查「规则没生效」神器）
sudo iptables -t nat -L -n -v
sudo nft list ruleset
sudo nft list ruleset -a             # 带 handle，便于删除单条

# 计数与丢包
sudo nft list ruleset | grep -A3 'hook '   # 看各 base chain 的 policy 与计数器
```

---

## 5. 用户态 API ↔ 内核实现对应表

| 用户态命令/概念 | 内核入口 | 源码文件 | 要点 |
|---|---|---|---|
| `iptables -A INPUT ...` | `do_ipt_set_ctl` → `translate_table` | `net/ipv4/netfilter/ip_tables.c` | 整表替换（`IPT_SO_SET_REPLACE`），带 `struct ipt_replace` |
| 规则匹配 | `ipt_do_table()` | 同上 | 线性遍历 + `xt_match` 回调 |
| `nft add rule ...` | `nf_tables_newrule` | `net/netfilter/nf_tables_api.c` | netlink 事务化 |
| 规则求值 | `nft_do_chain()` | `net/netfilter/nf_tables_core.c` | 表达式链 |
| （框架）hook 调用 | `nf_hook_slow()` | `net/netfilter/core.c` | 数组遍历 + verdict 处理 |
| `conntrack -L` | `ctnetlink_dump_table` | `net/netfilter/nf_conntrack_netlink.c` | NETLINK_NETFILTER 家族 |
| 连接跟踪 | `nf_conntrack_in()` | `net/netfilter/nf_conntrack_core.c` | 查表/建表 |
| 连接确认 | `__nf_conntrack_confirm()` | 同上 | POST_ROUTING 的 `INT_MAX` 优先级 |
| `-j SNAT/DNAT/MASQUERADE` | `nf_nat_inet_fn()` / `nf_nat_masquerade_ipv4()` | `net/netfilter/nf_nat_core.c`、`net/netfilter/nf_nat_masquerade.c` | 改写地址 + 序号修正（SNAT/DNAT/MASQUERADE 都走 `nf_nat_inet_fn()` 这一个 hook 回调） |
| xtables 的 nat hook 注册 | `nf_nat_ipv4_ops[]`（`priority = NF_IP_PRI_NAT_DST / NF_IP_PRI_NAT_SRC`） | `net/ipv4/netfilter/iptable_nat.c` | v6.12 里 `nf_nat_l3proto_ipv4.c`、`nf_nat_l4proto_register()` **都已删除**，统一走 `nf_nat_register_fn()` |
| nftables 的 nat hook 注册 | `nft_nat_...` / `nf_nat_register_fn()` | `net/netfilter/nft_chain_nat.c` | 与 iptables 的 nat 表竞争同一批 hook 点 |
| DNAT 后重查路由 | `ip_route_me_harder()` | `net/ipv4/netfilter.c` | 目的地址变了必须重查 |
| `-j NFQUEUE` | `nf_queue()` | `net/netfilter/nfnetlink_queue.c` | verdict 走 NF_QUEUE |
| `-j CT --notrack` | raw 表 hook（`NFPROTO_IPV4`，priority `NF_IP_PRI_RAW`=−300）；之后 `nf_conntrack_in()` 看到 `ctinfo == IP_CT_UNTRACKED` 直接 `return NF_ACCEPT` | `net/ipv4/netfilter/iptable_raw.c`、`net/netfilter/nf_conntrack_core.c` | 不建表项；之后 `--ctstate` 匹配不到、也不会被 NAT（**不是**设 `IPS_UNTRACKED`，那位已 obsolete） |
| 网桥上的 netfilter | `br_netfilter`（`br_netfilter_hooks.c`） | `net/bridge/br_netfilter_hooks.c` | 需 `bridge-nf-call-iptables=1`；本机 `/proc/sys/net/bridge/` 不存在（模块未加载） |

---

## 6. 常见陷阱、调优与自测题

### 6.1 生产环境高频问题

| 现象 | 根因 | 排查/处置 |
|---|---|---|
| 突然大量丢包，`insert_failed`/`drop` 增长 | **conntrack 表满** | `cat /proc/sys/net/netfilter/nf_conntrack_count` 对比 `max`；调大 `nf_conntrack_max` 与 `nf_conntrack_buckets`（并同步扩容内存） |
| `nf_conntrack: table full, dropping packet` 出现在 dmesg | 同上 | 或缩短超时（`nf_conntrack_tcp_timeout_established`），或对不需要跟踪的流量 `NOTRACK` |
| 容器里 `curl` 慢但宿主机正常 | 网桥 netfilter + conntrack 全量跟踪 | `nf_conntrack` 调优；必要时对 overlay 流量 `NOTRACK` |
| DNAT 后回包不回原路 | DNAT 改变了目的地址，需重查路由 | 确认 `ip_route_me_harder()` 能查到路由；检查 `rp_filter` 与 `ip_forward` |
| k8s Service 不通 | `KUBE-*` 链顺序 / conntrack 状态 | `iptables-save -c` 看计数器是否命中；看 `KUBE-SEP-*` 的 endpoint |
| NAT 后 TCP 吞吐异常/卡死 | 序号未修正 | 检查 `IPS_SEQ_ADJUST`（正常由内核处理；自研 NAT 必须处理） |
| 规则「明明写了却不生效」 | 链/hook/优先级不匹配 | 用 `-c` 看计数器是否为 0；确认 hook 点与方向；`-t nat` 别忘 |
| `-i/-o` 匹配不上 | PRE_ROUTING 无 `out`、LOCAL_OUT 无 `in` | 记住 1.1 节的两个「反直觉」点 |

### 6.2 安全相关

| 攻击/风险 | 内核缓解机制 | 相关参数 |
|---|---|---|
| SYN flood | SYN cookies、SYN 队列、synproxy | `tcp_syncookies`、`tcp_max_syn_backlog`、`nf_synproxy` |
| 伪造源地址反射/放大 | 反向路径过滤 | `rp_filter`（0/1/2，注意与策略路由的冲突） |
| 连接跟踪表耗尽（DoS） | 表上限 + `early_drop` | `nf_conntrack_max`、`nf_conntrack_tcp_loose` |
| 无状态绕过过滤 | conntrack `INVALID` 丢弃 | `-m conntrack --ctstate INVALID -j DROP` |
| 规则集膨胀 | nftables set/map | 用集合替代线性规则 |

> **不要照抄「调优模板」**：`nf_conntrack_max` 调多大取决于内存与实际连接数；
> `rp_filter=1` 在非对称路由（常见的多线/容器场景）下会**打断合法流量**。
> 每一项都必须先测量、再调整，见 [docs/09](09-内核网络观测与性能调优.md)。

### 6.3 自测题（≥8）

1. `filter` 表的 `INPUT` 链里看到的**目的地址**是 DNAT 之前还是之后？为什么？
2. 为什么 `iptables -t nat -A POSTROUTING` 里不能用 `-i eth0`？
3. conntrack 的 `IPS_SEQ_ADJUST` 是干什么的？不修正会怎样？
4. 为什么 `nf_conntrack_max` 本质上是「内存上限」而不是「条目上限」？
5. 一条 NAT 规则在「第一个包」和「后续包」上分别做了什么？为什么后续包更快？
6. `/proc/net/stat/nf_conntrack` 里 `insert_failed` 增长意味着什么？下一步查什么？
7. DNAT 之后内核为什么要调 `ip_route_me_harder()`？不调会怎样？
8. `iptables` 命令在现代发行版上可能其实是 nftables 后端，那么判断「规则是否真的生效」
   最可靠的手段是什么？
9. `raw` 表 + `-j CT --notrack` 为什么能提升性能？代价是什么？
10. 你自己写一个 netfilter 模块，`priority = NF_IP_PRI_FIRST + 1` 与 `priority = 0`
    分别会看到「有路由结果」还是「没有」？[labs/06](../labs/06-kernel-module-netfilter/) 的实测是哪种？

<details>
<summary>参考答案要点</summary>

1. **之后**。DNAT 在 `NF_IP_PRI_NAT_DST`(−100) 生效，filter 表在 0，所以 filter 看到的是改写后的目的地址。
2. POST_ROUTING 时出接口在 `state->out` 里通常已有，但**输入设备无意义**（包不是从网卡进来的，
   是本地生成或转发），另外在 `LOCAL_OUT` 阶段根本还没有出接口。规则语义上应避免依赖 `-i`。
3. NAT 改写地址后，TCP **序号/确认号也要相应平移**（因为改的是地址而不是连接本身，
   但序号在 NAT 设备视角下需要补偿，尤其配合 helper 与非对称路径）。
   不修正会导致对端认为数据错乱、连接卡死或校验失败。
4. 每个 `nf_conn` 要分配结构体 + 各扩展（timeout/nat/acct…），几百字节量级；
   `max` 乘以单条开销就是潜在内存占用。此外哈希桶本身也占内存（`nf_conntrack_buckets`）。
5. 第一个包：查不到条目 → 分配 `nf_conn` → 走 NAT 规则决定映射 → 写入 `nf_conn_nat`
   → `conntrack_confirm` 入库。后续包：直接哈希命中 → 按记录改写。
   因为**规则匹配只发生一次**，之后是 O(1) 查表。
6. 说明「要插入连接跟踪表但失败了」，绝大多数情况就是**表满**。
   下一步：对比 `nf_conntrack_count` 与 `nf_conntrack_max`，看 `drop`/`early_drop` 是否同步增长，
   再决定调大上限/桶数、缩短超时，或对部分流量 NOTRACK。
7. 因为 DNAT 改了目的地址，**路由结论可能完全改变**（出接口、下一跳、是否算本机）。
   不重查会用旧路由发包，表现为「DNAT 后不回包」或「回包走错接口」。
8. 最可靠的是看**计数器**：`iptables-save -c`（或 `nft list ruleset` 里的 counters）
   观察规则命中数是否增加。规则存在 ≠ 规则被命中（hook 点、方向、优先级都可能不对）。
9. 避免为这些连接创建/维护 `nf_conn`（省内存、省查表、省 confirm），适合大流量且不需要
   状态化处理的流量（如某些镜像/存储流量）。代价是**失去状态化过滤与 NAT 能力**，
   这类流量无法用 `--ctstate` 匹配，也不能被 NAT。
10. `NF_IP_PRI_FIRST + 1` 会在**几乎所有其他钩子之前**执行，
   所以此时很可能**还没有路由结果**（`skb_dst()` 为空、`state->out` 为 NULL）；
   应只依赖 skb 本身与 `state->in`。若要读路由结果，应选 filter(0) 之后的优先级，
   或在钩子里显式判断 `skb_dst(skb)` 是否为空再使用。
</details>

---

## 7. 本章小结

- netfilter 是**横切框架**：五个 hook 点 + 优先级排序 + verdict，数据路径上是一次数组遍历。
- 牢记优先级链条：**conntrack(−200) → DNAT(−100) → filter(0) → SNAT(100) → confirm(INT_MAX)**，
  它决定了「你在哪一步能改什么、能看到什么」。
- conntrack 是 `nf_conn` 的哈希表，双向元组；NAT 是 conntrack 的**副产品**，
  规则只在第一个包上生效，之后靠查表改写。
- 可观测面：`/proc/sys/net/netfilter/*` 与 `/proc/net/stat/nf_conntrack`（**非特权可读**）
  给出容量与健康度；`/proc/net/nf_conntrack` 与 `conntrack -L`（需 root）给出明细。
- 生产事故的高频根因：**conntrack 表满**、**DNAT 后没重查路由**、**规则命中了错误的 hook 点**。

## 8. 延伸阅读

- 内核文档：`Documentation/networking/netfilter-sysctl.rst`（在 elixir 上看）
- 源码入口（6.12，建议交叉引用确认）：`net/netfilter/core.c`、`nf_conntrack_core.c`、
  `nf_nat_core.c`、`net/ipv4/netfilter/ip_tables.c`、`net/ipv4/netfilter.c`（`ip_route_me_harder`）
- nftables 官方 wiki（语法与架构）；`man iptables`、`man nft`、`man conntrack`
- 配套实验：[labs/06-kernel-module-netfilter](../labs/06-kernel-module-netfilter/)（真编译内核模块 + netfilter hook）
- 相关章节：[docs/04 网络层](04-网络层-IP路由与邻居子系统.md)（hook 插入位置）、
  [docs/09 观测与调优](09-内核网络观测与性能调优.md)（表满与丢包定位）、
  [docs/07 eBPF/XDP](07-eBPF-XDP与可编程数据面.md)（另一种可编程数据面）

> **版本提示**：本章的宏与枚举值均取自本机同版本 UAPI 头文件
> （`/usr/src/linux-headers-6.12.107+deb13-common/include/uapi/linux/netfilter*.h`），
> 因此是**可复核**的。函数名与文件路径请以 `elixir.bootlin.com/linux/v6.12/source/` 为准。
