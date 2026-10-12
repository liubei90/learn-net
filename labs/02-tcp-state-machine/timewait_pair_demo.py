#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
timewait_pair_demo.py —— 用固定四元组复现 TIME_WAIT 对 bind 的影响（无需 root）

TCP 的 TIME_WAIT 是「四元组级」的：只有当新连接想复用的
(local_ip, local_port, remote_ip, remote_port) 与处于 TIME_WAIT 的连接完全相同时，
bind() 才会被 inet_csk_get_port()/inet_csk_bind_conflict() 拒绝（EADDRINUSE），
除非设置了 SO_REUSEADDR（sk_reuse = SK_CAN_REUSE）。

本脚本用 SO_REUSEPORT + 固定本地端口 42000 精确构造这种情况：
  1. 起监听 L(127.0.0.1:PORT_LISTEN)；
  2. 客户端 C 绑定固定本地端口 42000，connect 到 L，由 C 主动 close →
     C 的 (127.0.0.1:42000 → 127.0.0.1:PORT_LISTEN) 进入 TIME_WAIT；
  3. 立刻再次 bind(127.0.0.1:42000) 且不设 SO_REUSEADDR → 预期 EADDRINUSE；
  4. 设 SO_REUSEADDR 再 bind → 预期成功；
  5. 打印 /proc/net/tcp 中该四元组的状态码，验证就是 06 (TIME_WAIT)。

用法：./timewait_pair_demo.py [--local-port 42000]
"""

import argparse
import errno
import socket
import struct
import sys
import time

TW_STATES = {6: "TIME_WAIT"}


def hexdump_tw(local_port, remote_port):
    """直接从 /proc/net/tcp 里找 TIME_WAIT 的四元组，返回匹配行（原始文本）。"""
    hits = []
    try:
        with open("/proc/net/tcp", "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return hits
    for line in lines[1:]:
        p = line.split()
        if len(p) < 4:
            continue
        try:
            lport = int(p[1].split(":")[1], 16)
            rport = int(p[2].split(":")[1], 16)
            st = int(p[3], 16)
        except (ValueError, IndexError):
            continue
        if lport == local_port and (remote_port is None or rport == remote_port):
            hits.append((st, line))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--local-port", type=int, default=42000,
                    help="客户端侧固定本地端口（默认 42000）")
    args = ap.parse_args()
    lport = args.local_port

    print("=== timewait_pair_demo ===")
    print("固定客户端本地端口 = %d" % lport)

    # 1) 监听端
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    listen_port = listener.getsockname()[1]
    print("监听端       = 127.0.0.1:%d" % listen_port)

    # 2) 客户端固定本地端口并主动关闭
    def new_client():
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        try:
            s.bind(("127.0.0.1", lport))
        except OSError as e:
            print("  客户端 bind(127.0.0.1:%d) 失败: %s" % (lport, e))
            print("  提示：该端口可能已被占用，请换 --local-port")
            sys.exit(1)
        s.connect(("127.0.0.1", listen_port))
        return s

    c = new_client()
    a = listener.accept()[0]
    print("连接建立：%s -> %s" % (c.getsockname(), c.getpeername()))
    c.close()          # 主动关闭 → 客户端侧进入 TIME_WAIT
    time.sleep(0.2)
    a.close()
    listener.close()

    hits = hexdump_tw(lport, listen_port)
    if hits:
        st, line = hits[0]
        print("  /proc/net/tcp 中的该四元组（状态码 0x%02X = %s）:" % (st, TW_STATES.get(st, "?")))
        print("    " + line.strip())
    else:
        print("  /proc/net/tcp 中未找到该四元组（TIME_WAIT 可能已过期）")

    # 3) 不设 SO_REUSEADDR 直接重绑同一本地端口
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s2.bind(("127.0.0.1", lport))
        print("[期望失败但成功] bind 未设 SO_REUSEADDR：成功（说明内核允许复用）")
    except OSError as e:
        ename = errno.errorcode.get(e.errno, str(e.errno))
        print("[符合预期] bind 未设 SO_REUSEADDR：%s (%s)" % (e, ename))
        print("           原因：inet_csk_bind_conflict() 发现同四元组处于 TIME_WAIT，")
        print("                 且本 socket 的 sk_reuse 不是 SK_CAN_REUSE。")
    finally:
        s2.close()

    # 4) 设 SO_REUSEADDR 再绑
    s3 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s3.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s3.bind(("127.0.0.1", lport))
        print("[预期结果] bind 设 SO_REUSEADDR 后：成功")
        print("           原因：sock_setsockopt() 置 sk->sk_reuse = SK_CAN_REUSE，")
        print("                 inet_csk_bind_conflict() 对 TIME_WAIT 状态放行。")
    except OSError as e:
        print("[意外] bind 设 SO_REUSEADDR 后仍失败：%s" % e)
    finally:
        s3.close()

    print("\n结论：")
    print("  - TIME_WAIT 的粒度是四元组，不是端口本身；")
    print("  - SO_REUSEADDR 解决的是「bind 被 TIME_WAIT 挡住」这一类问题；")
    print("  - 端口耗尽（大量短连接）才需要 net.ipv4.ip_local_port_range 与 tcp_tw_reuse，")
    print("    后者本机已由内核默认值放行（/proc/sys/net/ipv4/tcp_tw_reuse）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
