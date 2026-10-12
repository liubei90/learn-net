/*
 * backlog_probe.c —— 用用户态观测面看懂 SYN 队列 / accept 队列（教学用）
 *
 * 背景（内核源码位置）：
 *   listen(fd, backlog) → net/socket.c: __sys_listen() → sock->ops->listen →
 *     net/ipv4/inet_connection_sock.c: inet_csk_listen_start()
 *       实际值 = min(backlog, net.core.somaxconn)，写入
 *       sk->sk_max_ack_backlog（"全连接队列"上限）。
 *   SYN 到达 → net/ipv4/tcp_input.c: tcp_conn_request()
 *       → inet_csk_reqsk_queue_add()：request_sock 进入
 *         icsk->icsk_accept_queue（半连接队列，即 SYN 队列），
 *         受 net.ipv4.tcp_max_syn_backlog 与 synq 溢出逻辑约束；
 *   ACK 到达 → tcp_check_req() → tcp_v4_syn_recv_sock()
 *       → inet_csk_complete_hashdance()：新 sock 挂到
 *         icsk_accept_queue.rskq_accept_head（全连接队列，
 *         长度即 ss -lnt 的 Recv-Q）；
 *   accept() → net/ipv4/inet_connection_sock.c: inet_csk_accept()
 *       从该队列摘走一个已建立连接。
 *
 * ss -lnt 的读法：
 *   Recv-Q = 当前全连接队列（icsk_accept_queue）中尚未 accept 的连接数
 *   Send-Q = sk_max_ack_backlog，即 listen 时内核实际采纳的 backlog
 *
 * 本程序做三件事：
 *   [阶段 1] listen(backlog) 后立刻用 ss 与内核接口确认实际 backlog；
 *   [阶段 2] 故意延迟 accept()，并发客户端把全连接队列塞满，
 *            统计成功/失败连接数，并读取 /proc/net/netstat TcpExt
 *            的 ListenOverflows / ListenDrops 增量；
 *   [阶段 3] 恢复正常 accept()，确认队列被排空。
 *
 * 运行：./backlog_probe --port 0 --backlog 1 --clients 24 --delay 3
 * 全部操作无需 root。
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_listen_fd = -1;
static unsigned short g_port = 0;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void msleep(double ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

/* 从 /proc/net/netstat 的 TcpExt 行取指定计数（同时兼容 name/value 两行格式） */
static long netstat_tapext(const char *key)
{
    FILE *f = fopen("/proc/net/netstat", "r");
    if (!f)
        return -1;
    char names[65536] = {0}, vals[65536] = {0};
    char line[65536];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "TcpExt:", 7)) {
            size_t n = strlen(line + 7);
            if (n >= sizeof names) n = sizeof names - 1;
            if (names[0] == 0) memcpy(names, line + 7, n), names[n] = 0;
            else               memcpy(vals,  line + 7, n), vals[n]  = 0;
        }
    }
    fclose(f);
    if (!names[0] || !vals[0])
        return -1;

    char *np = names, *vp = vals;
    while (*np && *vp) {
        char nbuf[64], vbuf[64];
        int i = 0;
        while (*np == ' ' || *np == '\t') np++;
        while (*np && *np != ' ' && *np != '\t' && *np != '\n' && i < 63) nbuf[i++] = *np++;
        nbuf[i] = 0;
        i = 0;
        while (*vp == ' ' || *vp == '\t') vp++;
        while (*vp && *vp != ' ' && *vp != '\t' && *vp != '\n' && i < 63) vbuf[i++] = *vp++;
        vbuf[i] = 0;
        if (!strcmp(nbuf, key))
            return strtol(vbuf, NULL, 10);
        if (!*np || !*vp)
            break;
    }
    return -1;
}

static void print_listen_queue(const char *tag)
{
    char cmd[256];
    printf("  --- %s: ss -lnt sport = :%u ---\n", tag, g_port);
    snprintf(cmd, sizeof cmd, "ss -lnt 'sport = :%u'", g_port);
    fflush(stdout);
    int rc = system(cmd);
    (void)rc;
    printf("  --- %s: /proc/net/tcp 中该监听端口对应行（st=0A 表示 LISTEN）---\n", tag);
    snprintf(cmd, sizeof cmd,
             "awk 'NR==1 || $4==\"0A\"' /proc/net/tcp | head -12");
    rc = system(cmd);
    (void)rc;
    fflush(stdout);
}

