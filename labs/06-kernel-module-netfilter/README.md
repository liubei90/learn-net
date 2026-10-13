# Lab 06：亲手写一个 netfilter 观测内核模块（netlab.ko）

> 这是本教程「内核子模块实操」的第一站。
> 目标不是"写个防火墙"，而是**真正编译出一个能 insmod 的外部内核模块**，
> 并在真实的 netfilter hook 点上观察数据包，理解 `nf_hook_ops` 注册、
> 优先级排布、skb 解析、per-CPU 统计这些内核网络子系统的第一手细节。

---

## 0. 实验目的

1. **打通"用户态 → 内核模块"这条最硬的路**：理解为什么外部模块必须用
   Kbuild（`make -C $(KDIR) M=$(PWD) modules`）编译，而不是直接 `gcc`。
2. **在真实的 hook 点上注册回调**：用 `nf_register_net_hooks()` 把
   `nf_hook_ops` 挂到 `NFPROTO_IPV4` 的 `NF_INET_LOCAL_IN` 与
   `NF_INET_LOCAL_OUT`，并理解 `priority` 的相对位置意味着什么。
3. **学会在数据面上安全地读 skb**：`skb_header_pointer()` 处理非线性 skb、
   `ip_is_fragment()` 之后再碰 L4 头、VLAN 下用 `skb_network_offset()`
   而不是硬编码偏移。
4. **学会在数据面上做统计**：per-CPU 计数（`this_cpu_inc`）为什么比全局
   `atomic64_t` 快，什么情况下它需要额外保护。
5. **把内核态数据导出给用户态**：`seq_file` + `proc_ops`，
   支持 `echo reset > /proc/netlab_stats` 这类控制接口。
6. **建立"内核模块 = 可能把机器搞挂"的安全意识**：版本/vermagic、
   Secure Boot 签名、以及一整套"never break the box"的操作纪律。

**本模块的自我约束（底线）**：hook 回调**永远返回 `NF_ACCEPT`**，
不修改 skb、不修改 mark、不 DROP、不 STOLEN、不 QUEUE。
它是一个纯粹的旁观者（observer），不是防火墙。

---

## 1. 涉及的内核机制与源码入口

| 机制 | 关键符号 | 源码位置（Linux v6.12） |
| --- | --- | --- |
| hook 链注册/注销 | `nf_register_net_hook()` / `nf_unregister_net_hook()` / `nf_register_net_hooks()` | `net/netfilter/core.c` |
| hook 链遍历与判决 | `nf_hook_slow()`、`nf_hook_entries` | `net/netfilter/core.c` |
| 协议栈里的调用点宏 | `NF_HOOK()` / `NF_HOOK_COND()` / `nf_hook()` | `include/linux/netfilter.h` |
| hook 描述与状态 | `struct nf_hook_ops`、`struct nf_hook_state`、`nf_hookfn` | `include/linux/netfilter.h` |
| verdict 与 hook 点枚举 | `NF_ACCEPT`、`NF_INET_LOCAL_IN`、`NFPROTO_IPV4` | `include/uapi/linux/netfilter.h` |
| hook 优先级常量 | `NF_IP_PRI_FIRST`、`NF_IP_PRI_CONNTRACK`、`NF_IP_PRI_NAT_DST`、`NF_IP_PRI_NAT_SRC`、`NF_IP_PRI_FILTER`、`NF_IP_PRI_CONNTRACK_CONFIRM` | `include/uapi/linux/netfilter_ipv4.h` |
| IPv4 入向调用点 | `ip_rcv()` / `ip_local_deliver()` | `net/ipv4/ip_input.c` |
| IPv4 出向调用点 | `__ip_local_out()` / `ip_output()` | `net/ipv4/ip_output.c` |
| IPv4 转发调用点 | `ip_forward()` | `net/ipv4/ip_forward.c` |
| skb 安全读取 | `skb_header_pointer()`、`skb_network_offset()` | `include/linux/skbuff.h` |
| 分片判定 | `ip_is_fragment()` | `include/net/ip.h` |
| per-CPU 计数 | `this_cpu_ptr()`、`this_cpu_inc()` | `include/linux/percpu.h` |
| procfs + seq_file | `proc_create()`、`single_open()`、`seq_printf()` | `include/linux/proc_fs.h`、`include/linux/seq_file.h` |
| debugfs | `debugfs_create_dir()`、`debugfs_create_file()` | `fs/debugfs/`、`include/linux/debugfs.h` |

> 源码路径以 Linux v6.12 为准，可在
> <https://elixir.bootlin.com/linux/v6.12/source/> 核对。
> 本文档**不给出具体行号**（行号随小版本变动，且容易写错）。

### 1.1 两个 hook 点在协议栈里的位置

```
              应用层 send()/write()
                     │
                     ▼
        ┌────────────────────────────┐
        │  ip_local_out()            │  net/ipv4/ip_output.c
        │  └─ NF_HOOK(LOCAL_OUT)  ←──┼── ★ 本模块挂在这里（priority = INT_MIN+1）
        │       └─ dst_output()      │
        └────────────┬───────────────┘
                     ▼
              ip_output() ── NF_HOOK(POST_ROUTING) ── ip_finish_output()
                     │
                     ▼
               邻居子系统 / 驱动 / 网线

   ─────────────────── 网络 ───────────────────

                驱动 RX → ip_rcv() ── NF_HOOK(PRE_ROUTING) ── ip_rcv_finish()
                                                                │
                                            路由判决：本机？转发？
                                                    │              │
                                      本机 ┌────────┘              └──────┐ 转发
                                           ▼                              ▼
                          ip_local_deliver()                    ip_forward()
                          └─ NF_HOOK(LOCAL_IN)  ←── ★ 本模块挂在这里
                                 │
                                 ▼
                          ip_local_deliver_finish() → TCP/UDP → socket
```

要点：**`LOCAL_IN` 只处理"目的地是本机"的包**，`LOCAL_OUT` 只处理
"本机产生"的包。转发流量走的是 `FORWARD`，本模块看不到（这是刻意的选择：
让读者自己动手加第三个 hook 是练习之一）。

---

## 2. 代码导读（`netlab.c`）

