/*
 * sockopt_probe.c —— 套接字选项与内核缓冲记账探针（教学用）
 *
 * 逐项实测（全部无需特权）：
 *   [1] SO_SNDBUF / SO_RCVBUF 的“内核加倍”语义
 *       setsockopt(SO_SNDBUF, x) 后 getsockopt 读到 2x
 *       —— 内核实现见 net/core/sock.c: sock_setsockopt() 的
 *          "vals[SO_SNDBUF] = min_t(unsigned int, val * 2, ...)" 逻辑，
 *          以及 include/net/sock.h 中 sk_sndbuf/sk_rcvbuf 的注释；
 *   [2] 与 sysctl 上限的关系：net.core.wmem_max / rmem_max 截断；
 *       net.ipv4.tcp_wmem / tcp_rmem 只在未显式 setsockopt 时生效
 *       （sk->sk_userlocks & SOCK_SNDBUF_LOCK / SOCK_RCVBUF_LOCK）；
 *   [3] 内存记账：/proc/net/sockstat 的 TCP inuse / mem 字段
 *       （sk_mem_charge()/sk_mem_uncharge() 落在 sk->sk_forward_alloc
 *        与 proto->memory_allocated 上）；
 *   [4] SO_REUSEADDR 与 SO_REUSEPORT 的行为差异（同时 bind 同一端口）；
 *   [5] TCP_NODELAY / TCP_MAXSEG / SO_KEEPALIVE 的读写回显。
 *
 * 运行：./sockopt_probe            # 全部探针
 *       ./sockopt_probe --skip-rebind
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
/* 使用内核 UAPI 头 <linux/tcp.h>：glibc 的 <netinet/tcp.h> 在 Debian 13 上仍是
 * 较旧的副本，缺少 tcpi_snd_wnd / tcpi_bytes_acked / tcpi_segs_out 等新字段。
 * 预定义 _NETINET_TCP_H 可避免 <netinet/in.h> 间接引入 glibc 版本造成 struct 重定义。 */
#define _NETINET_TCP_H 1
#include <linux/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int tcp_mem_from_sockstat(void)
{
    FILE *f = fopen("/proc/net/sockstat", "r");
    if (!f)
        return -1;
    char line[512];
    int mem = -1, inuse = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "TCP:", 4) != 0)
            continue;
        char *p = line;
        while ((p = strstr(p, "inuse")) != NULL) {
            sscanf(p, "inuse %d", &inuse);
            break;
        }
        p = line;
        while ((p = strstr(p, "mem")) != NULL) {
            sscanf(p, "mem %d", &mem);
            break;
        }
    }
    fclose(f);
    printf("  /proc/net/sockstat  TCP inuse=%d mem=%d pages\n", inuse, mem);
    return mem;
}

static int read_sysctl_int(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static void probe_default_buffers(void)
{
    printf("\n[1] 默认缓冲区 / sysctl 上限\n");
    printf("  sysctl net.core.rmem_default = %d\n", read_sysctl_int("/proc/sys/net/core/rmem_default"));
    printf("  sysctl net.core.wmem_default = %d\n", read_sysctl_int("/proc/sys/net/core/wmem_default"));
    printf("  sysctl net.core.rmem_max     = %d\n", read_sysctl_int("/proc/sys/net/core/rmem_max"));
    printf("  sysctl net.core.wmem_max     = %d\n", read_sysctl_int("/proc/sys/net/core/wmem_max"));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int snd = 0, rcv = 0;
    socklen_t l = sizeof snd;
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, &l);
    l = sizeof rcv;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, &l);
    printf("  新 TCP socket 实测 SO_SNDBUF=%d SO_RCVBUF=%d\n", snd, rcv);
    printf("  → 新建 SOCK_STREAM 时 sk_sndbuf/sk_rcvbuf 取自 net.core.wmem_default/rmem_default，\n"
           "    再被 tcp_wmem/tcp_rmem 的自动调优区间约束（见 net/ipv4/tcp.c: tcp_init_sock()）。\n");
    close(fd);
}

static void probe_doubling(void)
{
    printf("\n[2] SO_SNDBUF / SO_RCVBUF 加倍语义实测\n");
    int reqs[] = {1024, 4096, 65536, 100000, 200000, 1048576};
    printf("  %10s | %-22s | %-22s\n", "请求值", "SO_SNDBUF 回读", "SO_RCVBUF 回读");
    for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int want = reqs[i];
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &want, sizeof want);
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof want);
        int got_s = 0, got_r = 0;
        socklen_t l = sizeof got_s;
        getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &got_s, &l);
        l = sizeof got_r;
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got_r, &l);
        char note_s[64], note_r[64];
        snprintf(note_s, sizeof note_s, "%d (%s)", got_s,
                 got_s == want ? "未加倍" : (got_s == want * 2 ? "=2x" : "被截断"));
        snprintf(note_r, sizeof note_r, "%d (%s)", got_r,
                 got_r == want ? "未加倍" : (got_r == want * 2 ? "=2x" : "被截断"));
        printf("  %10d | %-22s | %-22s\n", want, note_s, note_r);
        close(fd);
    }
    printf("  结论：内核 sock_setsockopt() 把用户值乘 2 存入 sk_sndbuf/sk_rcvbuf，\n"
           "        回读时又除 2 呈现，所以 ≤ wmem_max/rmem_max 的请求表现为 2x；\n"
           "        超过上限的请求先被 min() 截断到 wmem_max/rmem_max 再乘 2。\n");
}

