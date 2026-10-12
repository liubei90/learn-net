# Lab 03：手写 rtnetlink —— 不依赖 libnl 的网络配置查询

> 对应文档：[`docs/04-网络层-IP路由与邻居子系统.md`](../../docs/04-网络层-IP路由与邻居子系统.md)、
> [`docs/05-链路层-网卡驱动-NAPI与中断.md`](../../docs/05-链路层-网卡驱动-NAPI与中断.md)

本 Lab 用 **纯 C11 + 内核 UAPI 头文件**，手写一个 rtnetlink 客户端，
复刻 `ip link` / `ip addr` / `ip route` 的查询功能。

**本 Lab 的全部功能都是只读查询，不需要任何 capability，在非特权账户下可以直接真跑。**
下面的所有输出均是在本机实测捕获的（不是示意）。

---

## 0. 本章定位

- **协议栈层次**：netlink 不是协议栈的一层，而是**用户态与内核网络子系统之间的配置/观测通道**。
  它横跨 L2（`RTM_GETLINK`）、L3（`RTM_GETADDR`/`RTM_GETROUTE`）以及 netfilter、
  邻居子系统、qdisc 等几乎所有可配置面。
- **前置知识**：socket API 基础（`docs/01`）、C 语言结构体与内存布局、
  字节序（网络序/主机序）。
- **学完能回答**：
  1. `ip route show` 底层到底发了什么？内核回了什么？
  2. netlink 报文为什么必须 4 字节对齐？TLV 属性是怎么嵌套的？
  3. dump（多部分消息）是怎么结束的？`NLMSG_ERROR` 什么时候不是错误？
  4. 为什么 `ethtool`、`ip`、`ss`、`nft` 都在往 netlink 迁移，而 ioctl 在退场？

---

## 1. 构建与运行

```console
$ make clean all
cc -D_GNU_SOURCE -std=c11 -O2 -g -Wall -Wextra -c -o rtnl.o rtnl.c
cc -D_GNU_SOURCE -std=c11 -O2 -g -Wall -Wextra -c -o rtnl_dump.o rtnl_dump.c
cc -std=c11 -O2 -g -Wall -Wextra -o rtnl_dump rtnl.o rtnl_dump.o

$ ./rtnl_dump link      # 等价 ip -j link show
$ ./rtnl_dump addr      # 等价 ip -j addr show
$ ./rtnl_dump route     # 等价 ip -j route show（含 local 表，见 §6）
$ ./rtnl_dump all
```

**验证过零警告**：额外用 `-Werror` 编译，退出码 0。

```console
$ make clean && make CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Werror"
...
BUILD_EXIT=0        # 无任何 warning
```

---

## 2. netlink 消息格式

netlink 是**面向消息（datagram）**的协议，不是字节流。
每条消息（`struct nlmsghdr`）后面跟一个「family 专属头」，
再后面跟一串 **TLV 属性**（`struct rtattr`）。

```
┌──────────────────────────────────────────────────────────┐
│ struct nlmsghdr（16 字节，所有 netlink 家族通用）          │
│   __u32 nlmsg_len     整条消息长度，含本头，按 4 字节对齐   │
│   __u16 nlmsg_type    消息类型 / 家族专属类型              │
│   __u16 nlmsg_flags   请求/多部分/ACK 等标志               │
│   __u32 nlmsg_seq     序号，用于匹配请求与回复              │
│   __u32 nlmsg_pid     发送方端口号（0 = 内核）             │
├──────────────────────────────────────────────────────────┤
│ family 专属头（rtnetlink 家族）                            │
│   RTM_*LINK  -> struct ifinfomsg  (16 字节)               │
│   RTM_*ADDR  -> struct ifaddrmsg  ( 8 字节)               │
│   RTM_*ROUTE -> struct rtmsg      (12 字节)               │
├──────────────────────────────────────────────────────────┤
│ 属性区：struct rtattr[]                                   │
│   __u16 rta_len    本属性总长（含 4 字节头）               │
│   __u16 rta_type   属性类型（IFLA_* / IFA_* / RTA_*）      │
│   __u8  rta_data[] 负载，长度 = rta_len - 4，4 字节对齐    │
└──────────────────────────────────────────────────────────┘
```

