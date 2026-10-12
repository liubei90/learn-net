# 06-tracepoint-templates.md —— perf / ftrace / bpftrace 一行命令模板

> **本文件是「模板 + 前置条件」，不是可直接运行的实验。**
> 原因：本教程的开发机（Debian 13, 内核 6.12.107）**没有权限也没有工具**来运行
> 这些命令。下面的模板全部经过**字段名核对**（对照本机内核源码头文件
> `include/trace/events/*.h`，即 6.12.107 的真实定义），但**命令本身未在本机执行**。
> 换一台有 root 的机器即可直接使用。

---

## 0. 为什么本机跑不了：实测环境事实

| 检查项 | 本机实测结果 | 对 tracepoint 实验的影响 |
|---|---|---|
| 用户 / capabilities | `uid=1000`，`CapEff=0x0000000000000000` | ❌ 没有 `CAP_BPF` / `CAP_PERFMON` / `CAP_SYS_ADMIN`，无法打开 perf event、无法写 tracefs |
| `/sys/kernel/tracing` | 已挂载，但为 **`ro`（只读）** 且无读权限 | ❌ 无法 `echo 1 > events/.../enable` |
| `/sys/kernel/debug` | 已挂载，同为 **`ro`** 且无读权限 | ❌ 无法使用 debugfs 下的 tracing |
| `kernel.unprivileged_bpf_disabled` | `2` | ❌ 值为 **2** 表示「已禁用且**无法在运行时重新启用**」（1 表示可改回 0）。所以非特权 bpftrace 无解 |
| `bpftrace` / `bcc` / `perf` / `trace-cmd` | 全部 **未安装** | ❌ 无用户态工具 |
| `CONFIG_FTRACE` / `CONFIG_KPROBE_EVENTS` / `CONFIG_BPF_SYSCALL` / `CONFIG_BPF_JIT` | 均为 `y` | ✅ 内核**支持**这些能力，纯粹是权限与工具缺失 |
| `CONFIG_BPF_JIT_ALWAYS_ON` | 未设置 | ℹ️ 解释器模式可用 |

**结论**：这些实验在本机属于「有内核能力、无用户态权限」。脚本检测不到前置条件时
一律打印 `SKIP` 并以 `exit 0` 退出（见 `Makefile` 的 `run` 目标），不会报错中断。

### 前置条件清单（在别的机器上要跑通，需要什么）

```bash
# 1) root（或等效 capability）
sudo -i

# 2) tracefs 必须以读写方式挂载
mount | grep tracefs          # 期望看到没有 ro
# 若为 ro：
mount -o remount,rw /sys/kernel/tracing
# 若根本没挂载：
mount -t tracefs nodev /sys/kernel/tracing

# 3) 安装工具（Debian/Ubuntu）
apt-get install -y linux-perf bpftrace bpfcc-tools trace-cmd
#    RHEL/CentOS/Fedora
# dnf install -y perf bpftrace bcc-tools trace-cmd

# 4) 允许非特权 BPF（仅当你想用非 root 跑 bpftrace）
sysctl -w kernel.unprivileged_bpf_disabled=0
```

> ⚠️ 若 `unprivileged_bpf_disabled` 的值是 `2`，把它改成 `0` 会失败；这是内核
> 启动时锁定的一次性设置，只能重启后在启动参数里改。本机就是 `2`。

---

## 1. 已核对的 tracepoint 字段表（6.12.107 源码为依据）

字段名取自本机 `/usr/src/linux-headers-6.12.107+deb13-common/include/trace/events/`，
这是**与运行内核完全同版本**的头文件，所以字段名可以直接信赖。
在 tracefs 里，这些字段会出现在 `/sys/kernel/tracing/events/<group>/<event>/format` 的
`field:` 行中，perf 与 bpftrace 的字段名与之一致。