/* 一个并发客户端：非阻塞 connect，timeout_ms 后仍未完成则判定失败 */
static int client_once(double timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(g_port);

    double t0 = now_ms();
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r == 0) {
        close(fd);
        return 0;
    }
    fd_set wf;
    struct timeval tv;
    for (;;) {
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        double left = timeout_ms - (now_ms() - t0);
        if (left <= 0) { close(fd); return -1; }
        tv.tv_sec = (time_t)(left / 1000.0);
        tv.tv_usec = (long)((left - (double)tv.tv_sec * 1000.0) * 1e3);
        r = select(fd + 1, NULL, &wf, NULL, &tv);
        if (r > 0) {
            int err = 0;
            socklen_t l = sizeof err;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0) err = errno;
            close(fd);
            return err == 0 ? 0 : -1;
        }
        if (r == 0) { close(fd); return -1; }   /* 超时：连接被丢弃 */
        if (errno != EINTR) { close(fd); return -1; }
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [--port N] [--backlog N] [--clients N] [--delay SEC] "
            "[--accept-all] [--connect-timeout MS]\n", p);
}

int main(int argc, char **argv)
{
    unsigned short port = 0;
    int backlog = 1;
    int clients = 24;
    double delay = 3.0;
    double cto = 600.0;
    bool accept_all = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned short)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--backlog") && i + 1 < argc) backlog = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--clients") && i + 1 < argc) clients = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc) delay = atof(argv[++i]);
        else if (!strcmp(argv[i], "--connect-timeout") && i + 1 < argc) cto = atof(argv[++i]);
        else if (!strcmp(argv[i], "--accept-all")) accept_all = true;
        else { usage(argv[0]); return 2; }
    }
    if (clients <= 0 || clients > 512) { usage(argv[0]); return 2; }

    signal(SIGPIPE, SIG_IGN);

    /* 监听 socket 用 SOCK_NONBLOCK：本程序后面的 accept() 是「有多少收多少」
     * 的轮询语义，必须靠 EAGAIN 判断队列已空；阻塞模式下最后一个 accept()
     * 会永远挂住（这是本实验最初真实踩到的 bug）。 */
    g_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (g_listen_fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(g_listen_fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    socklen_t al = sizeof a;
    getsockname(g_listen_fd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);

    printf("=== backlog_probe ===\n");
    printf("请求 listen(backlog=%d)，端口 = %u\n", backlog, g_port);

    long ovf0 = netstat_tapext("ListenOverflows");
    long drp0 = netstat_tapext("ListenDrops");

    if (listen(g_listen_fd, backlog) < 0) { perror("listen"); return 1; }

    /* 内核实际采纳的 backlog 无法直接 getsockopt 读取；
     * 但它在 ss -lnt 的 Send-Q 列与 /proc/net/tcp 的第 5 个字段（tx_queue:rx_queue）中可见。 */
    printf("内核 max_ack_backlog 由 ss -lnt 的 Send-Q 列给出：\n");
    print_listen_queue("listen 之后");
    printf("提示：net.core.somaxconn=%ld，backlog 请求值会被 min() 截断为它。\n",
           ({ FILE *f = fopen("/proc/sys/net/core/somaxconn", "r");
              long v = -1; if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); } v; }));

    printf("\n[阶段 1] 并发发起 %d 个连接，服务端 %s accept（延迟 %.1fs）\n",
           clients, accept_all ? "立即" : "故意延迟", delay);

    pid_t *pids = calloc((size_t)clients, sizeof(pid_t));
    if (!pids) { perror("calloc"); return 1; }

    double t_start = now_ms();
    for (int i = 0; i < clients; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            /* 子进程：等父进程把 backlog 塞满，尽量同时冲 */
            int ok = client_once(cto);
            _exit(ok == 0 ? 0 : 1);
        }
        if (pid < 0) {
            perror("fork");
            clients = i;
            break;
        }
        pids[i] = pid;
    }

    if (!accept_all) {
        /* 在 accept 之前抓队列快照：此时 Recv-Q 应当等于 Send-Q（队满） */
        msleep(300);
        print_listen_queue("accept 之前（队列应被塞满）");
        fflush(stdout);
        double waited = now_ms() - t_start;
        if (delay * 1000.0 > waited)
            msleep(delay * 1000.0 - waited);
    } else {
        msleep(300);
        print_listen_queue("立即 accept 模式下");
    }

    /* 收集子进程结果 */
    int success = 0, failed = 0;
    for (int i = 0; i < clients; i++) {
        int st = 0;
        if (pids[i] > 0 && waitpid(pids[i], &st, 0) > 0) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
                success++;
            else
                failed++;
        }
    }
    printf("\n[阶段 2] 连接结果：客户端侧 connect 完成 = %d，失败(超时) = %d\n",
           success, failed);
    printf("  注意：本机 net.ipv4.tcp_syncookies=1，全连接队列满时内核不会回 RST，\n"
           "        而是静默丢弃第三次握手的 ACK，因此客户端 connect() 仍会「成功」，\n"
           "        但服务端 accept() 永远拿不到这条连接 —— 这就是最难排查的\n"
           "        「客户端认为连上了、服务端却没收到」现象。证据见阶段 4 的计数增量。\n");

    /* 服务端开始 accept，把全连接队列排空 */
    printf("\n[阶段 3] 服务端开始 accept()，观察队列被排空\n");
    int accepted = 0;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(g_listen_fd, (struct sockaddr *)&peer, &pl);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            /* 没有更多排队连接时阻塞式 accept 会挂住，这里靠非阻塞+轮询 */
            break;
        }
        accepted++;
        close(fd);
    }
    printf("  本轮 accept() 摘走连接数 = %d\n", accepted);

    /* 再等一轮，把在途完成握手的连接也收掉 */
    msleep(500);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(g_listen_fd, (struct sockaddr *)&peer, &pl);
        if (fd < 0)
            break;
        accepted++;
        close(fd);
    }
    printf("  合计 accept() 连接数 = %d\n", accepted);
    print_listen_queue("accept 之后");

    long ovf1 = netstat_tapext("ListenOverflows");
    long drp1 = netstat_tapext("ListenDrops");
    printf("\n[阶段 4] /proc/net/netstat TcpExt 计数\n");
    printf("  ListenOverflows: %ld -> %ld (增量 %ld)\n", ovf0, ovf1, ovf1 - ovf0);
    printf("  ListenDrops    : %ld -> %ld (增量 %ld)\n", drp0, drp1, drp1 - drp0);
    printf("  含义：\n");
    printf("    ListenOverflows —— 三次握手已完成、但全连接队列已满而无处安放的连接数\n"
           "      （内核 net/ipv4/tcp_input.c: tcp_v4_syn_recv_sock() 之后\n"
           "        inet_csk_reqsk_queue_is_full() 判定失败的分支）。\n"
           "    ListenDrops     —— 因队列满/内存压力被直接丢弃的连接请求总数\n"
           "      （含 SYN 队列溢出被丢的 SYN，见 tcp_conn_request() 的 drop 分支）。\n");
    printf("  相关 sysctl（本机只读、不可写）：\n"
           "    net.core.somaxconn             = listen(backlog) 的上限，两者取 min 得到\n"
           "                                     sk_max_ack_backlog（ss -lnt 的 Send-Q）\n"
           "    net.ipv4.tcp_max_syn_backlog   = SYN 队列相关阈值，主要参与 cookie/溢出决策\n"
           "    net.ipv4.tcp_abort_on_overflow = 溢出时是否回 RST（0=默认静默丢 ACK）\n"
           "    net.ipv4.tcp_syncookies        = SYN 队列溢出时用 cookie 兜底（本机=1）\n");
    printf("  备注：两个队列的 is_full 判定都基于 sk_max_ack_backlog：\n"
           "    inet_csk_reqsk_queue_is_full(): qlen > sk_max_ack_backlog\n"
           "    sk_acceptq_is_full()          : sk_ack_backlog > sk_max_ack_backlog\n"
           "  所以 listen(1) 时 Recv-Q 可能显示 2（队满 + 1，严格大于的语义）。\n");

    free(pids);
    close(g_listen_fd);
    return success > 0 ? 0 : 1;
}