上面这几个 `sizeof` 都是实测值（本机 glibc 2.41 / Linux 6.12 UAPI 头）：

| 结构体 | 大小 | 说明 |
|---|---|---|
| `struct nlmsghdr` | 16 | 通用消息头 |
| `struct rtattr` | 4 | TLV 属性头 |
| `struct ifinfomsg` | 16 | link 消息头（`ifi_family/ifi_type/ifi_index/ifi_flags/ifi_change`） |
| `struct ifaddrmsg` | 8 | addr 消息头（`ifa_family/ifa_prefixlen/ifa_flags/ifa_scope/ifa_index`） |
| `struct rtmsg` | 12 | route 消息头（`rtm_family/rtm_dst_len/rtm_src_len/rtm_tos/rtm_table/rtm_protocol/rtm_scope/rtm_type/rtm_flags`） |

实测的消息类型编号（同一份程序打印，避免记错）：

```
RTM_NEWLINK=16  RTM_GETLINK=18
RTM_NEWADDR=20  RTM_GETADDR=22
RTM_NEWROUTE=24 RTM_GETROUTE=26
```

netlink 的命名惯例值得注意：**`GET` 是 `NEW` + 2**，因为内核把
「读一个对象」实现成「用空对象去 replace」，`doit` 与 `dumpit` 共用类型号。

### 请求的字节数怎么算

以 `RTM_GETLINK` + `NLM_F_REQUEST|NLM_F_DUMP` 为例（实测）：

```
NLMSG_LENGTH(16) = 32     # 16(hdr) + 16(ifinfomsg)
NLMSG_SPACE(16)  = 32     # 32 已经 4 字节对齐，故相同
NLMSG_ALIGN(17)  = 20     # 向上取整到 4 的倍数
```

注意区分两个概念：
- `nlmsg_len` 字段里写的是 **`NLMSG_LENGTH(payload)`**，即「头 + 负载」的实际长度；
- 而 `sendto()` 发送的字节数应该用 **`NLMSG_SPACE(payload)`**，即再对齐到 4 字节。

---

## 3. 对齐规则：为什么必须有 padding

netlink 有**两个独立的 4 字节对齐约束**：

| 宏 | 值 | 作用 |
|---|---|---|
| `NLMSG_ALIGNTO` | 4 | 消息与消息之间对齐 |
| `NLMSG_ALIGN(len)` | 向上取整到 4 | 下一条消息的起点 |
| `NLMSG_HDRLEN` | 16 | 已对齐的消息头长度 |
| `NLMSG_LENGTH(len)` | `len + NLMSG_HDRLEN` | `nlmsg_len` 字段的取值 |
| `NLMSG_SPACE(len)` | `NLMSG_ALIGN(NLMSG_LENGTH(len))` | 实际占用/发送字节数 |
| `RTA_ALIGNTO` | 4 | 属性与属性之间对齐 |
| `RTA_ALIGN(len)` | 向上取整到 4 | 下一个属性的起点 |
| `RTA_LENGTH(len)` | `RTA_ALIGN(sizeof(rtattr)) + len` = `4 + len` | `rta_len` 字段的取值 |
| `RTA_SPACE(len)` | `RTA_ALIGN(RTA_LENGTH(len))` | 实际占用字节数 |

**为什么需要对齐**：`nlmsg_len` 与 `rta_len` 都是 `__u16`，只记录「逻辑长度」。
接收方遍历时用 `NLMSG_NEXT`/`RTA_NEXT` 前进，而这些宏内部走的是 `*_ALIGN`。
如果发送方忘了按 `SPACE`/`RTA_SPACE` 补齐，接收方计算出的下一个偏移就会落在
错误位置，于是**解析出一堆垃圾属性，甚至越界**。这是手写 netlink 最常见的错误。

