/*
 * udp_echo.c —— UDP 回显服务端 + 客户端（单进程两种角色，教学用）
 *
 * 演示要点：
 *   1. UDP 是无连接数据报协议：recvfrom()/sendto() 一次调用对应一个 skb，
 *      内核路径为 udp_sendmsg() -> udp_send_skb() / __udp4_lib_rcv() ->
 *      udp_queue_rcv_skb() -> sk_receive_queue（与 TCP 的字节流语义完全不同）；
 *   2. 无连接 UDP 的接收队列是单一 sk_receive_queue，队列满即丢包并累加
 *      /proc/net/snmp 的 Udp: RcvbufErrors / InErrors；
 *   3. 用 connect() 让 UDP socket“记住”对端（connected UDP），
 *      之后可用 send()/recv()，内核走相同发送路径但跳过路由查找缓存。
 *
 * 运行：
 *   ./udp_echo server --port 0
 *   ./udp_echo client --port <P> --count 3 --size 512
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s server [--port N] [--once]\n"
            "       %s client --port P [--count N] [--size B] [--connect]\n",
            p, p);
}

static int do_server(unsigned short port, bool once)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int rcvbuf = 256 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) < 0)
        perror("setsockopt SO_RCVBUF");
    socklen_t l = sizeof rcvbuf;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &l);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    socklen_t al = sizeof addr;
    getsockname(fd, (struct sockaddr *)&addr, &al);
    /* 机器可读第一行，便于脚本抓取 */
    printf("UDP_ECHO_LISTENING 127.0.0.1:%u rcvbuf=%d pid=%d\n",
           ntohs(addr.sin_port), rcvbuf, (int)getpid());
    fflush(stdout);

    char buf[65536];
    unsigned long long pkts = 0, bytes = 0;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&peer, &pl);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        pkts++;
        bytes += (unsigned long long)n;
        if (sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&peer, pl) < 0) {
            perror("sendto");
            /* 典型的 ICMP port unreachable 反馈：ECONNREFUSED */
        }
        if (once && n > 0 && buf[0] == 'Q') {
            printf("UDP_ECHO_QUIT pkts=%llu bytes=%llu\n", pkts, bytes);
            break;
        }
    }
    close(fd);
    return 0;
}

static int do_client(unsigned short port, int count, int size, bool do_connect)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof srv);
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv.sin_port = htons(port);

    if (do_connect) {
        if (connect(fd, (struct sockaddr *)&srv, sizeof srv) < 0) {
            perror("connect(udp)");
            return 1;
        }
        printf("connected UDP: send()/recv() 现在可用，且能收到 ECONNREFUSED\n");
    }

    char *sbuf = malloc((size_t)size);
    char *rbuf = malloc((size_t)size > 65536 ? (size_t)size : 65536);
    if (!sbuf || !rbuf) { perror("malloc"); return 1; }
    memset(sbuf, 'u', (size_t)size);

    int ok = 0;
    for (int i = 0; i < count; i++) {
        memcpy(sbuf, "PING", 4);
        sbuf[4] = (char)('0' + (i % 10));
        double t0 = now_ms();
        ssize_t n = do_connect
                        ? send(fd, sbuf, (size_t)size, 0)
                        : sendto(fd, sbuf, (size_t)size, 0,
                                 (struct sockaddr *)&srv, sizeof srv);
        if (n < 0) {
            printf("send #%d failed: %s (errno=%d)\n", i, strerror(errno), errno);
            continue;
        }
        ssize_t r = do_connect ? recv(fd, rbuf, (size_t)size, 0)
                               : recvfrom(fd, rbuf, (size_t)size, 0, NULL, NULL);
        double t1 = now_ms();
        if (r < 0) {
            printf("recv #%d failed: %s\n", i, strerror(errno));
            continue;
        }
        ok++;
        printf("  #%d sent=%zd recv=%zd rtt=%.3f ms payload=\"%.*s\"\n",
               i, n, r, t1 - t0, (int)(r > 8 ? 8 : r), rbuf);
    }
    printf("UDP client done: %d/%d 成功往返\n", ok, count);
    free(sbuf);
    free(rbuf);
    close(fd);
    return ok == count ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *role = argv[1];
    unsigned short port = 0;
    int count = 3, size = 512;
    bool once = false, do_connect = false;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned short)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--once")) once = true;
        else if (!strcmp(argv[i], "--connect")) do_connect = true;
        else { usage(argv[0]); return 2; }
    }

    if (!strcmp(role, "server"))
        return do_server(port, once);
    if (!strcmp(role, "client")) {
        if (port == 0) { usage(argv[0]); return 2; }
        return do_client(port, count, size, do_connect);
    }
    usage(argv[0]);
    return 2;
}