| tracepoint | 源码文件 | 可用字段（6.12 实测核对） |
|---|---|---|
| `skb:kfree_skb` | `events/skb.h` | `skbaddr`, `location`, `rx_sk`, `protocol`, `reason` |
| `net:net_dev_queue` | `events/net.h` (`net_dev_template`) | `skbaddr`, `len`, `name` |
| `net:net_dev_xmit` | `events/net.h` | `skbaddr`, `len`, `rc`, `name` |
| `net:napi_gro_receive_entry` | `events/net.h` (`net_dev_rx_verbose_template`) | `name`, `napi_id`, `queue_mapping`, `skbaddr`, `vlan_tagged`, `vlan_proto`, `vlan_tci`, `protocol`, `ip_summed`, `hash`, `l4_hash`, `len`, `data_len`, `truesize`, `mac_header_valid`, `mac_header`, `nr_frags`, `gso_size`, `gso_type` |
| `sock:inet_sock_set_state` | `events/sock.h` | `skaddr`, `oldstate`, `newstate`, `sport`, `dport`, `family`, `protocol`, `saddr`, `daddr`, `saddr_v6`, `daddr_v6` |
| `tcp:tcp_retransmit_skb` | `events/tcp.h` (`tcp_event_sk_skb`) | `skbaddr`, `skaddr`, `state`, `sport`, `dport`, `family`, `saddr`, `daddr`, `saddr_v6`, `daddr_v6` |
| `tcp:tcp_probe` | `events/tcp.h` | `saddr`, `daddr`, `sport`, `dport`, `family`, `mark`, `data_len`, `snd_nxt`, `snd_una`, `snd_cwnd`, `ssthresh`, `snd_wnd`, `srtt`, `rcv_wnd`, `sock_cookie`, `skbaddr`, `skaddr` |
| `napi:napi_poll` | `events/napi.h` | `napi`, `dev_name`, `work`, `budget` |

**关于字段类型的两个坑**：

1. `__string(name, ...)` 在 tracefs 里是一个字符串字段，bpftrace 里类型是
   `char *`，要用 `str()` 包一层才能打印。
2. `skbaddr` / `skaddr` / `skbaddr` 是**指针**，bpftrace 里要 `%p`-ish 处理（直接打印
   通常会得到内核地址，某些内核因 `kptr_restrict` 会显示成全 0）。

### 关于 `tcp:tcp_probe` 的一个准确性提醒

`tcp:tcp_probe` 这个事件在 6.12 的 `events/tcp.h` 中**确实存在**（已核对）。
但它的**触发点**需要读者自行确认：老版本内核里 `tcp_probe` 是一个独立的内核模块，
只有在加载该模块后这个 tracepoint 才会被触发；若你的内核里没有触发点，
`enable` 之后会什么都看不到。

> 以 6.12 为准，请在 `elixir.bootlin.com/linux/v6.12/source/` 下搜索
> `trace_tcp_probe` 确认调用点位置，或用
> `grep -rn "trace_tcp_probe" net/ipv4/` 在本地源码树里查。

---

## 2. perf 模板

perf 是最通用的入口。`perf list` 可以列出本机所有可用 tracepoint。

### 2.1 列出所有网络相关 tracepoint

```bash
perf list 'net:*' 'skb:*' 'sock:*' 'tcp:*' 'napi:*'
```
**用途**：确认你的内核/工具版本里到底有哪些事件，避免照抄模板后报「unknown event」。
**输出示例格式**：
```
  net:net_dev_queue                                   [Tracepoint event]
  skb:kfree_skb                                       [Tracepoint event]
  tcp:tcp_retransmit_skb                              [Tracepoint event]
```

### 2.2 统计丢包原因分布（最常用的一条）

```bash
perf stat -a -e skb:kfree_skb sleep 5
```

**用途**：5 秒内全系统 `kfree_skb` 的触发次数。`kfree_skb` 是所有「包被丢弃」的
统一出口，次数异常增长说明协议栈在丢包。
**输出示例格式**：
```
 Performance counter stats for 'system wide':

             12345      skb:kfree_skb
       5.001234567 seconds time elapsed
```