| 段落 | 内容 | 关键点 |
| --- | --- | --- |
| §1 | `module_param` | `sample_limit`（环形缓冲条数）、`enabled`（总开关），运行时可在 `/sys/module/netlab/parameters/` 读写 |
| §2 | `DEFINE_PER_CPU(struct netlab_cpu_stats, netlab_pcpu)` | 每个 CPU 一份：总包数/字节数、按 hook、按协议号、按设备名 |
| §3 | `netlab_ring[]` + `spinlock` | 跨 CPU 共享的"最近 N 条"环形缓冲 |
| §4 | `netlab_hook()` | **数据面**：快路径 per-CPU 计数 + 慢路径写环形缓冲，最后 `return NF_ACCEPT` |
| §5 | `netlab_aggregate()` / `netlab_reset()` | 求和与清零 |
| §6 | `/proc/netlab_stats` | `seq_file` 输出 + `proc_write` 支持 `reset`/`enable`/`disable` |
| §7 | `/sys/kernel/debug/netlab/netlab_stats` | 可选加分项，复用同一个 show 函数 |
| §8 | `nf_register_net_hooks()` / `proc_create()` / `proc_remove()` / `nf_unregister_net_hooks()` | 初始化与退出的**镜像对称**清理 |

### 2.1 最容易踩坑的三处代码

**（a）分片包必须先判 `ip_is_fragment()` 再取 L4 头**

```c
frag = ip_is_fragment(iph);          /* frag_off & htons(IP_MF | IP_OFFSET) */
if (frag) {
        sport = 0; dport = 0;        /* 分片包根本没有可解析的 L4 头 */
} else {
        netlab_get_ports(skb, iph, &sport, &dport);
}
```

为什么？一个 1500 字节 MTU 上的 4000 字节 UDP 报文会被切成 3 片，
只有**第一个分片**（offset == 0 且 MF == 1）里才有 UDP 头，
后续分片的 `iph->ihl` 之后直接就是负载的中间部分。如果拿后续分片
当普通包去读 `struct udphdr`，你会：
- 统计出完全错误的"端口号"；
- 在 paged data 上越界读，运气不好直接内核 oops。

`netlab_get_ports()` 里还有第二道防线：
`iph->ihl < 5` 直接返回（畸形头）、`ihl + sizeof(struct udphdr) > tot_len`
直接返回（头声明长度与实际不符）。

**（b）用 `skb_header_pointer()` 而不是 `ip_hdr(skb)`**

```c
return skb_header_pointer(skb, skb_network_offset(skb), sizeof(*buf), buf);
```

`ip_hdr(skb)` 只返回 `skb->data + skb_network_offset(skb)` 这个指针，
它**假设 IP 头在线性区**。当 skb 是非线性（数据在 fraglist / page 里）时，
这个指针指向的内容根本不是 IP 头。`skb_header_pointer()` 会：
线性区 → 直接返回指针；非线性区 → 把需要的字节拷进调用者给的栈缓冲再返回；
数据不够 → 返回 NULL。这是所有"只想读、不想改 skb"的代码的正确姿势。

另外用 `skb_network_offset(skb)` 而不是写死 `0`：VLAN/QinQ 封装过的包，
网络头偏移是 14/18 字节。

**（c）per-CPU 计数为什么不用锁**

```c
st = this_cpu_ptr(&netlab_pcpu);
st->pkts++;
st->bytes += len;
```

- 全局 `atomic64_inc` 会让**所有 CPU 争抢同一条 cache line**。
  在 10Gbps 转发场景下，cache-line bouncing 本身就是可观的性能损失。
- per-CPU 变量让每个 CPU 只写"自己的"那条 cache line，写路径零竞争；
  只有读取（`cat /proc/netlab_stats`）时才遍历求和，代价从热路径转移到了冷路径。
- x86-64 上 `this_cpu_inc()` 编译成单条 `incq %gs:<offset>` 指令，
  同 CPU 上的抢占/中断不会把它拆开，因此**不需要** `preempt_disable()`。
  但要注意：并非所有 per-CPU RMW 都是单指令（如 `this_cpu_add_return()`），
  那些需要配合 `preempt_disable()`。

环形缓冲是跨 CPU 共享的，所以必须加锁；而且要用 `spin_lock_irqsave()`
而不是普通 `spin_lock()`：`LOCAL_OUT` 的 hook 可能在进程上下文被软中断打断，
`LOCAL_IN` 的 hook 本身就在软中断上下文，两者争同一把锁时会死锁。

---

## 3. 编译（本机实测，uid=1000，不需要 root）

### 3.1 环境事实（`make check-env` 的真实输出）

```console
$ make check-env
uname -r      : 6.12.107+deb13-amd64
KDIR          : /lib/modules/6.12.107+deb13-amd64/build  [exists]
uid           : 1000
CapEff        : 0000000000000000
gcc           : gcc (Debian 14.2.0-19) 14.2.0
make          : GNU Make 4.4.1
modinfo       : /sbin/modinfo
insmod        : /sbin/insmod
unprivileged_bpf_disabled : 2

结论: 可以编译 (make all)，但不能 insmod/rmmod。
      注意：**这不只是权限问题** —— 本机 UEFI Secure Boot 生效、运行时
      `sig_enforce=Y`，未签名模块连 root 都加载不了（`Key was rejected by service`），
      详见 8.4 / 8.5。
```

> 注意 `PATH` 里没有 `/sbin`，所以 `modinfo` / `insmod` / `rmmod`
> 直接敲会 "command not found"。Makefile 里做了 fallback
> （`command -v modinfo || echo /sbin/modinfo`），这样 `make info` 能用。

### 3.2 `make clean all` 真实输出

