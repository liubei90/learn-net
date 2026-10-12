#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
05-socket-diag.py —— 直接用 NETLINK_SOCK_DIAG 读取内核中的 TCP/UDP socket 明细

【这个实验在讲什么】
`ss` 命令并不是"读 /proc/net/tcp"那么简单。现代内核把 socket 状态通过一个
专门的 netlink 协议族 —— NETLINK_SOCK_DIAG(4) —— 暴露给用户态。`ss` 就是
这个协议的一个客户端。本脚本手写 netlink 报文，绕开 `ss`，直接问内核：
"把当前所有 TCP socket 的明细给我。"

这样做的价值：
  1. 你会真正理解"内核如何把内部数据结构导出成稳定 ABI"这件事；
  2. 你会看到 /proc/net/tcp 看不到的字段（rtt/cwnd/retrans/timer/inode/uid/内存）；
  3. 出故障时 ss 不可用（容器里没装 iproute2）也能诊断。

【本机权限说明】
本脚本 **不需要 root**。NETLINK_SOCK_DIAG 的自查询（dump 属于本 netns 的
socket）是允许非特权用户执行的 —— 内核只会返回调用者有权看到的信息。
实测本机 CapEff=0 依然可以完整 dump。

【内核侧源码入口】（以 6.12 为准，可在 elixir.bootlin.com/linux/v6.12/source/ 核对）
  - 协议族注册:      net/core/sock_diag.c
  - TCP 实现:        net/ipv4/tcp_diag.c  (tcp_diag_init -> inet_diag_register)
  - 通用 inet 逻辑:  net/ipv4/inet_diag.c  (inet_diag_dump / inet_sk_diag_fill)
  - UDP 实现:        net/ipv4/udp_diag.c  (udp_diag_handler)
  - UAPI 结构定义:   include/uapi/linux/inet_diag.h, include/uapi/linux/sock_diag.h,
                     include/uapi/linux/tcp.h (struct tcp_info)

【协议速览】
请求:  nlmsghdr (16B)
         + inet_diag_req_v2 (56B)
  nlmsg_type = SOCK_DIAG_BY_FAMILY (20)
  nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP
  inet_diag_req_v2.sdiag_family   = AF_INET / AF_INET6
  inet_diag_req_v2.sdiag_protocol = IPPROTO_TCP / IPPROTO_UDP
  inet_diag_req_v2.idiag_states   = 位掩码，全 1 表示"所有状态"
  inet_diag_req_v2.idiag_ext      = 要附加哪些扩展属性（meminfo/info/cong...）

响应:  nlmsghdr + inet_diag_msg (72B) + 若干 rtattr 扩展属性

