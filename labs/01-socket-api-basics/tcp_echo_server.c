/*
 * tcp_echo_server.c —— 单线程 epoll TCP 回显服务器（教学用）
 *
 * 演示要点：
 *   1. socket()/bind()/listen()/accept4() 的完整生命周期；
 *   2. epoll 的 LT（水平触发）与 ET（边沿触发）两种模式差异，
 *      ET 模式下必须“读干净 + 写干净”，否则事件不再上报；
 *   3. 用 getsockopt(TCP_INFO) 打印内核 TCP 控制块的实时状态
 *      （state / cwnd / snd_wnd / rtt / retransmits / sacked），
 *      对应内核 net/ipv4/tcp.c: do_tcp_getsockopt() -> tcp_get_info()。
 *
 * 构建：见同目录 Makefile（-std=c11 -Wall -Wextra）
 * 运行：./tcp_echo_server --port 0 --trigger et --verbose
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
/* 使用内核 UAPI 头 <linux/tcp.h>：glibc 的 <netinet/tcp.h> 在 Debian 13 上仍是
 * 较旧的副本，缺少 tcpi_snd_wnd / tcpi_bytes_acked / tcpi_segs_out 等新字段。
 * 预定义 _NETINET_TCP_H 可避免 <netinet/in.h> 间接引入 glibc 版本造成 struct 重定义。 */
#define _NETINET_TCP_H 1
#include <linux/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONN 256
#define BUF_SZ (64 * 1024)

enum trigger_mode { MODE_LT, MODE_ET };

struct conn {
    int fd;
    bool in_use;
    uint64_t bytes_in;
    uint64_t bytes_out;
    size_t out_len; /* 待写字节数（ET 模式下必须自己缓冲） */
    size_t out_off;
    char out[BUF_SZ];
    struct sockaddr_in peer;
};

static struct conn g_conns[MAX_CONN];
static int g_listen_fd = -1;
static int g_epfd = -1;
static int g_wake_fd[2] = {-1, -1};
static volatile sig_atomic_t g_stop = 0;
static bool g_verbose = false;
static enum trigger_mode g_mode = MODE_LT;

static void logmsg(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    fprintf(stderr, "[%s.%03ld] ", stamp, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* TCP 状态常量由内核内部枚举 enum tcp_state 定义（include/net/tcp_states.h），
 * 不导出到 UAPI，因此这里按同一数值显式定义（与 /proc/net/tcp 的状态码一致）。 */
enum {
    LS_TCP_ESTABLISHED = 1,
    LS_TCP_SYN_SENT    = 2,
    LS_TCP_SYN_RECV    = 3,
    LS_TCP_FIN_WAIT1   = 4,
    LS_TCP_FIN_WAIT2   = 5,
    LS_TCP_TIME_WAIT   = 6,
    LS_TCP_CLOSE       = 7,
    LS_TCP_CLOSE_WAIT  = 8,
    LS_TCP_LAST_ACK    = 9,
    LS_TCP_LISTEN      = 10,
    LS_TCP_CLOSING     = 11,
};

static const char *tcp_state_str(uint8_t s)
{
    switch (s) {
    case LS_TCP_ESTABLISHED: return "ESTABLISHED";
    case LS_TCP_SYN_SENT:    return "SYN_SENT";
    case LS_TCP_SYN_RECV:    return "SYN_RECV";
    case LS_TCP_FIN_WAIT1:   return "FIN_WAIT1";
    case LS_TCP_FIN_WAIT2:   return "FIN_WAIT2";
    case LS_TCP_TIME_WAIT:   return "TIME_WAIT";
    case LS_TCP_CLOSE:       return "CLOSE";
    case LS_TCP_CLOSE_WAIT:  return "CLOSE_WAIT";
    case LS_TCP_LAST_ACK:    return "LAST_ACK";
    case LS_TCP_LISTEN:      return "LISTEN";
    case LS_TCP_CLOSING:     return "CLOSING";
    default:              return "UNKNOWN";
    }
}

/* 打印内核 TCP_INFO：字段全部来自 include/uapi/linux/tcp.h: struct tcp_info */
static void dump_tcp_info(int fd, const char *tag)
{
    struct tcp_info ti;
    socklen_t len = sizeof ti;
    memset(&ti, 0, sizeof ti);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &len) < 0) {
        logmsg("%s TCP_INFO failed: %s", tag, strerror(errno));
        return;
    }
    fprintf(stderr,
            "  [TCP_INFO %s] state=%s(%u) ca_state=%u "
            "cwnd=%u ssthresh=%u snd_wnd=%u rcv_wnd(rcv_space)=%u "
            "rtt=%u us rttvar=%u us rto=%u us "
            "retransmits=%u total_retrans=%u sacked=%u lost=%u unacked=%u "
            "segs_out=%u segs_in=%u bytes_acked=%llu bytes_received=%llu "
            "snd_mss=%u rcv_mss=%u pmtu=%u advmss=%u\n",
            tag, tcp_state_str(ti.tcpi_state), ti.tcpi_state, ti.tcpi_ca_state,
            ti.tcpi_snd_cwnd, ti.tcpi_snd_ssthresh,
            (unsigned)ti.tcpi_snd_wnd, ti.tcpi_rcv_space,
            ti.tcpi_rtt, ti.tcpi_rttvar, ti.tcpi_rto,
            ti.tcpi_retransmits, ti.tcpi_total_retrans, ti.tcpi_sacked,
            ti.tcpi_lost, ti.tcpi_unacked,
            ti.tcpi_segs_out, ti.tcpi_segs_in,
            (unsigned long long)ti.tcpi_bytes_acked,
            (unsigned long long)ti.tcpi_bytes_received,
            ti.tcpi_snd_mss, ti.tcpi_rcv_mss, ti.tcpi_pmtu, ti.tcpi_advmss);
}

