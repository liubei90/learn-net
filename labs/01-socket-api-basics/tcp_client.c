/*
 * tcp_client.c —— TCP 收发压测 + 内核观测客户端（教学用）
 *
 * 演示要点：
 *   1. connect() 触发的内核路径 tcp_v4_connect()（本地端口从
 *      net.ipv4.ip_local_port_range 分配）；
 *   2. send()/recv() 与内核 tcp_sendmsg()/tcp_recvmsg() 的对应关系，
 *      以及“用户缓冲区大小”如何被内核加倍后成为 sk_sndbuf/sk_rcvbuf；
 *   3. TCP_NODELAY 开关对 Nagle 算法的影响；
 *   4. 连接结束后用 getsockopt(TCP_INFO) 读取真实内核统计
 *      （重传次数、RTT、cwnd、sacked），全部字段来自 struct tcp_info。
 *
 * 运行：./tcp_client --host 127.0.0.1 --port 9000 --count 4 --size 4096 --nodelay
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int read_int_opt(int fd, int level, int opt, const char *name)
{
    int v = -1;
    socklen_t l = sizeof v;
    if (getsockopt(fd, level, opt, &v, &l) < 0) {
        printf("  %-16s getsockopt failed: %s\n", name, strerror(errno));
        return -1;
    }
    return v;
}

static void report_tcp_info(int fd)
{
    struct tcp_info ti;
    socklen_t len = sizeof ti;
    memset(&ti, 0, sizeof ti);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &len) < 0) {
        printf("  TCP_INFO failed: %s\n", strerror(errno));
        return;
    }
    printf("  --- TCP_INFO (内核 struct tcp_info) ---\n");
    printf("  tcpi_state          = %u\n", ti.tcpi_state);
    printf("  tcpi_ca_state       = %u\n", ti.tcpi_ca_state);
    printf("  tcpi_retransmits    = %u  (当前正在重传的报文数)\n", ti.tcpi_retransmits);
    printf("  tcpi_total_retrans  = %u  (连接生命周期累计重传)\n", ti.tcpi_total_retrans);
    printf("  tcpi_snd_cwnd       = %u  (拥塞窗口，单位 MSS)\n", ti.tcpi_snd_cwnd);
    printf("  tcpi_snd_ssthresh   = %u\n", ti.tcpi_snd_ssthresh);
    printf("  tcpi_snd_wnd        = %u  (对端通告窗口，字节)\n", (unsigned)ti.tcpi_snd_wnd);
    printf("  tcpi_rcv_space      = %u  (本端接收自动调优空间)\n", ti.tcpi_rcv_space);
    printf("  tcpi_rtt            = %u us\n", ti.tcpi_rtt);
    printf("  tcpi_rttvar         = %u us\n", ti.tcpi_rttvar);
    printf("  tcpi_rto            = %u us\n", ti.tcpi_rto);
    printf("  tcpi_sacked         = %u  (SACK 标记的报文数)\n", ti.tcpi_sacked);
    printf("  tcpi_lost           = %u\n", ti.tcpi_lost);
    printf("  tcpi_unacked        = %u\n", ti.tcpi_unacked);
    printf("  tcpi_snd_mss        = %u\n", ti.tcpi_snd_mss);
    printf("  tcpi_rcv_mss        = %u\n", ti.tcpi_rcv_mss);
    printf("  tcpi_pmtu           = %u\n", ti.tcpi_pmtu);
    printf("  tcpi_advmss         = %u\n", ti.tcpi_advmss);
    printf("  tcpi_segs_out       = %u\n", ti.tcpi_segs_out);
    printf("  tcpi_segs_in        = %u\n", ti.tcpi_segs_in);
    printf("  tcpi_bytes_acked    = %llu\n", (unsigned long long)ti.tcpi_bytes_acked);
    printf("  tcpi_bytes_received = %llu\n", (unsigned long long)ti.tcpi_bytes_received);
    printf("  tcpi_notsent_bytes  = %u\n", ti.tcpi_notsent_bytes);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    unsigned short port = 9000;
    int count = 4;
    int size = 4096;
    bool nodelay = false;
    int sndbuf = -1, rcvbuf = -1;
    double sleep_after = 0.0; /* 收发结束后停住，便于外部 ss/proc 抓快照 */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned short)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nodelay")) nodelay = true;
        else if (!strcmp(argv[i], "--sndbuf") && i + 1 < argc) sndbuf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rcvbuf") && i + 1 < argc) rcvbuf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sleep-after") && i + 1 < argc) sleep_after = atof(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: %s [--host H] [--port P] [--count N] [--size BYTES] "
                    "[--nodelay] [--sndbuf N] [--rcvbuf N] [--sleep-after SEC]\n", argv[0]);
            return 2;
        }
    }
    if (count <= 0 || size <= 0 || size > 1 << 20) {
        fprintf(stderr, "invalid --count/--size\n");
        return 2;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    if (sndbuf > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf) < 0)
            perror("setsockopt SO_SNDBUF");
    }
    if (rcvbuf > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) < 0)
            perror("setsockopt SO_RCVBUF");
    }
    int one = 1;
    if (nodelay)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad host: %s\n", host);
        return 2;
    }

    printf("connect() -> %s:%u  count=%d size=%d nodelay=%d\n",
           host, port, count, size, nodelay ? 1 : 0);
    double t0 = now_ms();
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect");
        return 1;
    }
    double t1 = now_ms();

    struct sockaddr_in local;
    socklen_t ll = sizeof local;
    getsockname(fd, (struct sockaddr *)&local, &ll);
    printf("connect ok in %.3f ms; 本地端口 = %u (来自 ip_local_port_range)\n",
           t1 - t0, ntohs(local.sin_port));
    printf("  SO_SNDBUF(实测)  = %d\n", read_int_opt(fd, SOL_SOCKET, SO_SNDBUF, "SO_SNDBUF"));
    printf("  SO_RCVBUF(实测)  = %d\n", read_int_opt(fd, SOL_SOCKET, SO_RCVBUF, "SO_RCVBUF"));
    printf("  TCP_NODELAY(实测)= %d\n", read_int_opt(fd, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY"));
    printf("  TCP_MAXSEG(实测) = %d\n", read_int_opt(fd, IPPROTO_TCP, TCP_MAXSEG, "TCP_MAXSEG"));
    if (sndbuf > 0)
        printf("  [注意] 请求 SO_SNDBUF=%d，内核实测=%d（加倍语义）\n",
               sndbuf, read_int_opt(fd, SOL_SOCKET, SO_SNDBUF, "SO_SNDBUF"));
    if (rcvbuf > 0)
        printf("  [注意] 请求 SO_RCVBUF=%d，内核实测=%d（加倍语义）\n",
               rcvbuf, read_int_opt(fd, SOL_SOCKET, SO_RCVBUF, "SO_RCVBUF"));

    char *sbuf = malloc((size_t)size);
    char *rbuf = malloc((size_t)size);
    if (!sbuf || !rbuf) { perror("malloc"); return 1; }
    memset(sbuf, 'A', (size_t)size);

    double tw0 = now_ms();
    uint64_t sent = 0, recved = 0;
    for (int i = 0; i < count; i++) {
        if (i == 0)
            memcpy(sbuf, "PING", 4);
        ssize_t n = send(fd, sbuf, (size_t)size, MSG_NOSIGNAL);
        if (n < 0) { perror("send"); break; }
        sent += (uint64_t)n;
        size_t got = 0;
        while (got < (size_t)n) {
            ssize_t r = recv(fd, rbuf + got, (size_t)n - got, 0);
            if (r < 0) { perror("recv"); got = (size_t)n; break; }
            if (r == 0) { printf("  peer closed early\n"); break; }
            got += (size_t)r;
        }
        recved += got;
        if (i == 0)
            printf("echo check: %s (%zu bytes round-trip)\n", rbuf, got);
    }
    double tw1 = now_ms();
    if (tw1 - tw0 > 0.0)
        printf("round-trips=%d bytes_sent=%llu bytes_recv=%llu elapsed=%.3f ms throughput=%.2f MB/s\n",
               count, (unsigned long long)sent, (unsigned long long)recved,
               tw1 - tw0, ((double)sent / (1024.0 * 1024.0)) / ((tw1 - tw0) / 1000.0));

    if (sleep_after > 0.0) {
        printf("--sleep-after %.2fs：保持连接 ESTABLISHED，供外部 ss -tni / /proc 抓快照\n",
               sleep_after);
        fflush(stdout);
        struct timespec ts;
        ts.tv_sec = (time_t)sleep_after;
        ts.tv_nsec = (long)((sleep_after - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
        printf("sleep 结束，重新读取 TCP_INFO（注意 cwnd/rtt 已被内核更新）:\n");
    }

    report_tcp_info(fd);

    /* 半关闭：验证 FIN 语义；服务端读到 EOF 后关闭，本端进入 TIME_WAIT */
    shutdown(fd, SHUT_WR);
    char drain[512];
    while (recv(fd, drain, sizeof drain, 0) > 0)
        ;
    printf("after shutdown(SHUT_WR) + peer close:\n");
    report_tcp_info(fd);
    close(fd);
    printf("close() done; 主动关闭方本地会进入 TIME_WAIT（见 ss -tan / /proc/net/tcp）\n");
    free(sbuf);
    free(rbuf);
    return 0;
}