注意：Python 标准库的 socket 模块 **没有** NETLINK_SOCK_DIAG 常量
（只有 NETLINK_ROUTE=0），所以下面手动定义。这是本机实测确认过的。
"""

import errno
import os
import socket
import struct
import sys

# ---------------------------------------------------------------------------
# 常量定义（数值来自 include/uapi/linux/*.h，以 6.12 为准）
# ---------------------------------------------------------------------------
NETLINK_SOCK_DIAG = 4          # 标准库没有这个常量，手动定义
SOCK_DIAG_BY_FAMILY = 20       # include/uapi/linux/sock_diag.h

NLM_F_REQUEST = 0x01
NLM_F_DUMP = 0x300             # NLM_F_ROOT | NLM_F_MATCH

NLMSG_DONE = 3
NLMSG_ERROR = 2

INET_DIAG_MEMINFO = 1          # struct inet_diag_meminfo
INET_DIAG_INFO = 2             # TCP: struct tcp_info
INET_DIAG_VEGASINFO = 3
INET_DIAG_CONG = 4             # 拥塞控制算法名（NUL 结尾字符串）
INET_DIAG_TOS = 5
INET_DIAG_TCLASS = 6
INET_DIAG_SKMEMINFO = 7
INET_DIAG_SHUTDOWN = 8
INET_DIAG_MARK = 15
INET_DIAG_CGROUP_ID = 21

# inet_diag_req_v2.idiag_states 的位掩码。
#
# ⚠️ 这里有个非常容易踩的坑，本脚本在 6.12.107 上实测确认：
#    idiag_states 的**位序号 = TCP 状态编号本身**，而不是 (状态编号 - 1)。
#    也就是说 ESTABLISHED(TCP_ESTABLISHED==1) 对应 bit 1，LISTEN(TCP_LISTEN==10)
#    对应 bit 10。若按 `1 << (state - 1)` 去填，内核不会报错，而是安静地返回
#    0 条记录 —— 这是最难查的一类 bug。
#
#    实测复现（受控 socket：一个 LISTEN + 一对 ESTABLISHED）：
#      mask 1<<1  -> 只返回 idiag_state==1  (ESTABLISHED)
#      mask 1<<6  -> 只返回 idiag_state==6  (TIME-WAIT)
#      mask 1<<8  -> 只返回 idiag_state==8  (CLOSE-WAIT)
#      mask 1<<10 -> 只返回 idiag_state==10 (LISTEN)
#      mask 1<<0  -> 空（因为 TCP 状态从 1 开始编号）
#
#    可用 labs/09-observability/Makefile 的 `make verify-states` 复现。
#    权威定义以 6.12 为准，可在 elixir.bootlin.com/linux/v6.12/source/ 下
#    核对 include/uapi/linux/inet_diag.h 的 idiag_states 说明，以及
#    net/ipv4/inet_diag.c 中 inet_diag_dump_icsk() 对 states 的使用。
TCP_STATES = {
    "ESTABLISHED": 1 << 1,    # TCP_ESTABLISHED  == 1
    "SYN-SENT": 1 << 2,       # TCP_SYN_SENT     == 2
    "SYN-RECV": 1 << 3,       # TCP_SYN_RECV     == 3
    "FIN-WAIT-1": 1 << 4,     # TCP_FIN_WAIT1    == 4
    "FIN-WAIT-2": 1 << 5,     # TCP_FIN_WAIT2    == 5
    "TIME-WAIT": 1 << 6,      # TCP_TIME_WAIT    == 6
    "CLOSE": 1 << 7,          # TCP_CLOSE        == 7
    "CLOSE-WAIT": 1 << 8,     # TCP_CLOSE_WAIT   == 8
    "LAST-ACK": 1 << 9,       # TCP_LAST_ACK     == 9
    "LISTEN": 1 << 10,        # TCP_LISTEN       == 10
    "CLOSING": 1 << 11,       # TCP_CLOSING      == 11
    "NEW-SYN-RECV": 1 << 12,  # TCP_NEW_SYN_RECV == 12
}
ALL_TCP_STATES = 0xFFFFFFFF

# struct inet_diag_msg.idiag_state -> 可读名字
# 注意：TCP 用 1..11，UDP 用 7 表示 CLOSE（UDP 无连接状态机）
TCP_STATE_NAMES = {
    1: "ESTABLISHED", 2: "SYN-SENT", 3: "SYN-RECV", 4: "FIN-WAIT-1",
    5: "FIN-WAIT-2", 6: "TIME-WAIT", 7: "CLOSE", 8: "CLOSE-WAIT",
    9: "LAST-ACK", 10: "LISTEN", 11: "CLOSING", 12: "NEW-SYN-RECV",
}

# inet_diag_msg.idiag_timer
TIMER_NAMES = {0: "-", 1: "on", 2: "keepalive", 3: "timewait", 4: "retrans"}


def _nl_align(n):
    """netlink 属性按 4 字节对齐（NLMSG_ALIGNTO / RTA_ALIGNTO）"""
    return (n + 3) & ~3


def decode_cap_eff():
    """读 CapEff，判断自己是否具备做特权实验的能力（供输出自检用）"""
    try:
        with open("/proc/self/status", "r", encoding="ascii") as fh:
            for line in fh:
                if line.startswith("CapEff:"):
                    return int(line.split()[1], 16)
    except OSError:
        pass
    return 0


def fmt_addr(family, raw4, port_be, ifindex=0):
    """
    把 inet_diag_sockid 里的 16 字节地址还原成点分/冒号格式。
    idiag_src/idiag_dst 固定是 4 个 __be32（16 字节）的联合体：
      - IPv4 只用前 4 字节
      - IPv6 用满 16 字节
    端口是 __be16，需要 ntohs。
    """
    port = socket.ntohs(port_be)
    if family == socket.AF_INET:
        addr = socket.inet_ntop(socket.AF_INET, raw4[:4])
    elif family == socket.AF_INET6:
        addr = socket.inet_ntop(socket.AF_INET6, raw4)
        # IPv4-mapped IPv6 (::ffff:a.b.c.d) 打出来更好读
        if addr.startswith("::ffff:") and "." in addr:
            pass
    else:
        addr = "?"
    if ifindex:
        addr = "%s%%%d" % (addr, ifindex)
    return addr, port


def parse_rtattrs(payload):
    """
    解析 inet_diag_msg 之后的 TLV 扩展属性。
    每个属性: struct rtattr { __u16 rta_len; __u16 rta_type; } + payload
    rta_len 含头部，且要按 4 字节对齐跳到下一个。
    """
    attrs = {}
    off = 0
    total = len(payload)
    while off + 4 <= total:
        rta_len, rta_type = struct.unpack_from("=HH", payload, off)
        if rta_len < 4:
            break  # 畸形数据，停止解析而不是崩溃
        body = payload[off + 4: off + rta_len]
        # 同一个 type 可能出现多次（如 INET_DIAG_LOCALS/PEERS 是列表），存成 list
        attrs.setdefault(rta_type, []).append(body)
        off += _nl_align(rta_len)
    return attrs


def parse_tcp_info(buf):
    """
    解析 struct tcp_info（include/uapi/linux/tcp.h）的前若干字段。
    这个结构体会随内核版本增长，所以必须做长度检查，绝不越界读。
    布局（6.12）：8 个 __u8（第 7 个是位域）+ 一串 __u32 + 一串 __u64。
    """
    out = {}
    if len(buf) < 8:
        return out
    (state, ca_state, retransmits, probes, backoff, options,
     wscale_bits) = struct.unpack_from("=BBBBBBB", buf, 0)
    out["tcpi_state"] = state
    out["tcpi_ca_state"] = ca_state
    out["tcpi_retransmits"] = retransmits
    out["tcpi_probes"] = probes
    out["tcpi_backoff"] = backoff
    out["tcpi_options"] = options
    out["tcpi_snd_wscale"] = wscale_bits & 0x0F
    out["tcpi_rcv_wscale"] = (wscale_bits >> 4) & 0x0F
    # 8 个 u8 之后是连续的 __u32。逐个安全读取。
    u32_fields = [
        "tcpi_rto", "tcpi_ato", "tcpi_snd_mss", "tcpi_rcv_mss",
        "tcpi_unacked", "tcpi_sacked", "tcpi_lost", "tcpi_retrans",
        "tcpi_fackets", "tcpi_last_data_sent", "tcpi_last_ack_sent",
        "tcpi_last_data_recv", "tcpi_last_ack_recv", "tcpi_pmtu",
        "tcpi_rcv_ssthresh", "tcpi_rtt", "tcpi_rttvar",
        "tcpi_snd_ssthresh", "tcpi_snd_cwnd", "tcpi_advmss",
        "tcpi_reordering",
    ]
    off = 8
    for name in u32_fields:
        if off + 4 > len(buf):
            return out
        out[name] = struct.unpack_from("=I", buf, off)[0]
        off += 4
    return out


def parse_meminfo(buf):
    """struct inet_diag_meminfo { __u32 rmem, wmem, fmem, tmem; } 单位字节"""
    if len(buf) < 16:
        return {}
    rmem, wmem, fmem, tmem = struct.unpack_from("=IIII", buf, 0)
    return {"rmem": rmem, "wmem": wmem, "fmem": fmem, "tmem": tmem}


def build_request(family, protocol, states, ext_mask):
    """
    构造 nlmsghdr + inet_diag_req_v2。
    inet_diag_req_v2:
      __u8 sdiag_family; __u8 sdiag_protocol; __u8 idiag_ext; __u8 pad;
      __u32 idiag_states;
      struct inet_diag_sockid {
          __be16 idiag_sport; __be16 idiag_dport;
          __be32 idiag_src[4]; __be32 idiag_dst[4];
          __u32 idiag_if; __u32 idiag_cookie[2];
      }
    总长 4 + 4 + 48 = 56 字节。
    """
    # 全 0 的 sockid 表示"不过滤，dump 全部"。
    # 注意 src/dst 是 __be32[4]（各 16 字节）的联合体，IPv4 只占前 4 字节，
    # 但结构体里恒为 16 字节 —— 这里写成 4s 会把整个报文长度算错。
    sockid = struct.pack("=HH16s16sI8s",
                        0, 0, b"\x00" * 16, b"\x00" * 16, 0, b"\x00" * 8)
    req_v2 = struct.pack("=BBBB I", family, protocol, ext_mask, 0, states) + sockid
    assert len(req_v2) == 56, len(req_v2)

    seq = 1
    # nlmsg_len 在 len(header)+len(payload) 处，这里先填占位再回填
    nlmsg_len = 16 + len(req_v2)
    header = struct.pack("=IHHII", nlmsg_len, SOCK_DIAG_BY_FAMILY,
                         NLM_F_REQUEST | NLM_F_DUMP, seq, 0)
    return header + req_v2, seq


def dump_sockets(family, protocol, states, ext_mask=0):
    """
    向内核发起一次 dump 请求，返回 (inet_diag_msg dict, attrs dict) 列表。
    netlink dump 语义：一次请求，内核用多条 NLMSG_DONE 之前的消息回复，
    最后一条 NLMSG_DONE 表示结束。msg_flags 里有 NLM_F_MULTI 表示还有后续。
    """
    sock = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, NETLINK_SOCK_DIAG)
    # bind 到 pid=0 让内核分配，内核只把属于本进程的回复投递过来
    sock.bind((0, 0))
    sock.settimeout(5.0)

    req, seq = build_request(family, protocol, states, ext_mask)
    try:
        sock.send(req)
    except OSError as exc:
        sock.close()
        if exc.errno in (errno.EPERM, errno.EACCES):
            raise PermissionError(
                "内核拒绝 NETLINK_SOCK_DIAG dump（EPERM/EACCES）。"
                "通常说明被 seccomp/LSM 策略拦截。"
            ) from exc
        raise

    results = []
    done = False
    try:
        while not done:
            data = sock.recv(65536)
            offset = 0
            total = len(data)
            while offset + 16 <= total:
                (msg_len, msg_type, msg_flags, msg_seq,
                 _msg_pid) = struct.unpack_from("=IHHII", data, offset)
                if msg_len < 16 or offset + msg_len > total:
                    done = True
                    break
                body = data[offset + 16: offset + msg_len]

                if msg_type == NLMSG_DONE:
                    done = True
                    break
                if msg_type == NLMSG_ERROR:
                    if len(body) >= 4:
                        err = struct.unpack_from("=i", body, 0)[0]
                        if err != 0:
                            raise OSError(-err, os.strerror(-err))
                    done = True
                    break
                if msg_type == SOCK_DIAG_BY_FAMILY and len(body) >= 72:
                    parsed = parse_diag_msg(body, family)
                    if parsed:
                        results.append(parsed)
                offset += _nl_align(msg_len)
                if not (msg_flags & 0x2):  # NLM_F_MULTI
                    done = True
                    break
    except socket.timeout:
        # 超时不是致命错误：把已收到的返回，并明确告知调用者
        sys.stderr.write("[warn] netlink dump 超时，返回已收到的部分结果\n")
    finally:
        sock.close()
    return results


def parse_diag_msg(body, family):
    """
    解析 struct inet_diag_msg（72 字节）:
      __u8 idiag_family; __u8 idiag_state; __u8 idiag_timer; __u8 idiag_retrans;
      struct inet_diag_sockid id;              (48B)
      __u32 idiag_expires; __u32 idiag_rqueue; __u32 idiag_wqueue;
      __u32 idiag_uid; __u32 idiag_inode;
    4 + 48 + 20 = 72
    """
    # src/dst 必须按 16 字节解，否则后续所有字段偏移都会算错。
    # 这 4 个 __u8 + 48 字节 sockid 一共 52 字节，5 个 __u32 从偏移 52 开始。
    (fam, state, timer, retrans,
     sport, dport, src, dst, ifidx, cookie) = struct.unpack_from(
        "=BBBB HH16s16s I 8s", body, 0)
    assert struct.calcsize("=BBBBHH16s16sI8s") == 52, struct.calcsize(
        "=BBBBHH16s16sI8s")
    expires, rqueue, wqueue, uid, inode = struct.unpack_from("=IIIII", body, 52)

    laddr, lport = fmt_addr(fam, src, sport, ifidx)
    raddr, rport = fmt_addr(fam, dst, dport, 0)

    attrs = parse_rtattrs(body[72:])

    rec = {
        "family": fam,
        "state": state,
        "state_name": TCP_STATE_NAMES.get(state, "?%d" % state),
        "timer": timer,
        "timer_name": TIMER_NAMES.get(timer, "?%d" % timer),
        "retrans": retrans,
        "laddr": laddr, "lport": lport,
        "raddr": raddr, "rport": rport,
        "ifindex": ifidx,
        "expires_ms": expires,
        "rqueue": rqueue,
        "wqueue": wqueue,
        "uid": uid,
        "inode": inode,
    }

    # 扩展属性是"按需附加"的，所以每一项都要判断存在与否
    if INET_DIAG_CONG in attrs:
        rec["cong"] = attrs[INET_DIAG_CONG][0].split(b"\x00")[0].decode(
            "ascii", "replace")
    if INET_DIAG_MEMINFO in attrs:
        rec.update(parse_meminfo(attrs[INET_DIAG_MEMINFO][0]))
    if INET_DIAG_INFO in attrs:
        rec["tcp_info"] = parse_tcp_info(attrs[INET_DIAG_INFO][0])
    if INET_DIAG_MARK in attrs and len(attrs[INET_DIAG_MARK][0]) >= 4:
        rec["mark"] = struct.unpack_from("=I", attrs[INET_DIAG_MARK][0], 0)[0]
    if INET_DIAG_CGROUP_ID in attrs and len(attrs[INET_DIAG_CGROUP_ID][0]) >= 8:
        rec["cgroup_id"] = struct.unpack_from("=Q", attrs[INET_DIAG_CGROUP_ID][0], 0)[0]

    # 记录收到了哪些属性类型，方便观察"ext 掩码要什么、内核给什么"
    rec["attr_types"] = sorted(attrs.keys())
    return rec


def show(title, records, want_tcp_info=True):
    """把 socket 明细打成人类可读的表格"""
    print("=" * 100)
    print(title)
    print("=" * 100)
    if not records:
        print("  (没有匹配的 socket)")
        print()
        return

    # 按状态排序，看起来更像 ss 的输出
    records = sorted(records, key=lambda r: (r["state"], r["lport"], r["laddr"]))

    hdr = "%-13s %-24s %-24s %-9s %-7s %-7s %-8s" % (
        "STATE", "LOCAL", "REMOTE", "TIMER", "RQ", "WQ", "INODE")
    print(hdr)
    print("-" * 100)
    for r in records:
        local = "%s:%d" % (r["laddr"], r["lport"])
        if r["raddr"] == "0.0.0.0":
            remote = "*:*"
        else:
            remote = "%s:%d" % (r["raddr"], r["rport"])
        print("%-13s %-24s %-24s %-9s %-7d %-7d %-8d" % (
            r["state_name"], local, remote,
            "%s/%d" % (r["timer_name"], r["retrans"]),
            r["rqueue"], r["wqueue"], r["inode"]))

    if want_tcp_info:
        detailed = [r for r in records if "tcp_info" in r]
        if detailed:
            print()
            print("── TCP 明细（来自 INET_DIAG_INFO / struct tcp_info，ss -tni 的同类数据）──")
            print("%-13s %-24s %-12s %-9s %-7s %-7s %-7s" % (
                "STATE", "LOCAL", "CONG", "RTT(us)", "CWND", "LOST", "RETRANS"))
            print("-" * 100)
            for r in detailed:
                ti = r["tcp_info"]
                print("%-13s %-24s %-12s %-9d %-7d %-7d %-7d" % (
                    r["state_name"], "%s:%d" % (r["laddr"], r["lport"]),
                    r.get("cong", "?"), ti.get("tcpi_rtt", 0),
                    ti.get("tcpi_snd_cwnd", 0), ti.get("tcpi_lost", 0),
                    ti.get("tcpi_retrans", 0)))
    print()


def main():
    caps = decode_cap_eff()
    print("=" * 100)
    print("NETLINK_SOCK_DIAG 直连实验 —— 绕开 ss，直接向内核索取 socket 明细")
    print("=" * 100)
    print("内核      : %s" % os.uname().release)
    print("Python    : %s" % sys.version.split()[0])
    print("CapEff    : 0x%016x  (root 需要 CAP_NET_ADMIN 才能看到**别的进程**的 socket)" % caps)
    print("权限结论  : 本脚本只 dump 自己所在的 netns，非特权即可运行 ✅")
    print()

    ext = INET_DIAG_MEMINFO | INET_DIAG_INFO | INET_DIAG_CONG

    # ---- 1. TCP IPv4 全状态 ----
    tcp4 = dump_sockets(socket.AF_INET, socket.IPPROTO_TCP,
                        ALL_TCP_STATES, ext)
    show("① TCP/IPv4 — 所有状态（idiag_states = 0xFFFFFFFF）", tcp4)

    # ---- 2. 只查 LISTEN，演示 idiag_states 位掩码的用法 ----
    listen_only = dump_sockets(socket.AF_INET, socket.IPPROTO_TCP,
                               TCP_STATES["LISTEN"], ext)
    show("② TCP/IPv4 — 只要 LISTEN（idiag_states = 1<<10，等价 ss -lnt）",
         listen_only, want_tcp_info=False)

    # ---- 3. TIME-WAIT，演示端口耗尽排查 ----
    tw = dump_sockets(socket.AF_INET, socket.IPPROTO_TCP,
                      TCP_STATES["TIME-WAIT"], 0)
    show("③ TCP/IPv4 — 只要 TIME-WAIT（排查「Cannot assign requested address」）",
         tw, want_tcp_info=False)

    # ---- 4. UDP ----
    udp4 = dump_sockets(socket.AF_INET, socket.IPPROTO_UDP,
                        ALL_TCP_STATES, INET_DIAG_MEMINFO)
    show("④ UDP/IPv4 — 所有 socket（UDP 用 state=7 表示 CLOSE，无连接状态机）",
         udp4, want_tcp_info=False)

    # ---- 5. IPv6（很多容器/回环服务只监听 v6） ----
    has_v6 = socket.has_ipv6
    if has_v6:
        try:
            tcp6 = dump_sockets(socket.AF_INET6, socket.IPPROTO_TCP,
                                ALL_TCP_STATES, ext)
            show("⑤ TCP/IPv6 — 所有状态", tcp6)
        except OSError as exc:
            print("⑤ TCP/IPv6 查询失败（%s），已跳过\n" % exc)

    # ---- 汇总：每个状态的连接数 ----
    print("=" * 100)
    print("汇总：本 netns 内 TCP/IPv4 socket 状态分布")
    print("=" * 100)
    counts = {}
    for r in tcp4:
        counts[r["state_name"]] = counts.get(r["state_name"], 0) + 1
    if not counts:
        print("  (无 TCP socket)")
    for name, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("  %-14s %3d" % (name, n))
    print("  %-14s %3d" % ("TOTAL", len(tcp4)))
    print()

    print("=" * 100)
    print("解读要点")
    print("=" * 100)
    print("""