举例（实测）：一个属性若负载 6 字节，
`RTA_LENGTH(6) = 4 + 6 = 10`（写进 `rta_len`），
但它在缓冲区里实际占 `RTA_ALIGN(10) = 12` 字节，尾部 2 字节是 padding。

对齐还带来一个安全含义：**解析不可信输入时必须用 `RTA_OK`/`NLMSG_OK` 校验**，
它们会检查 `rta_len >= sizeof(struct rtattr)` 且不超过剩余长度。
本 Lab 的 `parse_rtattr()` 就是先把所有属性索引进 `tb[]`，
并且**先判 `rta_type <= max` 再写入数组**，避免越界写。

---

## 4. 多部分消息（dump）与结束条件

一次 `NLM_F_DUMP` 请求，内核会回**多条**消息。它们可能：
1. 打包在同一个 datagram 里（用 `NLMSG_NEXT` 在缓冲区内遍历）；
2. 分散在多个 datagram 里（需要反复 `recv()`）。

因此正确的收包循环必须写成两层：**外层 recv，内层 NLMSG_NEXT**。

关键细节：

- **结束标志是 `NLMSG_DONE`**，不是「recv 返回 0」，也不是「消息变少」。
- 多部分回复里每条消息都带 `NLM_F_MULTI` 标志，**最后一条除外**（`NLMSG_DONE`）。
  所以「看到 `NLM_F_MULTI` 清了就结束」是不可靠的，要以 `NLMSG_DONE` 为准。
- **`NLMSG_ERROR` 不一定是错误**：`struct nlmsgerr.error == 0` 表示 ACK
  （成功确认）。把它一律当失败处理是超常见的 bug：
  ```c
  if (h->nlmsg_type == NLMSG_ERROR) {
          struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
          if (e->error != 0) { /* 真正的错误，e->error 是负 errno */ }
          else continue;      /* 这是 ACK */
  }
  ```
- **`seq` 必须匹配**：回复里带着请求的 `seq`。多请求/多 socket 并发时，
  靠它把回复归属到正确的请求；否则会串包。

### 实战陷阱：大 dump 会丢消息（ENOBUFS）

dump 是「内核一次性把大量消息推给用户态 socket 接收队列」。
如果用户态处理慢（比如每条都 `printf` 到慢终端）或 `SO_RCVBUF` 太小，
队列会溢出，内核**丢弃消息**并让下一次收发返回 `ENOBUFS`。
结果就是「dump 结果莫名其妙少了几条」，而这和权限毫无关系。

本 Lab 的对策：

```c
int rcvbuf = 4 * 1024 * 1024;
setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
```

---

## 5. 真实输出 vs `ip -j`

以下左右两侧都是**本机实测**。左边是本 Lab 程序的输出，右边是把
`ip -j ...` 的 JSON 用 `python3` 展开成同构文本，便于逐字段对照。

### 5.1 `RTM_GETLINK` ↔ `ip -j link show`

`./rtnl_dump link`：

```
link  ifindex=1   ifname=lo               mtu=65536  operstate=UNKNOWN        type=0x0304
        address=00:00:00:00:00:00
link  ifindex=2   ifname=eth0           mtu=1500   operstate=DOWN           type=0x0001
        address=00:11:22:33:44:02
link  ifindex=3   ifname=eth1           mtu=1500   operstate=DOWN           type=0x0001
        address=00:11:22:33:44:01
link  ifindex=4   ifname=wlan0           mtu=1500   operstate=UP             type=0x0001
        address=00:11:22:33:44:05
link  ifindex=5   ifname=docker0          mtu=1500   operstate=DOWN           type=0x0001 kind=bridge
        address=00:11:22:33:44:03
link  ifindex=6   ifname=br-0123456789ab  mtu=1500   operstate=DOWN           type=0x0001 kind=bridge
        address=00:11:22:33:44:0a
```

`ip -j link show`（同机、同时刻）：