> 注意：`kfree_skb` 也包含**正常**的丢弃（例如主动丢弃不需要的包），
> 所以一定要配合 2.3 看 `reason`，不要只看总数就下结论。

### 2.3 按丢包原因（drop reason）分解 —— 定位「为什么丢」

```bash
perf record -a -e skb:kfree_skb --call-graph dwarf -- sleep 5
perf script | awk '{for(i=1;i<=NF;i++) if($i ~ /reason:/) print $0}' | head -50
```

**用途**：`kfree_skb` 的 `reason` 字段是内核的 `enum skb_drop_reason`
（如 `NO_SOCKET`、`SOCKET_FILTER`、`TCP_CSUM`、`IP_NOPROTO`、`NETFILTER_DROP`、
`SKB_OVERFLOW`、`PKT_TOO_SMALL` 等）。这是回答「丢包丢在哪一层」最直接的证据。
**输出示例格式**：
```
 ksoftirqd/0  1234 [000] 12345.678901: skb:kfree_skb: skbaddr=0xffff... protocol=2048 location=0xffffffff81a2b3c4 reason: NO_SOCKET
```
> `location` 字段是**丢弃代码的地址**，perf script 会自动翻译成函数名
> （如 `tcp_v4_rcv`），这比 `reason` 更精确。

### 2.4 观察 TCP 状态迁移事件流

```bash
perf record -a -e sock:inet_sock_set_state -- sleep 10
perf script
```
**用途**：每一次 TCP 状态变化都记录一条。这是诊断
「连接为什么卡在 SYN_SENT / CLOSE_WAIT 堆积」的**事件级**手段
（对比 `07-watch-tcp-state.sh` 的轮询快照，事件流不会漏掉短连接）。
**输出示例格式**：
```
 swapper     0 [002] 111.222333: inet_sock_set_state: family=2 sport=80 dport=54321 oldstate=1 newstate=6
```
`oldstate`/`newstate` 的数值就是 `include/net/tcp_states.h` 里的 `TCP_*` 枚举
（1=ESTABLISHED, 6=TIME_WAIT, 8=CLOSE_WAIT, 10=LISTEN ...）。

### 2.5 抓重传事件并看 RIP/调用栈

```bash
perf record -a -e tcp:tcp_retransmit_skb -c 1 -- sleep 10
perf script | head -40
```
**用途**：重传次数（`/proc/net/snmp` 的 `RetransSegs`）只告诉你「重传了多少」，
这个模板告诉你「**哪些连接**在重传、端口是多少」。
**输出示例格式**：
```
 ksoftirqd/1  56 [001] 222.333444: tcp:tcp_retransmit_skb: skbaddr=0xffff... skaddr=0xffff... state=1 sport=443 dport=51234 family=2
```

### 2.6 观察 NAPI poll 的工作量与预算

```bash
perf record -a -e napi:napi_poll -- sleep 5
perf script | awk '{print $NF, $(NF-2), $(NF-1)}' | sort | uniq -c | sort -rn | head
```
**用途**：每条记录带 `work`（本次实际处理了多少包）与 `budget`（内核给的预算）。
当 `work` 经常**贴近** `budget` 时，说明单次 poll 总是跑满预算 ——
这正是 `softnet_stat` 第 3 列 `time_squeeze` 增长的原因。
**输出示例格式**：
```
 ksoftirqd/0  12 [000] 333.444555: napi_poll: napi poll on napi struct 0xffff... for device eth0 work 64 budget 64
```

### 2.7 网卡收发路径的端到端延迟

```bash
perf record -a -e net:net_dev_queue -e net:net_dev_xmit -e net:napi_gro_receive_entry -- sleep 5
perf script --ns | head -40
```
**用途**：`net_dev_queue`（进入 qdisc）、`net_dev_xmit`（真正交给驱动）、
`napi_gro_receive_entry`（驱动收到包并进入 GRO）三者配合时间戳，
可以粗略看出包在协议栈里停留了多久。
**输出示例格式**：
```
 swapper 0 [000] 444.555666: net_dev_queue: dev=eth0 skbaddr=0xffff... len=74
 swapper 0 [000] 444.555670: net_dev_xmit: dev=eth0 skbaddr=0xffff... len=74 rc=0
```