```console
$ cd /home/user/code/learn-net
$ make -C labs/06-kernel-module-netfilter clean all
make: 进入目录“/home/user/code/learn-net/labs/06-kernel-module-netfilter”
make -C /lib/modules/6.12.107+deb13-amd64/build M=/home/user/code/learn-net/labs/06-kernel-module-netfilter clean
make[1]: 进入目录“/usr/src/linux-headers-6.12.107+deb13-amd64”
  CLEAN   /home/user/code/learn-net/labs/06-kernel-module-netfilter/Module.symvers
make[1]: 离开目录“/usr/src/linux-headers-6.12.107+deb13-amd64”
==> 已清理（.ko 已删除）
make -C /lib/modules/6.12.107+deb13-amd64/build M=/home/user/code/learn-net/labs/06-kernel-module-netfilter modules
make[1]: 进入目录“/usr/src/linux-headers-6.12.107+deb13-amd64”
  CC [M]  /home/user/code/learn-net/labs/06-kernel-module-netfilter/netlab.o
  MODPOST /home/user/code/learn-net/labs/06-kernel-module-netfilter/Module.symvers
  CC [M]  /home/user/code/learn-net/labs/06-kernel-module-netfilter/netlab.mod.o
  CC [M]  /home/user/code/learn-net/labs/06-kernel-module-netfilter/.module-common.o
  LD [M]  /home/user/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko
  BTF [M] /home/user/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko
make[1]: 离开目录“/usr/src/linux-headers-6.12.107+deb13-amd64”

==> 产物:
-rw-rw-r-- 1 liubei liubei 674560 10月12日 22:53 /home/user/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko
make: 离开目录“/home/user/code/learn-net/labs/06-kernel-module-netfilter”
$ echo $?
0
```

**结论：`netlab.ko` 真实生成（674560 字节），零 warning、零 error。**

用 `W=1`（打开内核额外告警等级）复核，同样没有任何告警：

```console
$ make -C /lib/modules/$(uname -r)/build M=$(pwd) W=1 modules
make: 进入目录“/usr/src/linux-headers-6.12.107+deb13-amd64”
make: 离开目录“/usr/src/linux-headers-6.12.107+deb13-amd64”
```

（只有这两行目录提示，中间没有任何 `warning:` / `error:`。）

### 3.3 编译产物验证

```console
$ ls -l netlab.ko
-rw-rw-r-- 1 liubei liubei 674560 10月12日 22:53 netlab.ko
```

模块链接了哪些内核符号（`nm netlab.ko | grep ' U '`，真实输出，节选）：

```text
__check_object_size   const_pcpu_hot        _copy_from_user      __cpu_possible_mask
debugfs_create_dir    debugfs_create_file   debugfs_remove       __fentry__
init_net              kfree                 __kmalloc_cache_noprof
ktime_get_with_offset memset                nf_register_net_hooks  nf_unregister_net_hooks
param_ops_bool        param_ops_uint        __per_cpu_offset
proc_create           proc_remove           _raw_spin_lock_irqsave
_raw_spin_unlock_irqrestore                 seq_printf           seq_read
single_open           single_release        sized_strscpy        skb_copy_bits
strim                 sysfs_streq           this_cpu_off
```

这几行是"真的挂上了 netfilter"的直接证据：`nf_register_net_hooks` /
`nf_unregister_net_hooks` 来自 `net/netfilter/core.c`；
`skb_copy_bits` 是 `skb_header_pointer()` 在非线性分支里调用的函数；
`mk` 出来的 `this_cpu_off` / `const_pcpu_hot` 是 per-CPU 访问的编译器展开；
`param_ops_*` 是 `module_param` 生成的。

---

## 4. `modinfo netlab.ko`（真实输出）

```console
$ /sbin/modinfo netlab.ko
filename:       /home/user/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko
version:        1.0
description:    Observational netfilter hooks on IPv4 LOCAL_IN/LOCAL_OUT (stats only, never drops)
author:         learn-net labs
license:        GPL
srcversion:     E8C4D5EF6CED796B0E2F327
depends:
name:           netlab
retpoline:      Y
vermagic:       6.12.107+deb13-amd64 SMP preempt mod_unload modversions
parm:           sample_limit:number of most-recent packet records kept in the ring buffer (1..256) (uint)
parm:           enabled:master switch: when 0 the hooks return NF_ACCEPT immediately (bool)
```

`.modinfo` 段的原始内容（`readelf -p .modinfo netlab.ko`）：

```text
  [     0]  version=1.0
  [     c]  description=Observational netfilter hooks on IPv4 LOCAL_IN/LOCAL_OUT (stats only, never drops)
  [    6b]  author=learn-net labs
  [    81]  license=GPL
  [    8d]  parm=enabled:master switch: when 0 the hooks return NF_ACCEPT immediately
  [    d7]  parmtype=enabled:bool
  [    ed]  parm=sample_limit:number of most-recent packet records kept in the ring buffer (1..256)
  [   145]  parmtype=sample_limit:uint
  [   160]  srcversion=E8C4D5EF6CED796B0E2F327
  [   183]  depends=
  [   18c]  name=netlab
  [   198]  retpoline=Y
  [   1a4]  vermagic=6.12.107+deb13-amd64 SMP preempt mod_unload modversions
```

**为什么 `depends:` 是空的？**
因为本模块用到的所有外部符号（`nf_register_net_hooks`、`seq_printf`、
`proc_create`、`debugfs_*`、`skb_header_pointer` 展开出的 `skb_copy_bits` …）
都在 **vmlinux（内核本体）** 里，不在其它可加载模块里。
如果依赖 `nf_conntrack` 这类独立模块，`depends` 里就会出现它，
`modprobe` 会自动先把依赖装上。

**vermagic 是最关键的一行**：

```console
$ uname -r
6.12.107+deb13-amd64
$ /sbin/modinfo -F vermagic netlab.ko
6.12.107+deb13-amd64 SMP preempt mod_unload modversions
```

vermagic 里的 `6.12.107+deb13-amd64` 与 `uname -r` **完全一致**，
加上 `SMP preempt mod_unload modversions` 与运行内核的编译配置一致，
说明内核在 `insmod` 时的版本校验会通过。
（如果内核编译时开了 `CONFIG_MODVERSIONS`，`modversions` 会参与符号 CRC 校验；
符号版本不匹配时 `insmod` 会报 `disagrees about version of symbol ...`。）

---

## 5. 真正加载的完整步骤（需 root，且机器允许未签名模块）