```
  1 lo               mtu=65536  operstate=UNKNOWN        address=00:00:00:00:00:00
  2 eth0           mtu=1500   operstate=DOWN           address=00:11:22:33:44:02
  3 eth1           mtu=1500   operstate=DOWN           address=00:11:22:33:44:01
  4 wlan0           mtu=1500   operstate=UP             address=00:11:22:33:44:05
  5 docker0          mtu=1500   operstate=DOWN           address=00:11:22:33:44:03
  6 br-0123456789ab  mtu=1500   operstate=DOWN           address=00:11:22:33:44:0a
```

字段对应关系：

| 本程序 | netlink 属性 | `ip -j` JSON 字段 |
|---|---|---|
| `ifindex` | `ifinfomsg.ifi_index` | `ifindex` |
| `ifname` | `IFLA_IFNAME` (3) | `ifname` |
| `mtu` | `IFLA_MTU` (4) | `mtu` |
| `operstate` | `IFLA_OPERSTATE` (16) | `operstate` |
| `address` | `IFLA_ADDRESS` (1) | `address` |
| `type` | `ifinfomsg.ifi_type`（`0x0001`=Ethernet，`0x0304`=loopback） | `link_type`（字符串） |
| `kind` | `IFLA_LINKINFO`(18) → `IFLA_INFO_KIND`(1) | `linkinfo.info_kind` |

> 注意 `ip -j link show` **默认不打印 `linkinfo`**，只有 `ip -d link show`
> 才会带出 `kind=bridge`；而内核在 dump 里本来就把 `IFLA_LINKINFO` 发下来了。

### 5.2 `RTM_GETADDR` ↔ `ip -j addr show`

`./rtnl_dump addr`：

```
addr  ifindex=1   ifname=lo               family=2  prefixlen=8   scope=host      local=127.0.0.1                label=lo flags=0x80
addr  ifindex=4   ifname=wlan0           family=2  prefixlen=24  scope=universe  local=10.0.0.10            label=wlan0 flags=0x00
addr  ifindex=5   ifname=docker0          family=2  prefixlen=16  scope=universe  local=172.17.0.1               label=docker0 flags=0x80
addr  ifindex=6   ifname=br-0123456789ab  family=2  prefixlen=16  scope=universe  local=172.18.0.1               label=br-0123456789ab flags=0x80
addr  ifindex=1   ifname=lo               family=10 prefixlen=128 scope=host      local=::1                      flags=0x80
addr  ifindex=4   ifname=wlan0           family=10 prefixlen=128 scope=universe  local=2408:8220:b20:d5c1::1b9  flags=0x00
addr  ifindex=4   ifname=wlan0           family=10 prefixlen=64  scope=universe  local=2408:8220:b20:d5c1:7a5e:a303:9260:f6ad flags=0x00
addr  ifindex=4   ifname=wlan0           family=10 prefixlen=64  scope=link      local=fe80::8c:5974:c0d1:e608  flags=0x80
```

`ip -j addr show`：

```
  1 lo               family=inet  prefixlen=8    scope=host      local=127.0.0.1
  1 lo               family=inet6 prefixlen=128  scope=host      local=::1
  4 wlan0           family=inet  prefixlen=24   scope=global    local=10.0.0.10
  4 wlan0           family=inet6 prefixlen=128  scope=global    local=2408:8220:b20:d5c1::1b9
  4 wlan0           family=inet6 prefixlen=64   scope=global    local=2408:8220:b20:d5c1:7a5e:a303:9260:f6ad
  4 wlan0           family=inet6 prefixlen=64   scope=link      local=fe80::8c:5974:c0d1:e608
  5 docker0          family=inet  prefixlen=16   scope=global    local=172.17.0.1
  6 br-0123456789ab  family=inet  prefixlen=16   scope=global    local=172.18.0.1
```