static void probe_memory_accounting(void)
{
    printf("\n[3] 缓冲区与 /proc/net/sockstat 内存记账\n");
    printf("  记账路径：分配 skb 数据区时 sk_mem_charge(sk, size) 增加 sk_forward_alloc 占用，\n"
           "            释放时 sk_mem_uncharge()；协议层用量汇总到 proto->memory_allocated，\n"
           "            由 tcp_memory_allocated（/proc/net/sockstat 的 TCP: mem）与\n"
           "            tcp_memory_pressure 体现（net/ipv4/tcp.c、include/net/sock.h）。\n");
    printf("  基线：\n");
    tcp_mem_from_sockstat();

    /* 建一个监听 + 一个已连接 socket，设置大缓冲区，观测 TCP mem 变化 */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); close(lfd); return; }
    listen(lfd, 8);
    socklen_t al = sizeof a;
    getsockname(lfd, (struct sockaddr *)&a, &al);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(cfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); close(lfd); close(cfd); return; }
    int afd = accept(lfd, NULL, NULL);
    int big = 2097152; /* 会被 rmem_max/wmem_max 截断 */
    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(afd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);

    /* 推数据填满接收队列，让 skb 真正占用内存 */
    char *buf = malloc(1 << 20);
    memset(buf, 'm', 1 << 20);
    size_t pushed = 0;
    for (int i = 0; i < 16; i++) {
        ssize_t n = send(cfd, buf, 1 << 20, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n <= 0)
            break;
        pushed += (size_t)n;
    }
    printf("  已推送 %zu 字节到被动方 sk_receive_queue（未 recv）后：\n", pushed);
    tcp_mem_from_sockstat();
    int s_snd = 0, s_rcv = 0;
    socklen_t l = sizeof s_snd;
    getsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &s_snd, &l);
    l = sizeof s_rcv;
    getsockopt(afd, SOL_SOCKET, SO_RCVBUF, &s_rcv, &l);
    printf("  主动方 SO_SNDBUF=%d  被动方 SO_RCVBUF=%d (请求 %d，受 rmem_max/wmem_max=%d 限制)\n",
           s_snd, s_rcv, big, read_sysctl_int("/proc/sys/net/core/rmem_max"));
    printf("  提示：sockstat 的 mem 单位是 page（x86_64 下 4 KiB），且包含其他进程的用量，\n"
           "        单次实验波动可能被噪声淹没，建议多跑几次或对比 ss -tm 的 skmem 明细。\n");

    close(afd);
    close(cfd);
    close(lfd);
    free(buf);
    printf("  关闭连接后：\n");
    tcp_mem_from_sockstat();
}

static int bind_same_port(int custom_reuse, int custom_reuseport, int *out_fd)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (custom_reuse)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (custom_reuseport)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(0);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &al);
    *out_fd = fd;
    return ntohs(a.sin_port);
}

static void try_second_listener(int reuse, int reuseport, unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (reuse) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (reuseport) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    int r = bind(fd, (struct sockaddr *)&a, sizeof a);
    printf("    bind(SO_REUSEADDR=%d SO_REUSEPORT=%d) -> %s\n",
           reuse, reuseport, r == 0 ? "成功（可共存/可抢占）" : strerror(errno));
    if (r == 0) {
        int gr = listen(fd, 8);
        printf("      listen() -> %s\n", gr == 0 ? "成功" : strerror(errno));
    }
    close(fd);
}