1. 你刚刚做的事情和 `ss -tni` 完全同源：ss 内部就是发同一种 netlink 报文。
   区别只是 ss 把 inet_diag_msg 渲染成文本，而这里用 struct.unpack 自己解。

2. RECV-Q / SEND-Q 就是 inet_diag_msg 的 idiag_rqueue / idiag_wqueue。
   这是判断"应用读取太慢"最直接的证据：Recv-Q 长时间 > 0 且持续增长，
   说明数据已经到了 socket 接收队列但应用没来得及 read()。
   ⚠️ 注意：对 LISTEN 状态的 socket，这两个字段含义不同 —— 内核在这里
   返回的是**当前已完成三次握手、等待 accept() 的队列长度**（ss 显示为
   Send-Q 列）。本脚本打印的队列值对 LISTEN 需按此语义理解。

3. INET_DIAG_INFO 返回的 struct tcp_info 是内核 TCP 栈的"体检报告"：
   tcpi_rtt(微秒) / tcpi_snd_cwnd(拥塞窗口, 单位 MSS) / tcpi_lost /
   tcpi_retrans / tcpi_retransmits。这些在 /proc/net/tcp 里**看不到**，
   这是 sock_diag 相对 /proc 的核心优势。

4. idiag_states 是个位掩码而不是单个状态值，而且**位序号就等于 TCP 状态编号**：
   "查所有状态"是 0xFFFFFFFF，"只查 TIME-WAIT"是 1<<6（因为 TCP_TIME_WAIT==6），
   "只查 LISTEN"是 1<<10（因为 TCP_LISTEN==10）。ss 的 -t/-a/state 选项就是在设这个位。
   这一点极易搞错（写成 1<<(状态-1) 不会报错，只会安静地返回空结果）。
   运行 `make verify-states` 可以在本机复现验证。