---

## 3. ftrace / tracefs 模板

ftrace 不需要额外工具，只要能写 `/sys/kernel/tracing`。它比 perf 更"原始"，
但**开销最低**，适合生产环境短时间排查。

### 3.1 通用开关模板（把 EVENT 换成任意事件名）

```bash
cd /sys/kernel/tracing
echo 0 > tracing_on
echo nop > current_tracer
echo > trace
echo 1 > events/net/net_dev_queue/enable
echo 1 > tracing_on
sleep 3
echo 0 > tracing_on
echo 0 > events/net/net_dev_queue/enable
cat trace | head -50
```
**用途**：最基础的事件记录。所有 ftrace 用法都是这个骨架。
**输出示例格式**（列的含义：进程名-PID、CPU#、时间戳、事件名、字段）：
```
#           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
#              | |         |   |||||     |         |
          <idle>-0       [002] d.h. 12345.678901: net_dev_queue: dev=eth0 skbaddr=0xffff... len=98
```

### 3.2 只看特定进程 / 特定事件，减少噪音

```bash
echo 1 > events/skb/kfree_skb/enable
echo 1 > options/function-fork      # 跟随子进程
cat trace_pipe                       # 实时流式输出，Ctrl-C 退出
```

### 3.3 用 filter 只留下关心的事件（非常实用）

```bash
# 只看 80 端口的重传
echo 'dport == 80' > events/tcp/tcp_retransmit_skb/filter
echo 1 > events/tcp/tcp_retransmit_skb/enable

# 只看丢包原因里的 NO_SOCKET
echo 'reason == 1' > events/skb/kfree_skb/filter
```
**用途**：filter 语法就是 C 表达式，字段名和上面第 1 节的表一致。
**先看有哪些字段和可选值**：
```bash
cat events/skb/kfree_skb/format
```
`format` 文件会列出每个字段的名字、类型、偏移，以及 `print fmt` 的格式串。
**这是最权威的字段参考**——比任何博客都准。

### 3.4 统计而非记录（开销更低）

```bash
echo > trace
echo 1 > events/tcp/tcp_retransmit_skb/enable
cat trace_stat/function   # 或者用 events/.../hist 触发器
```
**注意**：聚合统计更推荐用 bpftrace 或 `perf stat`。

### 3.5 查看 tracepoint 列表与字段的正确姿势

```bash
ls /sys/kernel/tracing/events/            # 所有事件组
ls /sys/kernel/tracing/events/tcp/        # tcp 组下所有事件
cat /sys/kernel/tracing/events/tcp/tcp_probe/format
```

---

## 4. bpftrace 模板

bpftrace 用类 awk 的语法做内核级聚合，是**做统计和直方图最舒服的工具**。

### 4.1 统计各进程触发的丢包次数

```bash
bpftrace -e 'tracepoint:skb:kfree_skb { @[comm] = count(); }'
```
**用途**：找出「谁在丢包」。`comm` 是触发丢包时正在运行的进程名。
**输出示例格式**（Ctrl-C 后打印聚合结果）：
```
@[swapper/0]: 1423
@[ksoftirqd/1]: 87
@[nginx]: 12
```
> 若在软中断上下文触发，`comm` 会是 `swapper/N` 或 `ksoftirqd/N` —— 这本身就是
> 有用信息：说明丢包发生在软中断里，而不是某个应用的系统调用路径上。

### 4.2 按丢包原因聚合（最有用的一条）