static void probe_reuse(void)
{
    printf("\n[4] SO_REUSEADDR 与 SO_REUSEPORT 行为对比\n");
    printf("  A. 两个 socket 都不设置任何选项：\n");
    int fd1 = -1;
    int p = bind_same_port(0, 0, &fd1);
    if (p > 0) {
        listen(fd1, 8);
        printf("    第一个监听已 bind+listen 到 127.0.0.1:%d\n", p);
        try_second_listener(0, 0, (unsigned short)p);
        try_second_listener(1, 0, (unsigned short)p); /* 仅 REUSEADDR */
        try_second_listener(0, 1, (unsigned short)p); /* 仅 REUSEPORT */
        try_second_listener(1, 1, (unsigned short)p); /* 两者都设 */
        close(fd1);
    }

    printf("  B. 两个 socket 都设置 SO_REUSEPORT：应能同时 bind+listen（内核做负载均衡）\n");
    int fa = -1;
    p = bind_same_port(1, 1, &fa);
    if (p > 0) {
        listen(fa, 8);
        printf("    第一个监听 127.0.0.1:%d (REUSEADDR+REUSEPORT)\n", p);
        try_second_listener(1, 1, (unsigned short)p);
        close(fa);
    }

    printf("  C. 内核差异（源码级）\n");
    printf("    SO_REUSEADDR → sock_setsockopt() 置 sk->sk_reuse = SK_CAN_REUSE；\n"
           "      影响 inet_csk_get_port()/inet_csk_bind_conflict()：允许 bind 处于\n"
           "      TIME_WAIT 的同一四元组，也允许绑定已存在监听端口的*通配地址 vs 具体地址*组合，\n"
           "      但不允许两个完全相同的 (addr,port) 同时 LISTEN。\n"
           "    SO_REUSEPORT → sock_setsockopt() 置 sk->sk_reuseport = 1，并在\n"
           "      inet_csk_bind_conflict() 里要求双方都是 reuseport 且 uid 一致（非特权时），\n"
           "      才允许同一 (addr,port) 多监听并存，accept 时由内核按流哈希选 socket。\n"
           "    对应源码：net/core/sock.c（选项落点）、net/ipv4/inet_connection_sock.c\n"
           "      （inet_csk_bind_conflict / inet_csk_get_port）。\n");
}

static void probe_misc(void)
{
    printf("\n[5] 其它常用选项的读写回显\n");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int v = 0;
    socklen_t l = sizeof v;
    const struct { int level, opt; const char *name; } items[] = {
        {IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY"},
        {IPPROTO_TCP, TCP_MAXSEG, "TCP_MAXSEG"},
        {IPPROTO_TCP, TCP_KEEPIDLE, "TCP_KEEPIDLE"},
        {IPPROTO_TCP, TCP_SYNCNT, "TCP_SYNCNT"},
        {IPPROTO_TCP, TCP_USER_TIMEOUT, "TCP_USER_TIMEOUT"},
        {SOL_SOCKET, SO_KEEPALIVE, "SO_KEEPALIVE"},
        {SOL_SOCKET, SO_LINGER, "SO_LINGER(struct)"},
        {SOL_SOCKET, SO_REUSEADDR, "SO_REUSEADDR"},
        {SOL_SOCKET, SO_REUSEPORT, "SO_REUSEPORT"},
        {SOL_SOCKET, SO_SNDBUF, "SO_SNDBUF"},
        {SOL_SOCKET, SO_RCVBUF, "SO_RCVBUF"},
        {SOL_SOCKET, SO_SNDTIMEO, "SO_SNDTIMEO(struct)"},
    };
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        if (!strncmp(items[i].name, "SO_LINGER", 9) ||
            !strncmp(items[i].name, "SO_SNDTIMEO", 11)) {
            struct linger lg;
            struct timeval tv;
            socklen_t ll = (items[i].opt == SO_LINGER) ? sizeof lg : sizeof tv;
            void *dst = (items[i].opt == SO_LINGER) ? (void *)&lg : (void *)&tv;
            if (getsockopt(fd, items[i].level, items[i].opt, dst, &ll) == 0) {
                if (items[i].opt == SO_LINGER)
                    printf("  %-20s = {l_onoff=%d, l_linger=%d}\n", items[i].name, lg.l_onoff, lg.l_linger);
                else
                    printf("  %-20s = {tv_sec=%ld, tv_usec=%ld}\n", items[i].name,
                           (long)tv.tv_sec, (long)tv.tv_usec);
            }
            continue;
        }
        l = sizeof v;
        v = 0;
        if (getsockopt(fd, items[i].level, items[i].opt, &v, &l) == 0)
            printf("  %-20s = %d\n", items[i].name, v);
        else
            printf("  %-20s getsockopt: %s\n", items[i].name, strerror(errno));
    }
    close(fd);
    printf("  选项落点：net/core/sock.c: sock_setsockopt()（SOL_SOCKET 层）\n"
           "            net/ipv4/tcp.c: do_tcp_setsockopt()（IPPROTO_TCP 层，如 TCP_NODELAY→icsk->icsk_ack.picts）\n");
}

int main(int argc, char **argv)
{
    bool skip_rebind = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--skip-rebind"))
            skip_rebind = true;
        else {
            fprintf(stderr, "usage: %s [--skip-rebind]\n", argv[0]);
            return 2;
        }
    }
    printf("=== sockopt_probe: 用 getsockopt 与 /proc 反推内核缓冲与选项语义 ===\n");
    probe_default_buffers();
    probe_doubling();
    probe_memory_accounting();
    if (!skip_rebind)
        probe_reuse();
    probe_misc();
    printf("\n=== 探针结束（全部操作均无需 root）===\n");
    return 0;
}