> **本机限制（实测）**：`uid=1000`、`CapEff=0000000000000000`（无任何 capability）、
> `no_new_privs` 已设置，因此 `sudo` 也无法提权：
>
> ```console
> $ sudo -n true
> sudo: 已设置“no new privs”标志，它阻止 sudo 以 root 身份运行。
> ```
>
> 所以 `insmod` / `rmmod` 在本机跑不了。
>
> **但拿到 root 也不够（这是 2026-10-13 的新实测结论）**：用特权容器
> （真 root、`CapEff=000001ffffffffff`）加载仍然失败，报
> `insmod: ERROR: could not insert module netlab.ko: Key was rejected by service`
> —— 本机 Secure Boot 生效、运行时 `sig_enforce=Y`，**未签名模块一律被拒**。
> 详见 8.4（证据）与 8.5（用 MOK 签名的正规解法）。
>
> 下面给出在**有 root、且允许未签名模块（或已按 8.5 签名）的机器**上的完整步骤。
> 其中标注 **[预期]** 的输出是按代码逻辑推导的形态，不是本机实测。

### 5.1 加载

```console
$ sudo insmod netlab.ko sample_limit=32
$ dmesg | tail -n 3
[预期]
[ 1234.567890] netlab: loaded, 2 hooks on NFPROTO_IPV4 (prio=-2147483647), stats at /proc/netlab_stats
```

`insmod netlab.ko sample_limit=32` 这种写法能生效，是因为
`module_param(sample_limit, uint, 0644)` 注册了同名参数；
等价的运行时接口是 `/sys/module/netlab/parameters/sample_limit`。

### 5.2 产生流量，然后读统计

```console
$ curl -s -o /dev/null http://127.0.0.1/ ; ping -c 1 127.0.0.1
$ cat /proc/netlab_stats
[预期，格式与字段完全对应代码里的 seq_printf]
netlab: enabled=1 sample_limit=32 ring_max=256
totals: pkts=57 bytes=8432
by_hook: PRE_ROUTING=0 LOCAL_IN=25 FORWARD=0 LOCAL_OUT=32 POST_ROUTING=0
by_proto: ICMP(1)=2 TCP(6)=37 UDP(17)=18
by_indev: lo=25/5120B - =32/3312B
by_outdev: - =25/5120B lo=32/3312B
ring: len=32 head=57 total_recorded=57
recent (oldest first):
  #25 t=1759452812.334567890 hook=LOCAL_OUT    len=84 proto=TCP 127.0.0.1:41234 -> 127.0.0.1:80 in=- out=lo
  #26 t=1759452812.334612345 hook=LOCAL_IN     len=60 proto=TCP 127.0.0.1:80 -> 127.0.0.1:41234 in=lo out=-
  ...
hint: echo reset > /proc/netlab_stats   # clear all counters
```

读这个输出时要注意几个**观测点语义**：

- `LOCAL_IN` 的 `by_indev` 有值、`by_outdev` 是 `-`；
  `LOCAL_OUT` 恰好相反。这正是 `struct nf_hook_state` 里
  `in` / `out` 两个 `struct net_device *` 在不同 hook 点的填充规则。
- 因为模块的 `priority = NF_IP_PRI_FIRST + 1` 排在最前面，
  这里看到的是**未经 NAT 修改**的地址/端口。想看到 DNAT 之后的地址，
  得把 priority 调到 `NF_IP_PRI_NAT_DST`（-100）**之后**，比如 `+1`。
- loopback 流量也会经过 `LOCAL_IN`/`LOCAL_OUT`，所以访问 `127.0.0.1`
  就能自己造数据，不需要外部网络。

### 5.3 运行时调参与清零

```console
$ echo 4 | sudo tee /sys/module/netlab/parameters/sample_limit
$ echo 0 | sudo tee /sys/module/netlab/parameters/enabled     # 关闭统计
$ echo reset | sudo tee /proc/netlab_stats
$ sudo dmesg | tail -n 2
[预期]
[ 1300.111111] netlab: counters reset by pid 4711
```

注意 `enabled=0` 关闭后 `by_hook` 计数不再增长，但**hook 本身仍被调用**
（只是立刻 return）—— 想彻底消除开销只能 `rmmod`。
要衡量"统计到底花了多少 CPU"，正确的做法是开关 `enabled` 前后
用 `iperf3` / `netperf` 或 `perf top` 对比。

### 5.4 卸载

```console
$ sudo rmmod netlab
$ dmesg | tail -n 1
[预期]
[ 1400.222222] netlab: unloaded
$ ls /proc/netlab_stats
ls: cannot access '/proc/netlab_stats': No such file or directory
```

### 5.5 非 root 时 Makefile 的行为（本机实测）

```console
$ make load
SKIP: 当前 uid=1000，不是 root。
      加载内核模块需要 CAP_SYS_MODULE，普通用户无法完成。
      请在 root shell（或 sudo）里执行：
        sudo insmod /home/user/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko
      编译本身不需要 root，make all 已经成功。
$ echo $?
0

$ make unload
SKIP: 当前 uid=1000，不是 root，无法 rmmod。
      请在 root shell 里执行： sudo rmmod netlab
$ echo $?
0

$ make log
NOTE: dmesg 被 kernel.dmesg_restrict=1 限制。
      请用 root 执行: sudo dmesg | tail -n 30
      或:            sudo journalctl -k | tail -n 30
$ echo $?
0
```

`make load` / `make unload` / `make log` 在非 root 下**退出码都是 0**，
只打印说明 —— 这样 CI 里可以无条件跑 `make all load unload` 而不会红。

---

## 6. Makefile 逐条说明

```make
obj-m += netlab.o                                             # 声明要构建的外部模块
KDIR  ?= /lib/modules/$(shell uname -r)/build                  # 内核构建目录（指向 /usr/src/linux-headers-*）
MODDIR := $(CURDIR)                                            # 本目录（绝对路径）

all:
	$(MAKE) -C $(KDIR) M=$(MODDIR) modules                 # 借内核 Kbuild 编译
clean:
	$(MAKE) -C $(KDIR) M=$(MODDIR) clean
load / unload / reload / log / info / check-env / help
```

- `M=$(MODDIR)` 必须是**绝对路径**，Kbuild 用它定位外部模块的源码与产物。
- 第一次构建会额外生成 `Module.symvers`、`modules.order`、`*.mod.c`、
  `.*.cmd`，这些是 MODPOST 的产物，`make clean` 会清掉。