static void dump_sockbuf(int fd, const char *tag)
{
    int snd = 0, rcv = 0;
    socklen_t l = sizeof snd;
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, &l);
    l = sizeof rcv;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, &l);
    int nodelay = 0;
    l = sizeof nodelay;
    getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &l);
    logmsg("%s SO_SNDBUF=%d SO_RCVBUF=%d TCP_NODELAY=%d", tag, snd, rcv, nodelay);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_wake_fd[1] >= 0) {
        char c = 'x';
        ssize_t n = write(g_wake_fd[1], &c, 1);
        (void)n;
    }
}

static struct conn *conn_alloc(int fd)
{
    for (int i = 0; i < MAX_CONN; i++) {
        if (!g_conns[i].in_use) {
            memset(&g_conns[i], 0, sizeof g_conns[i]);
            g_conns[i].fd = fd;
            g_conns[i].in_use = true;
            return &g_conns[i];
        }
    }
    return NULL;
}

static void conn_close(struct conn *c)
{
    logmsg("close fd=%d in=%llu out=%llu", c->fd,
           (unsigned long long)c->bytes_in, (unsigned long long)c->bytes_out);
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->in_use = false;
    c->fd = -1;
}

/* 尝试把缓冲区剩余数据写完；返回 -1 表示连接应关闭 */
static int conn_flush(struct conn *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            c->bytes_out += (uint64_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT,
                                     .data.fd = c->fd};
            if (g_mode == MODE_ET)
                ev.events |= EPOLLET;
            epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
            return 0;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    c->out_off = c->out_len = 0;
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = c->fd};
    if (g_mode == MODE_ET)
        ev.events |= EPOLLET;
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
    return 0;
}

static int conn_handle_read(struct conn *c)
{
    char buf[BUF_SZ];
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof buf, 0);
        if (n > 0) {
            c->bytes_in += (uint64_t)n;
            /* 教学实现：先把数据搬到用户态缓冲，再回写（体现内核 skb 拷贝语义） */
            if (c->out_len + (size_t)n > sizeof c->out) {
                logmsg("fd=%d user buffer full, dropping %zd bytes (教学简化)", c->fd, n);
                continue;
            }
            memcpy(c->out + c->out_len, buf, (size_t)n);
            c->out_len += (size_t)n;
            if (g_mode == MODE_LT)
                break; /* LT 模式下一轮 epoll_wait 会再次上报，无需循环 */
            continue;  /* ET 模式必须读到 EAGAIN */
        }
        if (n == 0) {
            return -1; /* 对端 FIN */
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        return -1;
    }
    return conn_flush(c);
}

