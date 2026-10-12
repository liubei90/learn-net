#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
parse_proc_tcp.py —— 把 /proc/net/tcp[6] 翻译成人可读的 TCP 连接表

为什么需要它：/proc/net/tcp 里地址是十六进制、端口是十六进制、状态是两位十六进制码，
肉眼基本没法读。本脚本做四件事：
  1. 解析 IPv4/IPv6 的 hex 地址与端口（IPv4 是小端存储的 4 字节；IPv6 是 4 个 32bit 小端字）；
  2. 把状态码翻译成名字（状态码表见 TCP_STATE 字典，与内核
     include/net/tcp_states.h 的 enum tcp_state 数值一致）；
  3. 汇总状态分布、每个本地端口的连接数、uid、inode、tx/rx 队列、重传计数；
  4. 支持只看某个端口 / 某个状态 / 只统计，以及从 /proc/net/netstat 读 TcpExt 计数。

用法示例：
    ./parse_proc_tcp.py                       # 打印全部 TCP 连接表
    ./parse_proc_tcp.py --state TIME_WAIT     # 只看 TIME_WAIT
    ./parse_proc_tcp.py --port 8080           # 只看涉及 8080 端口的行
    ./parse_proc_tcp.py --summary             # 只打印状态分布与端口汇总
    ./parse_proc_tcp.py --json                # 机器可读输出
    ./parse_proc_tcp.py --netstat TW ListenOverflows ListenDrops
    ./parse_proc_tcp.py --watch 5             # 每秒采样一次共 5 次，输出每轮状态分布