- `make -C $(KDIR) M=$(MODDIR) clean` 在**内核头文件目录不存在**时会报错，
  所以 `all` 里先做了一次 `[ -d "$(KDIR)" ]` 检查并给出安装头文件的提示。

### 本机真实存在的内核构建目录布局

```console
$ ls -d /lib/modules/$(uname -r)/build
/lib/modules/6.12.107+deb13-amd64/build        # -> /usr/src/linux-headers-6.12.107+deb13-amd64
$ ls /usr/src/
linux-headers-6.12.107+deb13-amd64     # 架构相关：arch/ include/generated/ scripts/ Makefile Module.symvers
linux-headers-6.12.107+deb13-common    # 通用头文件：include/linux/... include/net/...
linux-kbuild-6.12.107+deb13            # Kbuild 脚本
```

注意 **头文件被拆成两个包**：`-amd64` 是 `linux-headers-*`（含 Makefile、
`Module.symvers`、`include/generated/`），`-common` 才是真正的
`include/linux/netfilter.h`、`include/net/netfilter/*.h`。
Debian 上找不到内核头文件的经典原因就是少装了 `-common` 或 `linux-kbuild-*`。

---

## 7. 观察题与自测题

**Q1. 为什么 netfilter hook 回调里绝对不能睡眠？**
A：hook 回调的调用路径上可能持有 `rcu_read_lock()`（`nf_hook()` 里就是
`rcu_read_lock()` → `nf_hook_slow()`），并且**入向 hook 运行在软中断上下文**
（`NET_RX_SOFTIRQ` 里跑 `net_rx_action()` → `ip_rcv()` → `NF_HOOK`）。
在这个上下文里调用 `mutex_lock()`、`kmalloc(GFP_KERNEL)`、`copy_to_user()`、
`msleep()` 会触发 `BUG: scheduling while atomic` 甚至死锁 ——
RCU 读侧临界区里睡眠更是直接违规。
所以本模块里只用 `GFP_ATOMIC` 级别的操作、栈变量、`spin_lock_irqsave()`；
真正的内存分配（`kzalloc(sizeof(*tot), GFP_KERNEL)`）放在 **procfs 的 show 回调**里，
那是进程上下文、可以睡眠的地方。
需要异步或者可能失败的"慢活"，正确做法是丢进 `nf_queue`、
或者用 workqueue / 软中断后处理。

**Q2. 为什么统计要用 per-CPU 而不是一个全局 `atomic64_t`？**
A：见 §2.1(c)。核心是**写竞争**：全局计数器的 cache line 会在所有 CPU 之间
来回迁移（cache-line bouncing），这本身就是可观的开销，而且随核数增加而恶化。
per-CPU 把"一次全局原子操作"变成"一次本地内存写 + 读时聚合"，
把代价从每个包的热路径挪到了每次 `cat` 的冷路径。
Linux 内核自己的 `/proc/net/softnet_stat`、网络设备统计同样是 per-CPU 的。
代价是：(1) 读到的值是"略过期"的快照；(2) 内存占用 = 结构大小 × CPU 数
（本模块 `netlab_cpu_stats` 约 2.6KB × CPU 数）。

**Q3. `this_cpu_inc()` 到底要不要 `preempt_disable()`？**
A：取决于它展开成几条指令。
x86-64 上 `this_cpu_inc()` 是单条 `incq %gs:<offset>`，指令本身不可分割，
同 CPU 上的抢占/中断不会破坏它；而且"每个 CPU 只写自己的副本"，
不存在跨 CPU 竞争，所以**不需要** `preempt_disable()`。
但 `this_cpu_add_return()` / `this_cpu_cmpxchg()` 这类需要"读-改-写"多步的
操作，在可抢占上下文里就必须配 `preempt_disable()`（或 `local_irq_save()`），
否则任务可能被迁移到另一个 CPU 上继续执行，读到别人 CPU 的值。
本模块只用单指令的 `++` / `+=`，因此是安全的。

**Q4. skb 在 `NF_INET_PRE_ROUTING` 里有没有路由结果（`skb_dst()`）？**
A：**没有**（准确说：还没有由本机路由决策产生的 dst）。v6.12 的入向路径是
`ip_rcv()` 里先跑 `NF_INET_PRE_ROUTING`，成功后再由 `ip_rcv_finish()` →
`ip_rcv_finish_core()` 调用 `ip_route_input_noref()` 做输入路由，
把 `dst_entry` 挂到 skb 上（`skb_dst_set()`）。
所以
- 在 `PRE_ROUTING` 的 hook 里 `skb_dst(skb)` 通常为 NULL，
  `skb->_skb_refdst` 还没有有效路由；
- 在 `LOCAL_IN` / `FORWARD` 的 hook 里，路由结果已经有了，
  `ip_route_input_noref()` 的结论（本机 / 转发 / 丢弃）已经体现为 skb 的 dst。
- 出向方向相反：`LOCAL_OUT` 时**已经有**路由（`ip_local_out()` 之前
  `__ip_route_output_key()` 已经查过），`POST_ROUTING` 时路由和邻居输出
  基本都已就绪。
这也是 DNAT 之后必须"重查路由"（`ip_route_me_harder()`）的根本原因：
目的地址被改了，原来基于旧目的地址算出的 dst 不再有效。

**Q5. 本模块挂在 `priority = NF_IP_PRI_FIRST + 1`，能看到 DNAT/SNAT 之后的地址吗？**
A：不能。`NF_IP_PRI_FIRST` 是 `INT_MIN`，`+1` 之后仍然排在所有 hook 之前
（conntrack 在 -200、NAT_DST 在 -100、filter 在 0、NAT_SRC 在 100）。
在同一 hook 点上，优先级数值**从小到大**依次执行，所以本模块先跑，
看到的是原始地址。想观察 NAT 结果，把 `priority` 改成 `NF_IP_PRI_NAT_SRC + 1`
（即 101）就能在 SNAT 之后看到改写后的源地址/端口。
这也是排查 NAT 问题时"到底看哪一层"的关键。