| 本程序 | netlink | `ip -j` JSON |
|---|---|---|
| `family=2/10` | `ifa_family`（`AF_INET=2`/`AF_INET6=10`） | `family`（`inet`/`inet6`） |
| `prefixlen` | `ifa_prefixlen`（结构体字段，**不是** `IFA_PREFIXLEN` 属性） | `prefixlen` |
| `scope` | `ifa_scope`（`RT_SCOPE_*`） | `scope` |
| `local` | `IFA_LOCAL`(2)，IPv6 回退 `IFA_ADDRESS`(1) | `local` |
| `label` | `IFA_LABEL`(3) | `label` |
| `flags` | `ifa_flags` | `flags`（数组形式） |

> **scope 的命名差异**：netlink 里 `RT_SCOPE_UNIVERSE`（0）对应 `ip -j` 的
> `"global"`。两者是同一个东西的两种叫法，对照时不要以为是 bug。

### 5.3 `RTM_GETROUTE` ↔ `ip -j route show`

`./rtnl_dump route`（**23 条**，因为它 dump 了所有 family、所有表）：

```
route dst=default                via=10.0.0.1     dev=wlan0           table=main     proto=dhcp     scope=universe type=unicast     metric=600 prefsrc=10.0.0.10 dst_len=0
route dst=172.17.0.0/16          via=-                dev=docker0          table=main     proto=kernel   scope=link     type=unicast     prefsrc=172.17.0.1 dst_len=16
route dst=172.18.0.0/16          via=-                dev=br-0123456789ab  table=main     proto=kernel   scope=link     type=unicast     prefsrc=172.18.0.1 dst_len=16
route dst=10.0.0.0/24        via=-                dev=wlan0           table=main     proto=kernel   scope=link     type=unicast     metric=600 prefsrc=10.0.0.10 dst_len=24
route dst=127.0.0.0/8            via=-                dev=lo               table=local    proto=kernel   scope=host     type=local       prefsrc=127.0.0.1 dst_len=8
route dst=127.0.0.1/32           via=-                dev=lo               table=local    proto=kernel   scope=host     type=local       prefsrc=127.0.0.1 dst_len=32
route dst=127.255.255.255/32     via=-                dev=lo               table=local    proto=kernel   scope=link     type=broadcast   prefsrc=127.0.0.1 dst_len=32
route dst=10.0.0.10/32       via=-                dev=wlan0           table=local    proto=kernel   scope=host     type=local       prefsrc=10.0.0.10 dst_len=32
route dst=10.0.0.255/32      via=-                dev=wlan0           table=local    proto=kernel   scope=link     type=broadcast   prefsrc=10.0.0.10 dst_len=32
route dst=2408:8220:b20:61::/64  via=-                dev=wlan0           table=main     proto=ra       scope=universe type=unicast     metric=600 dst_len=64
route dst=fe80::/64              via=-                dev=wlan0           table=main     proto=kernel   scope=universe type=unicast     metric=1024 dst_len=64
route dst=default                via=fe80::5aea:1fff:fec2:6464 dev=wlan0           table=main     proto=ra       scope=universe type=unicast     metric=600 dst_len=0
route dst=::1/128                via=-                dev=lo               table=local    proto=kernel   scope=universe type=local       dst_len=128
route dst=ff00::/8               via=-                dev=wlan0           table=local    proto=kernel   scope=universe type=multicast   metric=256 dst_len=8
```

（上面为节省篇幅省略了若干条 `type=local` 的 IPv6 单地址路由，完整列表见 `./rtnl_dump route`。）

`ip -j route show`（默认只看 **main** 表，所以只有 4 条）：

```
dst=default                via=10.0.0.1     dev=wlan0           table=main     proto=dhcp     type=unicast     metric=600 prefsrc=10.0.0.10
dst=172.17.0.0/16          via=-                dev=docker0          table=main     proto=kernel   type=unicast     metric= prefsrc=172.17.0.1
dst=172.18.0.0/16          via=-                dev=br-0123456789ab  table=main     proto=kernel   type=unicast     metric= prefsrc=172.18.0.1
dst=10.0.0.0/24        via=-                dev=wlan0           table=main     proto=kernel   type=unicast     metric=600 prefsrc=10.0.0.10
```