无需 root：/proc/net/tcp 的 uid 列对非特权用户可见，但只能看到全部 socket 的元数据
（没有 CAP_NET_ADMIN 也读得到，因为它不是内核密钥，只是连接元信息）。
"""

import argparse
import json
import os
import socket
import struct
import sys
import time

# 状态码表：必须与 include/net/tcp_states.h 的 enum tcp_state 一致
TCP_STATE = {
    0x01: "ESTABLISHED",
    0x02: "SYN_SENT",
    0x03: "SYN_RECV",
    0x04: "FIN_WAIT1",
    0x05: "FIN_WAIT2",
    0x06: "TIME_WAIT",
    0x07: "CLOSE",
    0x08: "CLOSE_WAIT",
    0x09: "LAST_ACK",
    0x0A: "LISTEN",
    0x0B: "CLOSING",
    # 0x0C SYN_RECV 之后的扩展状态在新内核里归入上述；TCP_NEW_SYN_RECV 只在内部使用，
    # 不会出现在 /proc/net/tcp 中。
}

PROC_FILES = ("/proc/net/tcp", "/proc/net/tcp6")


def hex_to_ipv4(h: str) -> str:
    """IPv4 以 __be32 存放，按小端字节序读出来再填点分十进制。"""
    raw = struct.pack("<I", int(h, 16))
    return socket.inet_ntop(socket.AF_INET, raw)


def hex_to_ipv6(h: str) -> str:
    """IPv6 在 /proc/net/tcp6 中按 4 组 32bit 小端字拼接。"""
    out = b""
    for i in range(0, 32, 8):
        out += struct.pack("<I", int(h[i:i + 8], 16))
    return socket.inet_ntop(socket.AF_INET6, out)


def parse_addr(field: str, v6: bool):
    addr_hex, _, port_hex = field.partition(":")
    try:
        ip = hex_to_ipv6(addr_hex) if v6 else hex_to_ipv4(addr_hex)
    except (ValueError, OSError):
        ip = "?" + addr_hex
    return ip, int(port_hex, 16)


def read_lines(path: str):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def parse_proc_file(path: str):
    """返回 (rows, header)。每行是 dict。"""
    lines = read_lines(path)
    if not lines:
        return [], []
    header = lines[0].split()
    v6 = path.endswith("tcp6")
    rows = []
    for line in lines[1:]:
        parts = line.split()
        if len(parts) < 12:
            continue
        try:
            local_ip, local_port = parse_addr(parts[1], v6)
            remote_ip, remote_port = parse_addr(parts[2], v6)
            st_code = int(parts[3], 16)
            txrx = parts[4].split(":")
            tr = parts[5].split(":")
            rows.append({
                "src_file": path,
                "sl": int(parts[0].rstrip(":")),
                "family": "ipv6" if v6 else "ipv4",
                "local_addr": local_ip,
                "local_port": local_port,
                "remote_addr": remote_ip,
                "remote_port": remote_port,
                "state_code": st_code,
                "state": TCP_STATE.get(st_code, "UNKNOWN(0x%02X)" % st_code),
                "tx_queue": int(txrx[0], 16),
                "rx_queue": int(txrx[1], 16),
                # tr:tm->when 中 tr 是重传超时次数，tm->when 是定时器剩余 jiffies（0x 表示无效）
                "retrnsmt": int(tr[0], 16) if tr[0] else 0,
                "timer_when": tr[1] if len(tr) > 1 else "",
                "uid": int(parts[6]),
                "timeout": int(parts[7]),
                "inode": int(parts[8]) if parts[8].isdigit() else 0,
            })
        except (ValueError, IndexError):
            continue
    return rows, header


def collect():
    rows = []
    for p in PROC_FILES:
        r, _ = parse_proc_file(p)
        rows.extend(r)
    return rows


def fmt_endpoint(ip, port):
    if ":" in ip:
        return "[%s]:%d" % (ip, port)
    return "%s:%d" % (ip, port)


def print_table(rows):
    print("%-6s %-5s %-28s %-28s %-12s %8s %8s %6s %6s %8s"
          % ("sl", "fam", "Local", "Remote", "State", "TxQ", "RxQ", "retr", "uid", "inode"))
    print("-" * 132)
    for r in rows:
        print("%-6d %-5s %-28s %-28s %-12s %8d %8d %6d %6d %8d"
              % (r["sl"], r["family"],
                 fmt_endpoint(r["local_addr"], r["local_port"]),
                 fmt_endpoint(r["remote_addr"], r["remote_port"]),
                 r["state"], r["tx_queue"], r["rx_queue"],
                 r["retrnsmt"], r["uid"], r["inode"]))


def print_summary(rows):
    from collections import Counter, defaultdict
    print("总条目数：%d（/proc/net/tcp + /proc/net/tcp6）" % len(rows))
    print("\n按状态分布：")
    st = Counter(r["state"] for r in rows)
    for name, n in sorted(st.items(), key=lambda kv: -kv[1]):
        print("  %-18s %4d" % (name, n))

    print("\n按本地端口分布（TOP 15）：")
    ports = Counter(r["local_port"] for r in rows)
    for port, n in ports.most_common(15):
        states = Counter(r["state"] for r in rows if r["local_port"] == port)
        detail = ", ".join("%s=%d" % (k, v) for k, v in states.most_common())
        print("  %-8d %4d  [%s]" % (port, n, detail))

    print("\n按 uid 分布：")
    for uid, n in Counter(r["uid"] for r in rows).most_common():
        print("  uid %-8d %4d" % (uid, n))

    listen = [r for r in rows if r["state"] == "LISTEN"]
    if listen:
        print("\nLISTEN 端口：")
        for r in sorted(listen, key=lambda x: x["local_port"]):
            print("  %-8d %s" % (r["local_port"], fmt_endpoint(r["local_addr"], r["local_port"])))

    tw = [r for r in rows if r["state"] == "TIME_WAIT"]
    print("\nTIME_WAIT 数：%d" % len(tw))
    if tw:
        print("  （TIME_WAIT 由主动关闭方持有，通常持续 60s，见 net.ipv4.tcp_fin_timeout 无关）")
        for r in tw[:10]:
            print("   %s -> %s  timer_when=%s"
                  % (fmt_endpoint(r["local_addr"], r["local_port"]),
                     fmt_endpoint(r["remote_addr"], r["remote_port"]), r["timer_when"]))

    # 发送队列 / 接收队列积压
    backlog = [r for r in rows if r["rx_queue"] or r["tx_queue"]]
    if backlog:
        print("\n存在队列积压的条目（tx_queue/rx_queue 非 0）：")
        for r in backlog[:15]:
            print("  %s -> %s  state=%s tx=%d rx=%d"
                  % (fmt_endpoint(r["local_addr"], r["local_port"]),
                     fmt_endpoint(r["remote_addr"], r["remote_port"]),
                     r["state"], r["tx_queue"], r["rx_queue"]))


def read_netstat(keys):
    """解析 /proc/net/netstat，返回 {key: value}。"""
    lines = read_lines("/proc/net/netstat")
    result = {}
    name_row = val_row = None
    for line in lines:
        if line.startswith("TcpExt:"):
            if name_row is None:
                name_row = line.split()[1:]
            elif val_row is None:
                val_row = line.split()[1:]
    if name_row and val_row:
        d = dict(zip(name_row, val_row))
        for k in keys:
            result[k] = d.get(k, "N/A")
    return result


def print_netstat(keys):
    print("=== /proc/net/netstat TcpExt 计数 ===")
    vals = read_netstat(keys)
    for k in keys:
        print("  %-24s = %s" % (k, vals.get(k, "N/A")))
    print("\n常见字段含义（内核 net/ipv4/tcp*.c 维护）：")
    meanings = {
        "TW": "TIME_WAIT 状态的 socket 数（tcp_time_wait() 累加）",
        "TWRecycled": "TIME_WAIT 被回收复用的次数（TWRecycled 统计）",
        "TWKilled": "因超过 net.ipv4.tcp_max_tw_buckets 而被强制关闭的 TIME_WAIT 数",
        "ListenOverflows": "三次握手完成但全连接队列(accept queue)已满而无法入队的连接数",
        "ListenDrops": "所有导致丢弃连接请求的计数总和（含 SYN 队列溢出）",
        "TCPTimeouts": "RTO 超时次数（重传定时器到期）",
        "TCPFastRetrans": "快速重传次数（收到 3 个 dup ACK）",
        "TCPSlowStartRetrans": "慢启动期间的重传次数",
        "TCPSackRecovery": "由 SACK 触发的快速恢复次数",
        "TCPLossProbes": "TLP（Tail Loss Probe）探测次数",
        "TCPAbortOnClose": "因应用 close 且接收队列有数据而发 RST 的次数",
        "TCPAbortOnLinger": "因 SO_LINGER 超时而中止连接的次数",
        "TCPBacklogDrop": "因 sk_add_backlog 失败而丢弃的报文数（backlog 队列满）",
        "TCPRcvCollapsed": "接收队列中 skb 被折叠合并的次数（tcp_collapse）",
        "DelayedACKs": "发出的延迟 ACK 次数",
        "DelayedACKLost": "延迟 ACK 期间检测到丢包而转为快速重传的次数",
        "TCPOFOQueue": "进入 out-of-order 队列的报文数",
        "TCPOFODrop": "因内存不足被丢弃的 out-of-order 报文数",
        "TCPOFOMerge": "与已有 out-of-order 段合并的报文数",
        "SyncookiesSent": "发出的 SYN cookie 数（SYN 队列溢出时启用）",
        "SyncookiesRecv": "收到的有效 SYN cookie 数",
        "TCPMemoryPressures": "TCP 内存压力事件次数",
    }
    for k in keys:
        if k in meanings:
            print("    %-22s %s" % (k + ":", meanings[k]))


def watch(rows_fn, times, interval):
    for i in range(times):
        rows = rows_fn()
        from collections import Counter
        st = Counter(r["state"] for r in rows)
        print("[%s] #%d 总计=%d  %s"
              % (time.strftime("%H:%M:%S"), i + 1, len(rows),
                 " ".join("%s=%d" % (k, v) for k, v in sorted(st.items()))))
        sys.stdout.flush()
        if i + 1 < times:
            time.sleep(interval)


def main():
    ap = argparse.ArgumentParser(description="解析 /proc/net/tcp 并翻译状态/地址/端口")
    ap.add_argument("--state", help="只显示指定状态，如 TIME_WAIT")
    ap.add_argument("--port", type=int, help="只显示本地或远端端口等于该值的条目")
    ap.add_argument("--summary", action="store_true", help="只打印汇总")
    ap.add_argument("--json", action="store_true", help="以 JSON 输出")
    ap.add_argument("--netstat", nargs="+", metavar="KEY", help="读取 /proc/net/netstat TcpExt 的指定计数")
    ap.add_argument("--watch", type=int, metavar="N", help="采样 N 次状态分布")
    ap.add_argument("--interval", type=float, default=1.0, help="--watch 的采样间隔秒数")
    args = ap.parse_args()

    if args.netstat:
        print_netstat(args.netstat)
        return 0

    if args.watch:
        watch(collect, args.watch, args.interval)
        return 0

    rows = collect()
    if args.state:
        want = args.state.upper()
        rows = [r for r in rows if r["state"] == want or ("0x%02X" % r["state_code"]) == want]
    if args.port:
        rows = [r for r in rows if r["local_port"] == args.port or r["remote_port"] == args.port]

    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=2))
        return 0

    if args.summary:
        print_summary(rows)
        return 0

    print_table(rows)
    print()
    print_summary(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