**Q6. `nf_register_net_hooks()` 与 `nf_register_net_hook()` 有什么区别？为什么用前者？**
A：`nf_register_net_hook(struct net *net, const struct nf_hook_ops *ops)`
注册**一个** hook；`nf_register_net_hooks(struct net *net, const struct nf_hook_ops *reg, unsigned int n)`
批量注册 n 个，内部逐个插入 `net->nf.hooks_ipv4[hooknum]` 等数组。
批量版本的意义是**减少失败时的回滚复杂度**：如果第 2 个失败，
它会把第 1 个也摘掉再返回错误码，调用者不用自己写回滚逻辑。
本模块挂 2 个 hook，正好用批量版本；同时如果注册失败就直接
`return ret`（绝不能"部分注册成功还返回 0"）。

**Q7. 为什么本模块在容器（docker）里看不到流量？怎么修？**
A：netfilter 的 hook 数组是**per-netns** 的（`struct net` 里的
`nf.hooks_ipv4[]`），`nf_register_net_hooks(&init_net, ...)` 只改了
初始网络命名空间。容器在自己的 netns 里，注册表是另一份，
hook 链里根本没有我们的 ops。
要覆盖所有 netns，必须实现 `struct pernet_operations`：
```c
static struct pernet_operations netlab_net_ops = {
        .init = netlab_net_init,   /* 在里面 nf_register_net_hooks(net, ...) */
        .exit = netlab_net_exit,   /* 对应 nf_unregister_net_hooks(net, ...) */
};
register_pernet_subsys(&netlab_net_ops);
```
注意此时 `netlab_ops` 不能再是全局共享的可写结构，通常要为每个 net
复制一份，或者把 `priv` 指向 per-net 数据。
（`register_pernet_operations` 的 `.init` 在创建新 netns 时被调用，
所以 `ip netns add` 出来的新命名空间也会被覆盖。）

**Q8. 模块卸载的顺序为什么是"先摘 procfs/debugfs，再摘 hook"？**
A：两个理由。
(1) **避免用户态读到半截数据**：procfs 节点一旦暴露，用户态随时可能 `read()`；
先把入口摘掉，再停止数据面写入，读者不会看到"数据结构正在被拆"。
(2) **`nf_unregister_net_hook()` 会等 RCU**：它把 ops 从 hook 链上摘掉后
会做同步（等价于 `synchronize_rcu()`），保证返回时**不再有任何 CPU
停留在我们的 `netlab_hook()` 里**。因为之后就没有新的读者了，
才能安全地释放/遗忘全局数据。
实操上还要注意：`__exit` 函数必须与 `__init` **镜像对称**地清理 ——
`proc_create` 对应 `proc_remove`、`debugfs_create_dir` 对应
`debugfs_remove_recursive`、`nf_register_net_hooks` 对应
`nf_unregister_net_hooks`。少一个就是 `rmmod` 之后留下悬空指针，
下一次访问直接 oops。

**Q9. `skb_header_pointer()` 返回 NULL 时为什么不能直接 `return NF_DROP`？**
A：两层原因。
(1) **语义**：本模块是观测者，观测者不应该改变数据流的命运。
如果因为"我没读到"就把包丢了，那监控系统本身成了故障源 ——
线上最常见的低级事故就是"上个监控模块把生产流量丢了"。
(2) **正确性**：`skb_header_pointer()` 返回 NULL 只表示
"我想读的那段不在当前可见范围内"，这在 GSO/GRO、非线性 skb、
截断包上完全正常，不表示包是坏的。要判断包是否合法，
应该看协议栈自己的校验（`ip_rcv()` 里的 `ip_rcv_core()` 会检查
版本、头长、checksum、`pskb_may_pull` 等）。
正确做法是统计"我看不清的包"到一个单独计数器里（本模块把它记成
`proto=UNKNOWN`），然后照常 `NF_ACCEPT`。

**Q10. 为什么模块里用 `READ_ONCE()` / `WRITE_ONCE()` 访问 `enabled`？**
A：`enabled` 是一个可以在运行时被另一个 CPU 改写的标志：
- 用户可能 `echo 0 > /sys/module/netlab/parameters/enabled`，
- 或者 `echo disable > /proc/netlab_stats`，
两条路径都在**另一个 CPU、进程上下文**里写，
而 `netlab_hook()` 在软中断上下文里读。
`READ_ONCE()` 保证编译器不会把这个读操作拆开、不会把它提到别的
分支之外、也不会缓存旧值；`WRITE_ONCE()` 保证写入不被拆开。
严格地说这种"无锁标志位"更严谨的写法是 `WRITE_ONCE` + `smp_store_release`，
但对于一个统计开关，`READ_ONCE`/`WRITE_ONCE` 已经足够 ——
最坏情况是开关切换的那一瞬间有几个包被算进/漏算，这不影响正确性。
反面例子：如果这里用 `static int` 直接读，编译器完全可以把
`if (!enabled)` 的判断提到循环外面（提升优化），导致开关"不生效"。

**Q11. 环形缓冲为什么是"全局 + 锁"而不是也做成 per-CPU？**
A：可以做，而且高吞吐场景下**应该**做 —— 每个 CPU 一个私有环，
读取时按时间戳归并，就能做到写路径零锁。
本模块故意保留一个全局环，是为了让读者看清"跨 CPU 共享数据必须加锁"这件事，
以及 `spin_lock_irqsave` vs `spin_lock` 的选择依据（软中断 vs 进程上下文的竞争）。
真实的生产模块（比如 `sample` 记录器）通常会用 per-CPU ring + `BPF_MAP_TYPE_RINGBUF`
之类的机制来避免这个锁；这也是 eBPF 在可观测性上取代手写模块的原因之一。

**Q12. `module_param(x, bool, 0644)` 里的 `0644` 是什么？改成 `0444` 会怎样？**
A：它是 `/sys/module/netlab/parameters/x` 这个 sysfs 文件的权限位。
`0644` 表示 root 可读写、其他人只读 —— 也就是**运行时可以改**；
`0444` 则是只读，`echo` 进去会得到 `Permission denied`。
注意这里用的是八进制字面量，且**内核里没有 root 的“写”之外的概念**：
普通用户对这个文件永远没有写权限，无论 `0666`。
想看效果：`ls -l /sys/module/netlab/parameters/`。
另外 `module_param` 的类型（bool/uint/charp/…）决定了 sysfs 解析方式，
写错类型（比如给 `bool` 写 `2`）会返回 `-EINVAL`。