| 本程序 | netlink | `ip -j` JSON |
|---|---|---|
| `dst`/`dst_len` | `RTA_DST`(1) + `rtm_dst_len` | `dst`（`default` 表示无 RTA_DST 且 len=0） |
| `via` | `RTA_GATEWAY`(5) | `gateway` |
| `dev` | `RTA_OIF`(4) → `if_indextoname()` | `dev` |
| `metric` | `RTA_PRIORITY`(6) | `metric` |
| `prefsrc` | `RTA_PREFSRC`(7) | `prefsrc` |
| `table` | `RTA_TABLE`(15)，回退 `rtm_table` | `table` |
| `proto` | `rtm_protocol`（`RTPROT_*`） | `protocol` |
| `scope` | `rtm_scope`（`RT_SCOPE_*`） | `scope` |
| `type` | `rtm_type`（`RTN_*`） | `type` |

> **为什么条数不同**：`ip route show` 默认把请求限定在 `table main`，
> 而本 Lab 的 dump 请求 `rtm_table = 0`（`RT_TABLE_UNSPEC`）。
> 想两边完全对齐，请用 `ip route show table all`，或者在本程序里
> 填充 `RTA_TABLE` 把请求限定到 `RT_TABLE_MAIN`。

---

## 6. 实测踩到的两个真 bug（很有教学价值）

这两个 bug 都不是「想想就发现」的，而是靠**和 `ip` 的真实输出逐字段对照**才暴露的。
这正是本 Lab 要求做对照的原因。

### bug 1：IPv6 地址打印成 `-`

最初 `local` 只读 `IFA_LOCAL`。结果 IPv4 全对，**IPv6 全部打印 `-`**。
原因：内核在 IPv4 上填 `IFA_LOCAL` + `IFA_ADDRESS`，
而在 IPv6 上**常常只填 `IFA_ADDRESS`**，没有 `IFA_LOCAL`。

修复是加回退：

```c
if (!local)
        local = peer;   /* IPv6 没有 IFA_LOCAL，回退到 IFA_ADDRESS */
```

另一个语义细节：在点对点链路（TUN/PPP）上，`IFA_ADDRESS` 是**对端**地址、
`IFA_LOCAL` 才是本端 —— 这就是 `ip addr` 输出里 `local`/`peer` 的由来。

### bug 2：默认路由的网关打印成了 prefsrc

对照时发现本程序打印 `default via=10.0.0.10`，
而 `ip -j route show` 明确是 `via=10.0.0.1`。

根因是经典的 **C 静态缓冲区别名**：`addr_str()` 当时返回的是
指向函数内 `static char buf[]` 的指针。而在

```c
printf("via=%s prefsrc=%s", gw, prefsrc);
```

里 `gw` 和 `prefsrc` 指向**同一块内存**，最终两个 `%s` 都打印最后一个写入的值。

修复：缓冲区改由调用者提供，每个字段用独立局部数组：

```c
static const char *addr_str(int family, const void *data, size_t len,
                            char *buf, size_t buflen);
...
char gbuf[INET6_ADDRSTRLEN], pbuf[INET6_ADDRSTRLEN];
gw      = addr_str(..., gbuf, sizeof gbuf);
prefsrc = addr_str(..., pbuf, sizeof pbuf);
```

修复后实测：`route dst=default via=10.0.0.1 ... prefsrc=10.0.0.10`
—— 与 `ip` 完全一致。

> 教训：**任何返回 `char *` 的函数都要明确缓冲区归属**。
> 在同一个 `printf` 里放两个「返回静态缓冲区的函数调用」是定时炸弹。

---

## 7. 为什么配置面都在往 netlink 迁移

历史上有四条配置通道，如今重心完全倒向 netlink：