static void accept_new(void)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept4(g_listen_fd, (struct sockaddr *)&peer, &plen,
                         SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            logmsg("accept4: %s", strerror(errno));
            return;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        struct conn *c = conn_alloc(fd);
        if (!c) {
            logmsg("too many conns, rejecting fd=%d", fd);
            close(fd);
            continue;
        }
        c->peer = peer;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        logmsg("accept fd=%d from %s:%u", fd, ip, ntohs(peer.sin_port));

        struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
        if (g_mode == MODE_ET)
            ev.events |= EPOLLET;
        if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            logmsg("epoll_ctl ADD: %s", strerror(errno));
            close(fd);
            c->in_use = false;
        }
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [--port N] [--trigger lt|et] [--verbose]\n"
            "  --port N      监听端口，默认 0（由内核临时端口分配，绑定后打印实际端口）\n"
            "  --trigger      epoll 触发模式，默认 lt\n"
            "  --verbose      打印每连接事件与 TCP_INFO\n",
            p);
}

int main(int argc, char **argv)
{
    unsigned short port = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = (unsigned short)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--trigger") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "et")) g_mode = MODE_ET;
            else if (!strcmp(m, "lt")) g_mode = MODE_LT;
            else { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "--verbose")) {
            g_verbose = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (pipe2(g_wake_fd, O_NONBLOCK | O_CLOEXEC) < 0) {
        perror("pipe2");
        return 1;
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(g_listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(g_listen_fd, (struct sockaddr *)&addr, &alen) == 0)
        port = ntohs(addr.sin_port);

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) {
        perror("epoll_create1");
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = g_listen_fd};
    if (g_mode == MODE_ET)
        ev.events |= EPOLLET;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ev);

    struct epoll_event wev = {.events = EPOLLIN, .data.fd = g_wake_fd[0]};
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_wake_fd[0], &wev);

    /* 关键：把实际端口打到 stdout 并 flush，便于 run.sh 抓取 */
    printf("LISTENING 127.0.0.1:%u trigger=%s pid=%d\n", port,
           g_mode == MODE_ET ? "et" : "lt", (int)getpid());
    fflush(stdout);
    logmsg("listen fd=%d epfd=%d mode=%s", g_listen_fd, g_epfd,
           g_mode == MODE_ET ? "et" : "lt");

    struct epoll_event events[64];
    while (!g_stop) {
        int n = epoll_wait(g_epfd, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;

            if (fd == g_wake_fd[0]) {
                char drain[64];
                while (read(g_wake_fd[0], drain, sizeof drain) > 0)
                    ;
                continue;
            }
            if (fd == g_listen_fd) {
                accept_new();
                continue;
            }

            struct conn *c = NULL;
            for (int k = 0; k < MAX_CONN; k++) {
                if (g_conns[k].in_use && g_conns[k].fd == fd) {
                    c = &g_conns[k];
                    break;
                }
            }
            if (!c)
                continue;

            if (evs & (EPOLLHUP | EPOLLERR)) {
                dump_tcp_info(fd, "on-hup");
                conn_close(c);
                continue;
            }
            if (evs & EPOLLOUT) {
                if (c->out_off < c->out_len && conn_flush(c) < 0) {
                    conn_close(c);
                    continue;
                }
            }
            if (evs & EPOLLIN) {
                int r = conn_handle_read(c);
                if (r < 0) {
                    dump_tcp_info(fd, "on-peer-fin");
                    dump_sockbuf(fd, "on-peer-fin");
                    conn_close(c);
                    continue;
                }
                logmsg("fd=%d in=%llu out=%llu pending=%zu", c->fd,
                       (unsigned long long)c->bytes_in,
                       (unsigned long long)c->bytes_out, c->out_len - c->out_off);
            }
        }
    }

    /* 打印所有仍存活连接的 TCP_INFO 后退场 */
    for (int k = 0; k < MAX_CONN; k++) {
        if (g_conns[k].in_use) {
            dump_tcp_info(g_conns[k].fd, "shutdown");
            conn_close(&g_conns[k]);
        }
    }
    logmsg("server exiting");
    close(g_listen_fd);
    close(g_epfd);
    close(g_wake_fd[0]);
    close(g_wake_fd[1]);
    return 0;
}