---

## 8. 内核模块开发的安全注意事项（Never break the box）

> 内核模块运行在 **ring 0**。它没有进程隔离、没有 MMU 保护、
> 没有"异常被捕获后重试"。一个空指针解引用就是**整车 panic**。

### 8.1 版本与符号校验

- **vermagic**：`insmod` 会比对模块 `.modinfo` 里的 vermagic 与运行内核的
  `UTS_RELEASE` + SMP/PREEMPT/MODULE_UNLOAD 配置。不一致直接拒绝：
  `insmod: ERROR: could not insert module ...: Invalid module format`。
  本模块的 vermagic（真实）：`6.12.107+deb13-amd64 SMP preempt mod_unload modversions`。
- **CONFIG_MODVERSIONS**：vermagic 里的 `modversions` 表示内核编译时启用
  了符号 CRC 校验。模块里引用的每个导出符号都带着 CRC，
  符号签名变了会报 `disagrees about version of symbol module_layout`
  之类。所以**内核一升级就必须重新编译模块**。
- **`depends:`**：`modinfo` 显示模块间的依赖，`modprobe` 用它做拓扑排序；
  `insmod` 不看依赖，所以要自己保证依赖模块已经加载。

### 8.2 Secure Boot 与模块签名

- `CONFIG_MODULE_SIG_FORCE=y`（很多发行版启用）时，**未签名模块一律拒绝加载**，
  报错形如 `insmod: ERROR: could not insert module netlab.ko: Key was rejected by service`。
- UEFI **Secure Boot** 开启 + `CONFIG_MODULE_SIG` + `MODULE_SIG_FORCE` 是最常见的
  组合。绕过方式（都不是好主意）：
  - 关 Secure Boot（降低整机安全性）；
  - 进 MOK 管理界面注册自己的签名密钥（`mokutil --import`，
    用 `scripts/sign-file` 签名），这是**正规做法**；
  - 有的发行版对 DKMS 有自动签名流程。
- 判断当前机器状态：
  ```console
  $ mokutil --sb-state        # 需要装 mokutil
  $ zgrep -E 'MODULE_SIG|MODULE_SIG_FORCE' /proc/config.gz
  $ cat /sys/module/module/parameters/sig_enforce   # 有的内核提供
  ```
  本机实测 `ls /sys/module/module/parameters/` 可读，可以自己看一眼。

### 8.3 "Never break the box" 操作纪律

1. **永远别在生产机 / 主力工作机上首次 `insmod` 一个自己刚写的模块。**
   先用虚拟机（QEMU + 自己编译的内核）或一次性容器/测试机。
2. **准备逃生通道**：
   - 物理机/虚拟机：串口控制台（`console=ttyS0`）、
     `magic SysRq`（先 `echo 1 > /proc/sys/kernel/sysrq`，
     再用 `SysRq` 键组合，可以 `sync`/`umount`/`reboot`）；
   - 云主机：厂商的 VNC / 串口 / 强制重启按钮；
   - 确认你知道"panic 之后怎么恢复"再动手。
3. **panic 不是最坏的结果**：最坏是"部分初始化 → 数据损坏 → 几小时后
   文件系统崩了"。所以 `__init` 里的每一步失败都要**完整回滚**，
   并把错误码传上去（本模块就是：hook 注册失败就返回；
   `proc_create` 失败就注销 hook 再返回）。
4. **不要在 hook 里改别人的数据**：像本模块这样只读 + 只统计，
   即便有 bug 也只会"统计错"或"自己 panic"，不会悄悄改坏用户流量。
   一旦开始 `skb_store_bytes` / 改 `skb->mark` / 返回 `NF_DROP`，
   就必须考虑与 conntrack、NAT、fail-open 的交互。
5. **`rmmod` 之前确认没有引用**：本模块有 procfs 入口和 netfilter hook，
   两者都在 `__exit` 里摘干净；否则 `rmmod` 后回调仍指向已卸载的代码 → oops。
   内核会保护性拒绝卸载（`Module netlab is in use`），
   但如果引用计数没记对，就是一条 use-after-free 路径。
6. **调试手段的顺序**：`pr_info/pr_debug`（配 `dynamic_debug`）→
   `/sys/kernel/debug/tracing/`（ftrace/kprobe，**不用重编译**）→
   `bpftrace` → 最后才是加 printk 重编译。
   本机 `dmesg_restrict=1`、`/sys/kernel/debug` 不可读，
   所以这些手段都需要 root，正好说明为什么"内核调试"默认是 root 的活。
7. **别把 root-only 的接口开给普通用户**：procfs 节点写成 `0444` 只读、
   `0644` 只允许 root 写。写成 `0666` 就是给整个系统开了一个
   未审计的内核写入口。

### 8.4 本机为什么不能加载（结论）

| 条件 | 本机状态 | 结论 |
| --- | --- | --- |
| uid | `1000`（非 root） | ✗ |
| CapEff | `0000000000000000` | ✗ 无 `CAP_SYS_MODULE` |
| `no_new_privs` | 已设置（`sudo` 也无法提权） | ✗ |
| 内核头文件 | `/lib/modules/6.12.107+deb13-amd64/build` 存在 | ✓ |
| gcc / make | 14.2.0 / 4.4.1 | ✓ |
| vermagic 匹配 | 与 `uname -r` 完全一致 | ✓ 编译无障碍 |
| **Secure Boot** | **`SecureBoot=1`、`SetupMode=0`** | **✗ 根本障碍：未签名模块被拒** |
| **运行时 `sig_enforce`** | `/sys/module/module/parameters/sig_enforce` = **`Y`** | **✗ 直接原因** |
| **lockdown** | `none [integrity] confidentiality` | **✗ lockdown=integrity 生效** |

**能做的：编译、`modinfo`、符号分析、vermagic 校验。**
**不能做的：`insmod` / 读 `/proc/netlab_stats` / `rmmod` / `dmesg`**（`dmesg_restrict=1`）。