| 通道 | 典型代表 | 主要问题 |
|---|---|---|
| `ioctl(SIOCxxx)` | `SIOCGIFADDR`、`SIOCSIFMTU` | 参数是**定长 C 结构体**，改不了、加不了字段；`struct ifreq` 只有 16 字节名字段；一次只能操作一个对象；`SIOCGIFCONF` 的缓冲区大小竞态是著名的历史坑 |
| `/proc/net/*` | `/proc/net/route`、`/proc/net/arp` | 文本格式、**无类型**、难以扩展；只读为主；字段顺序被脚本硬编码后就无法演进 |
| sysfs | `/sys/class/net/*/` | 一个文件一个值，**没有事务性**，读多个值之间状态可能已变；写也是逐个生效 |
| **netlink** | `RTM_*`、`ethtool` netlink、`nft`、`ss` | 见下 |

netlink 胜出的具体原因：

1. **TLV 可扩展**：加属性只需分配新的 `rta_type`，老程序自动忽略未知属性，
   **前向兼容**。这是 ioctl 定长结构体做不到的。
2. **批量与原子性**：一次请求可以 dump 整个路由表；`NLM_F_*` 与
   `nlmsg_seq` 支持事务化/确认语义（`NLM_F_ACK`、`NLM_F_CREATE|NLM_F_REPLACE`）。
   `RTM_NEWLINK` 甚至能在一条消息里创建并配置一个虚拟设备（`IFLA_LINKINFO`）。
3. **命名空间天然友好**：netlink 套接字属于某个 netns，
   查询结果自动限定在调用者所在的命名空间，不需要额外传递 netns 标识。
4. **双向、异步、可订阅**：`RTMGRP_LINK`/`RTMGRP_IPV4_ROUTE` 等多播组
   让用户态能收到「链路状态变化」事件（`ip monitor` 就是订阅者）。
   ioctl 完全没有事件机制。
5. **结构化类型与二进制安全**：地址是二进制、长度由属性自带，
   不受文本解析的坑（locale、字段顺序、转义）影响。
6. **一套通用传输**：`NETLINK_ROUTE`、`NETLINK_NETFILTER`、
   `NETLINK_GENERIC`（`genl`，ethtool 的新接口走这里）共用同一套
   头部/对齐/多部分语义，内核与用户态都能复用解析与框架代码。

**边界与代价**（也要知道）：
- netlink 原始接口很啰嗦（本 Lab 200 多行才做了三个查询），
  所以才会有 libnl / `pyroute2` 这类封装；
- 属性嵌套可能很深，解析器必须防御畸形输入；
- 大 dump 有 `ENOBUFS` 丢消息风险（见 §4）；
- 简单单值查询（如读 MTU）用 `ioctl` 反而更短，所以 ioctl 不会立刻消失，
  但**新增能力基本只走 netlink**。

---

## 8. 自测题

<details>
<summary>1. `NLMSG_LENGTH(n)` 和 `NLMSG_SPACE(n)` 有什么区别？</summary>

`NLMSG_LENGTH(n)` = `n + NLMSG_HDRLEN`，是写进 `nlmsg_len` 字段的
「头+负载」长度；`NLMSG_SPACE(n)` = `NLMSG_ALIGN(NLMSG_LENGTH(n))`，
是把尾部 padding 也算上的**实际占用字节数**，用于分配缓冲区和 `sendto()`。
当 `NLMSG_LENGTH(n)` 已是 4 的倍数时两者相等（例如 `n=16` 时都是 32）。
</details>

<details>
<summary>2. 收到 `NLMSG_ERROR` 就一定是失败吗？</summary>

不是。`struct nlmsgerr.error == 0` 表示 **ACK**（成功确认），
通常在请求带 `NLM_F_ACK` 时出现。只有 `error != 0` 才是失败，
且其值是**负的 errno**，应打印 `strerror(-e->error)`。
</details>

<details>
<summary>3. 为什么 dump 循环不能靠 `NLM_F_MULTI` 判断结束？</summary>

因为多部分回复的**最后一条消息不带 `NLM_F_MULTI`**，
但更可靠的结束信号是内核显式发送的 `NLMSG_DONE`。
用 `NLM_F_MULTI` 判断在跨 datagram 边界时容易误判。
</details>

<details>
<summary>4. `parse_rtattr()` 里为什么必须先判断 `rta_type <= max`？</summary>