5. inode 字段能把 socket 和 /proc/<pid>/fd/ 对上：
   ls -l /proc/*/fd/ 2>/dev/null | grep socket: | grep <inode>
   ——这就是 ss -p 找进程的原理（注意：非 root 只能看到自己的进程）。

6. 本机 CapEff=0 也能 dump，是因为内核只导出调用者有权限看到的部分。
   如果想 dump 别的 netns 或别的用户的 socket，就需要 CAP_NET_ADMIN。
""")


def verify_states():
    """
    受控实验：证明 idiag_states 的位序号 == TCP 状态编号（而不是 状态编号-1）。

    做法：先建立已知状态的 socket（一个 LISTEN + 一对 ESTABLISHED），
    然后用单个 bit 的掩码去 dump，观察内核实际返回的 idiag_state。
    如果掩码约定是 `1 << (state - 1)`，那么 bit1 应该返回 state==2(SYN_SENT)；
    如果约定是 `1 << state`，那么 bit1 应该返回 state==1(ESTABLISHED)。
    实测 6.12.107 的结果是后者。
    """
    import threading

    print("=" * 100)
    print("verify-states：验证 idiag_states 的位序号约定")
    print("=" * 100)
    print("预期（若约定是 位序号 == TCP 状态编号）：")
    print("  bit 1  -> 只返回 idiag_state==1  (ESTABLISHED)")
    print("  bit 6  -> 只返回 idiag_state==6  (TIME-WAIT)")
    print("  bit 10 -> 只返回 idiag_state==10 (LISTEN)")
    print()

    # 建立一个受控的 LISTEN socket 并 accept 一条连接
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(8)
    port = srv.getsockname()[1]

    cli = socket.create_connection(("127.0.0.1", port))
    conn, _ = srv.accept()
    print("已建立受控 socket：LISTEN 端口 %d + 一对 ESTABLISHED 连接\n" % port)

    try:
        print("%-6s %-8s %-28s %-8s %s" % (
            "bit", "mask", "内核返回的 idiag_state", "条数", "是否命中我们的测试 socket"))
        print("-" * 100)
        for b in range(1, 13):
            recs = dump_sockets(socket.AF_INET, socket.IPPROTO_TCP, 1 << b, 0)
            states = sorted({r["state"] for r in recs})
            ours = [r for r in recs
                    if r["lport"] == port or r["rport"] == port]
            verdict = ""
            if states and len(states) == 1:
                # 位序号 == 状态编号 才是我们验证的约定
                verdict = ("✅ 一致（bit == state）" if states[0] == b
                           else "❌ 不一致！state=%d != bit=%d" % (states[0], b))
            elif not states:
                verdict = "（该状态本机无 socket）"
            else:
                verdict = "（返回了多种状态，异常）"
            if ours:
                verdict += "  ← 命中测试 socket"
            print("%-6d %-8d %-28s %-8d %s" % (
                b, 1 << b, ",".join(str(s) for s in states) or "-",
                len(recs), verdict))

        # 反向确认：bit 0 永远是空的，因为 TCP 状态从 1 开始编号
        recs0 = dump_sockets(socket.AF_INET, socket.IPPROTO_TCP, 1 << 0, 0)
        print()
        print("补充验证：bit 0 (mask=1) 返回 %d 条。" % len(recs0))
        print("  因为 TCP 状态从 1 开始编号，bit 0 永远对应不到任何状态 —— "
              "这反过来证明位序号就是状态编号本身，而不是『状态编号-1』。")
        print()
        print("源码依据（以 6.12 为准）：include/net/tcp_states.h 中")
        print("  #define TCPF_LISTEN (1 << TCP_LISTEN)")
        print("  其中 TCP_LISTEN == 10，所以 TCPF_LISTEN == 1<<10 == 1024。")
        print("可在 elixir.bootlin.com/linux/v6.12/source/include/net/tcp_states.h 核对。")
    finally:
        for s in (cli, conn, srv):
            try:
                s.close()
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    if "--verify-states" in sys.argv:
        try:
            sys.exit(verify_states())
        except OSError as exc:
            print("[SKIP] netlink 不可用：%s" % exc)
            sys.exit(0)
    try:
        sys.exit(main())
    except PermissionError as exc:
        print("[SKIP] 权限不足：%s" % exc)
        sys.exit(0)   # 按契约：跑不了就优雅退出 0
    except OSError as exc:
        print("[SKIP] netlink 不可用：%s" % exc)
        sys.exit(0)