#### 8.4.1 实测过程与证据（2026-10-13）

用 `docker run --privileged`（真 root、`CapEff=000001ffffffffff`，并把宿主的
`/lib/modules`、`/usr/src`、`kmod` 挂进去）尝试加载，内核**直接拒绝**：

```console
$ /bin/insmod /repo/labs/06-kernel-module-netfilter/netlab.ko
insmod: ERROR: could not insert module /repo/.../netlab.ko: Key was rejected by service
```

`Key was rejected by service` 就是 `EKEYREJECTED`，来自模块签名校验。根因链：

| 检查项 | 本机实测值 | 说明 |
| --- | --- | --- |
| `SecureBoot` EFI 变量 | `1`（`SetupMode=0`） | UEFI Secure Boot 已开启 |
| `CONFIG_LOCK_DOWN_IN_EFI_SECURE_BOOT` | `y` | Secure Boot → 自动进 lockdown |
| `/sys/kernel/security/lockdown` | `none [integrity] confidentiality` | 当前为 integrity |
| `/sys/module/module/parameters/sig_enforce` | **`Y`** | **运行时签名强制已开** |
| `CONFIG_MODULE_SIG_FORCE` | `# ... is not set` | 配置层面没强制，是 Secure Boot 触发的 |
| `CONFIG_MODULE_SIG` / `_SHA256` | `y` / `y` | 内核支持模块签名 |
| `modinfo netlab.ko` 的 `signer` / `sig_id` | 无 | 本地编出的模块**未签名** |

**结论：能不能加载模块，要过三道门 —— 权限（capability）→ 模块格式
（vermagic / 符号版本）→ 签名（Secure Boot / lockdown）。本机卡在第三道。**
三道门的报错各不相同（`Operation not permitted` / `Invalid module format` /
`Key was rejected by service`），会读报错就知道卡在哪一道。

### 8.5 如果一定要在本机真正加载：用 MOK 给模块签名

本机三件工具齐全（实测）：内核自带的 `scripts/sign-file`、`openssl`、`mokutil`。
正规做法是用一张 **MOK（Machine Owner Key）** 签名，并把公钥登记进固件——
**不需要关闭 Secure Boot**。本目录下的 [`mok-sign.sh`](mok-sign.sh) 已把第 1~2 步
自动化（实测可用：签名后 `modinfo` 会多出 `sig_id: PKCS#7` / `signer: learn-net module signing`）：

```bash
bash mok-sign.sh        # 只生成密钥 + 签名；故意不碰 mokutil/固件
```

手动步骤等价于：

```bash
# 1) 生成签名密钥对（放哪都行，这里放 ~/mok）
mkdir -p ~/mok && cd ~/mok
openssl req -new -x509 -newkey rsa:2048 -keyout MOK.key -out MOK.crt \
        -nodes -days 3650 -subj "/CN=learn-net module signing/"
openssl x509 -in MOK.crt -out MOK.cer -outform DER

# 2) 用内核自带的 sign-file 给模块附签名（sha256）
K=/usr/src/linux-headers-$(uname -r)
$K/scripts/sign-file sha256 MOK.key MOK.crt \
    ~/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko
modinfo ~/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko | grep -iE 'signer|sig_id'

# 3) 登记公钥（会要求设一个一次性密码），然后**重启**，在开机 MOK Manager 界面确认
sudo mokutil --import MOK.cer

# 4) 重启之后
sudo insmod ~/code/learn-net/labs/06-kernel-module-netfilter/netlab.ko
cat /proc/netlab_stats        # 到这一步才是真正跑起来
```

三条替代路径的取舍：

| 方案 | 代价 |
| --- | --- |
| MOK 签名（上面这套） | 重启一次 + 进固件界面确认；此后**任何用该密钥签名的模块**都能加载 |
| 关闭 Secure Boot | 整机安全性下降，不建议 |
| 换没有 Secure Boot 的机器 / QEMU 虚拟机 | 最安全，也正是 8.3 节推荐的「首次 `insmod` 别在主力机」 |

---

## 9. 延伸练习

1. 给模块加第三个 hook：`NF_INET_PRE_ROUTING`，观察
   `skb_dst(skb)` 是否为 NULL，并把结果打印到 `/proc/netlab_stats`。
2. 实现 `struct pernet_operations`，让模块覆盖所有 netns；
   用 `ip netns add n1` 建一个 netns 来验证。
3. 把 `priority` 从 `NF_IP_PRI_FIRST + 1` 改成 `NF_IP_PRI_NAT_SRC + 1`，
   在本机加一条 SNAT（`iptables -t nat -A POSTROUTING ... -j SNAT`），
   对比前后看到的源端口。
4. 把全局环形缓冲改成 per-CPU 环形缓冲 + 读取时归并，
   用 `perf stat` 或 `iperf3` 对比吞吐。
5. 加一个"1/N 采样"参数，让落记录的频率可控，并说明对
   per-CPU 计数（每包都记）与明细记录（采样）为什么可以分别设计。

---

## 10. 延伸阅读

- `man 2 bpf`、`man 7 netfilter`（部分发行版需要手动装 `man-pages`）
- Linux 内核文档：`Documentation/core-api/this_cpu_ops.rst`（per-CPU 语义权威说明）
- Linux 内核文档：`Documentation/networking/filter.rst`（socket filter 与 BPF）
- Linux 内核文档：`Documentation/kbuild/modules.rst`（外部模块构建的官方说明）
- Linux 内核文档：`Documentation/process/kernel-docs.rst`
- 源码（v6.12）：<https://elixir.bootlin.com/linux/v6.12/source/net/netfilter/core.c>
- 源码（v6.12）：<https://elixir.bootlin.com/linux/v6.12/source/include/linux/netfilter.h>
- 本教程下一章：`docs/06-netfilter-conntrack与NAT.md`
  （把本模块看到的 hook 点放进 conntrack / NAT 的完整图景里）
- 再下一章：`docs/07-eBPF-XDP与可编程数据面.md`
  （当"写内核模块"太贵时，eBPF 这条路为什么更安全、更适合生产观测）