`rta_type` 来自网络/内核消息，属于**不可信输入**。若直接
`tb[rta->rta_type] = rta`，一个畸形的大 `rta_type` 就会越界写 `tb[]`，
造成内存破坏。先判上界是解析 TLV 的基本纪律。
</details>

<details>
<summary>5. netlink 里 `RT_SCOPE_UNIVERSE` 对应 `ip -j` 的什么？</summary>

对应 `"global"`。二者是同一语义的不同命名：
netlink/UAPI 用 `RT_SCOPE_*`，`ip -j` 在 JSON 里把它转成
`global`/`link`/`host`。默认路由没有显式 scope 时 `ip -j` 会省略该字段。
</details>

<details>
<summary>6. 为什么 `./rtnl_dump route` 比 `ip route show` 条数多？</summary>

因为本程序的 dump 请求里 `rtm_table = RT_TABLE_UNSPEC`(0) 且
`rtm_family = AF_UNSPEC`，内核会把**所有 family、所有路由表**
（含 `local` 表、IPv6 路由）都返回；而 `ip route show` 默认只看
`main` 表。用 `ip route show table all` 才能对齐。
</details>

<details>
<summary>7. dump 结果少了若干条，最可能的原因是什么？</summary>

`ENOBUFS` —— netlink socket 接收队列溢出导致内核丢消息。
dump 是内核一次性推大量消息，用户态读得慢或 `SO_RCVBUF` 太小就会丢。
对策是调大 `SO_RCVBUF`，并尽快把消息从队列里取走。
这与权限无关。
</details>

<details>
<summary>8. 为什么 `RTM_GETLINK` 比 `RTM_NEWLINK` 大 2？</summary>

netlink 把「读」复用了「写」的编号布局：`NEW=16`/`GET=18`、
`NEWADDR=20`/`GETADDR=22`、`NEWROUTE=24`/`GETROUTE=26`。
内核用同一套 `rtnl_register` 表处理请求，`doit`（单对象）与
`dumpit`（批量）分开实现。
</details>

<details>
<summary>9. 为什么 `IFA_ADDRESS` 与 `IFA_LOCAL` 会不同？</summary>

在点对点链路（TUN、PPP）上，`IFA_ADDRESS` 是**对端**地址，
`IFA_LOCAL` 是**本端**地址；在普通广播链路上两者通常相同。
这也是 `ip addr` 输出 `local`/`peer` 的来源。
</details>

---

## 9. 文件清单

| 文件 | 作用 |
|---|---|
| `rtnl.h` | 公共接口：socket 管理、dump 循环、TLV 解析、枚举转名 |
| `rtnl.c` | 传输层（`nl_open`/`nl_dump`）+ 通用属性解析器 + 枚举表 |
| `rtnl_dump.c` | 三个回调（link/addr/route）+ `main` 分发 |
| `Makefile` | `all` / `clean` / `run` |

## 10. 延伸阅读

- `man 7 netlink`、`man 7 rtnetlink` —— 权威格式说明。
- `man 3 rtnetlink` —— 属性与消息类型的完整列表。
- 内核 UAPI 头（本机可直接读）：
  `/usr/include/linux/netlink.h`、`linux/rtnetlink.h`、
  `linux/if_link.h`、`linux/if_addr.h`。
  注意 UAPI 头里的常量就是 ABI，**不要凭记忆写数字**，用宏。
- 内核实现：`net/core/rtnetlink.c`（`rtnetlink_rcv_msg`、`rtnl_dump_ifinfo`）、
  `net/ipv4/fib_frontend.c`（`inet_dump_fib`、`inet_rtm_getroute`）。
  以 6.12 为准，可在 <https://elixir.bootlin.com/linux/v6.12/source/> 核对。
- `iproute2` 源码 `lib/libnetlink.c`（`rtnl_dump_request`、`rtnl_talk`）与
  `ip/iproute.c`、`ip/ipaddress.c` 的 `parse_rtattr`，与本 Lab 写法同源。