```bash
bpftrace -e 'tracepoint:skb:kfree_skb { @[args->reason] = count(); }'
```
**用途**：直接把 `enum skb_drop_reason` 当聚合键，一眼看出丢包的根因分布。
**输出示例格式**：
```
@[NO_SOCKET]: 900
@[SOCKET_FILTER]: 300
@[TCP_CSUM]: 12
```
> ⚠️ `args->reason` 打印出来可能是数字而不是名字（取决于 bpftrace 版本是否内置
> 该枚举的符号表）。是数字时，请对照
> `include/net/dropreason-core.h`（或 `include/linux/skbuff.h`）的
> `enum skb_drop_reason` 顺序解读。

### 4.3 TCP 状态迁移的直方图 / 时序

```bash
bpftrace -e 'tracepoint:sock:inet_sock_set_state { @[args->oldstate, args->newstate] = count(); }'
```
**用途**：看状态机里哪条边最热。例如 `@[1, 6]` 表示大量 ESTABLISHED→TIME_WAIT，
说明本机是主动关闭方且短连接很多。

### 4.4 按端口看重传

```bash
bpftrace -e 'tracepoint:tcp:tcp_retransmit_skb { @[args->dport] = count(); }'
```
**输出示例格式**：
```
@[443]: 231
@[80]: 12
```

### 4.5 观察某条连接的 RTT / cwnd 变化（用 tcp:tcp_probe）

```bash
bpftrace -e 'tracepoint:tcp:tcp_probe /args->dport == 443/ {
    printf("%s cwnd=%u ssthresh=%u srtt=%u\n", comm, args->snd_cwnd, args->ssthresh, args->srtt);
}'
```
**用途**：`srtt` 单位是微秒？还是内核内部刻度？**以源码为准**：
`events/tcp.h` 里直接取 `tp->srtt_us`，因此单位是**微秒**（>0 时）。
**再次提醒**：`tcp:tcp_probe` 在本内核是否真的有触发点，需按第 1 节末尾的说明核对。

### 4.6 谁在调用 connect() 并失败

```bash
bpftrace -e 'kprobe:tcp_v4_connect { @[comm] = count(); }'
```
**用途**：kprobe 不依赖 tracepoint，只要能挂 kprobe 就行（需要 `CAP_PERFMON`/root）。
**风险**：kprobe 挂在高频函数上会显著影响性能，生产环境务必用短时间窗口。

### 4.7 一行式：实时看 NAPI work/budget 比率

```bash
bpftrace -e 'tracepoint:napi:napi_poll { @work = hist(args->work); @budget = hist(args->budget); }'
```
**用途**：直方图一眼看出 `work` 是否长期贴着 `budget`。
**输出示例格式**：
```
@budget:
[32, 64)              412 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
@work:
[32, 64)              398 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  |
```

---

## 5. 本机优雅降级验证

本目录的 `Makefile` 的 `run` 目标会尝试这些实验，并在检测不到前置条件时打印
`SKIP` 且以 0 退出。可用下面命令确认降级行为：

```bash
cd labs/09-observability
make run                      # 观察需要 root 的步骤都打印 SKIP
scripts/env-check.sh          # 在仓库根目录，打印工具/tracefs/capability 检查表
```

---

## 6. 权威延伸阅读

- tracepoint 的**权威字段参考**永远是本机这个文件：
  `/sys/kernel/tracing/events/<group>/<event>/format`
  （本机不可读，但任何有 root 的机器上都能看，且 100% 准确）
- 内核源码（以 6.12 为准）：`elixir.bootlin.com/linux/v6.12/source/`
  - `include/trace/events/tcp.h`、`sock.h`、`skb.h`、`net.h`、`napi.h`
  - `include/net/dropreason-core.h` —— `enum skb_drop_reason` 的完整列表
  - `Documentation/trace/` —— ftrace 官方文档
  - `Documentation/bpf/bpftrace.rst` —— bpftrace 官方文档
- Brendan Gregg 的 `Linux BPF Performance Tools` 一书与
  `brendangregg/bpf-perf-tools-book` 仓库（工具脚本集合）
