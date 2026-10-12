/*
 * timewait_demo.c —— 复现并解释 TIME_WAIT / SO_LINGER(RST) / SO_REUSEADDR（教学用）
 *
 * 内核路径（全部在 net/ipv4/ 下）：
 *   主动关闭：close() → tcp_close() → tcp_send_fin() → tcp_set_state(TCP_FIN_WAIT1)
 *             → 收到对端 FIN+ACK → tcp_rcv_state_process() 的 FIN_WAIT2/TIME_WAIT 分支
 *             → tcp_time_wait()：把 sock 变成轻量 timewait 结构挂进
 *               inet_hashinfo 的 ehash（include/net/inet_timewait_sock.h 的
 *               struct inet_timewait_sock），并由
 *               tcp_timewait_state_process() 处理重传的 FIN/新 SYN。
 *   被动关闭：收到 FIN → tcp_set_state(TCP_CLOSE_WAIT)，应用 close() 后发 FIN
 *             进入 LAST_ACK，收到 ACK 后 CLOSED。
 *   SO_LINGER{l_onoff=1,l_linger=0}：tcp_close() 走 tcp_send_active_reset()，
 *             直接发 RST，双方跳过 TIME_WAIT（用于“快速回收”但会丢数据）。
 *   SO_REUSEADDR：sock_setsockopt() 置 sk_reuse，影响
 *             inet_csk_get_port()/inet_csk_bind_conflict()：
 *             允许 bind 处于 TIME_WAIT 的同一四元组。
 *
 * 本程序输出全部依赖用户态可用的观测面：getsockopt(TCP_INFO)、
 * /proc/net/tcp、/proc/net/netstat(TcpExt)、以及 ss（由 shell 调用）。
 * 无需 root。
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define _NETINET_TCP_H 1
#include <linux/tcp.h>

static void msleep(double ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

static const char *state_name(int s)
{
    switch (s) {
    case 1:  return "ESTABLISHED";
    case 2:  return "SYN_SENT";
    case 3:  return "SYN_RECV";
    case 4:  return "FIN_WAIT1";
    case 5:  return "FIN_WAIT2";   /* ss 显示为 FIN-WAIT-2 */
    case 6:  return "TIME_WAIT";
    case 7:  return "CLOSE";
    case 8:  return "CLOSE_WAIT";  /* ss: CLOSE-WAIT */
    case 9:  return "LAST_ACK";
    case 10: return "LISTEN";
    case 11: return "CLOSING";
    default: return "?";
    }
}

static int tcp_state(int fd)
{
    struct tcp_info ti;
    socklen_t l = sizeof ti;
    memset(&ti, 0, sizeof ti);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &l) < 0)
        return -1;
    return ti.tcpi_state;
}

static void show_state(int fd, const char *tag)
{
    int s = tcp_state(fd);
    printf("  %-42s TCP_INFO state = %d (%s)\n", tag, s,
           s > 0 ? state_name(s) : "getsockopt failed");
    fflush(stdout);
}

static long tapext(const char *key)
{
    FILE *f = fopen("/proc/net/netstat", "r");
    if (!f) return -1;
    char names[65536] = {0}, vals[65536] = {0}, line[65536];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "TcpExt:", 7)) {
            size_t n = strlen(line + 7);
            if (n >= sizeof names) n = sizeof names - 1;
            if (!names[0]) memcpy(names, line + 7, n), names[n] = 0;
            else           memcpy(vals,  line + 7, n), vals[n]  = 0;
        }
    }
    fclose(f);
    if (!names[0] || !vals[0]) return -1;
    char *np = names, *vp = vals;
    while (*np && *vp) {
        char nb[64], vb[64];
        int i = 0;
        while (*np == ' ' || *np == '\t') np++;
        while (*np && *np != ' ' && *np != '\t' && *np != '\n' && i < 63) nb[i++] = *np++;
        nb[i] = 0;
        i = 0;
        while (*vp == ' ' || *vp == '\t') vp++;
        while (*vp && *vp != ' ' && *vp != '\t' && *vp != '\n' && i < 63) vb[i++] = *vp++;
        vb[i] = 0;
        if (!strcmp(nb, key)) return strtol(vb, NULL, 10);
        if (!*np || !*vp) break;
    }
    return -1;
}

static void cat_sysctl(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[256] = "?";
    if (f) {
        if (!fgets(buf, sizeof buf, f)) snprintf(buf, sizeof buf, "?");
        fclose(f);
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
    }
    printf("  %-42s = %s\n", path, buf);
}

static int make_listener(unsigned short *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setenv("LISTEN_ONE", "1", 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(*port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &al);
    *port = ntohs(a.sin_port);
    return fd;
}

/* reuse!=0 时在 connect 之前设置 SO_REUSEADDR。
 * 这一点至关重要：TIME_WAIT 的 bind 冲突判定是**双向**的 ——
 * inet_csk_bind_conflict() 除了看新 socket 的 sk_reuse，
 * 还要求旧连接（已变成 inet_timewait_sock）的 tw_reuse 也是允许的，
 * 而 tw_reuse 是从原 socket 继承的。所以“服务端先设 SO_REUSEADDR 再 listen”
 * 才使得 TIME_WAIT 端口可被复用。 */
static int connect_loopback_reuse(unsigned short port, int reuse)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (reuse) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

/* 用 `ss -tan` 抓取某端口上的所有 TCP 状态（依赖 iproute2 的 ss） */
static void shell_ss(const char *filter)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "ss -tan %s", filter);
    printf("  $ %s\n", cmd);
    fflush(stdout);
    int rc = system(cmd);
    (void)rc;
    fflush(stdout);
}

/* 场景 1：应用层主动关闭 → TIME_WAIT，并复现 bind 冲突 */
static void scenario1(void)
{
    printf("\n========== 场景 1：主动关闭方进入 TIME_WAIT，并尝试立即重绑同一四元组 ==========\n");
    unsigned short port = 0;
    int lfd = make_listener(&port);
    if (lfd < 0) { perror("make_listener"); return; }
    printf("监听端口 = %u\n", port);

    long tw0 = tapext("TW");
    printf("  客户端 connect 前已设置 SO_REUSEADDR（使旧连接演化的 TIME_WAIT 可被复用）\n");
    int cfd = connect_loopback_reuse(port, 1);
    if (cfd < 0) { perror("connect"); close(lfd); return; }
    int afd = accept(lfd, NULL, NULL);

    struct sockaddr_in local;
    socklen_t ll = sizeof local;
    getsockname(cfd, (struct sockaddr *)&local, &ll);
    unsigned short lport = ntohs(local.sin_port);
    show_state(cfd, "客户端 connect 后");
    show_state(afd, "服务端 accept 后");

    printf("  客户端 close()（主动关闭：FIN_WAIT1 -> FIN_WAIT2 -> TIME_WAIT）\n");
    close(cfd);
    msleep(200);
    show_state(afd, "服务端（被动关闭方）收到 FIN 后");

    /* 关键：服务端必须也走完「读走 EOF → close 发 FIN」，
     * 客户端收到这个 FIN 才会从 FIN_WAIT2 进入 TIME_WAIT。
     * 只 close(cfd) 而不让服务端关，客户端会停在 FIN_WAIT2。 */
    {
        char eofbuf[64];
        ssize_t r = recv(afd, eofbuf, sizeof eofbuf, 0);
        printf("  服务端 recv() 返回 %zd（0 = 读到对端 FIN），随后 close(afd) 发自己的 FIN\n", r);
        close(afd);
        afd = -1;
        msleep(250);
    }

    printf("  ss 观测（本机端口 %u）：\n", lport);
    char filter[128];
    snprintf(filter, sizeof filter, "sport = :%u or dport = :%u", lport, lport);
    printf("  期望：客户端这一侧（sport=%u）应显示 TIME-WAIT，服务端一侧已消失\n", lport);
    shell_ss(filter);

    /* 用同一本地 (127.0.0.1:lport) 重绑 —— TIME_WAIT 未过期，默认应失败 */
    printf("  [测试 A] 旧连接未继承 reuse 的情况无法在本进程复现（旧 socket 已设 reuse）；\n"
           "           想看“双向判定”，可把 connect 前的 setsockopt 去掉再编译运行。\n");
    printf("  尝试 bind(127.0.0.1:%u) 而不设 SO_REUSEADDR：\n", lport);
    int f1 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(lport);
    if (bind(f1, (struct sockaddr *)&a, sizeof a) < 0)
        printf("    bind 失败: %s (errno=%d) —— inet_csk_bind_conflict() 判定：\n"
               "    存在同 (addr,port) 的 TIME_WAIT，而本 socket 没有 sk_reuse\n",
               strerror(errno), errno);
    else
        printf("    bind 成功（TIME_WAIT 可能已过期或本机策略允许）\n");
    close(f1);

    printf("  再 bind 同一地址并设置 SO_REUSEADDR：\n");
    int f2 = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(f2, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(f2, (struct sockaddr *)&a, sizeof a) < 0)
        printf("    bind 仍失败: %s (errno=%d)\n", strerror(errno), errno);
    else
        printf("    bind 成功 —— 新 sk 的 sk_reuse = SK_CAN_REUSE，且旧 TIME_WAIT 的\n"
               "                 tw_reuse 也为真，inet_csk_bind_conflict() 才放行\n");
    close(f2);

    long tw1 = tapext("TW");
    printf("  /proc/net/netstat TcpExt: TW = %ld -> %ld (增量 %ld)\n", tw0, tw1, tw1 - tw0);
    printf("  语义：TIME_WAIT 保留 2*MSL；本机 net.ipv4.tcp_fin_timeout 只影响 FIN_WAIT2，\n"
           "        不影响 TIME_WAIT 时长。TIME_WAIT 存在的原因：\n"
           "        (1) 让本连接的迟到报文在网络中消亡，避免污染复用了同一四元组的新连接；\n"
           "        (2) 保证最后那个 ACK 丢失时，对端重传的 FIN 还能被本端回应。\n");
    cat_sysctl("/proc/sys/net/ipv4/tcp_fin_timeout");
    cat_sysctl("/proc/sys/net/ipv4/tcp_tw_reuse");
    cat_sysctl("/proc/sys/net/ipv4/tcp_max_tw_buckets");
    if (afd >= 0)
        close(afd);
    close(lfd);
}

/* 场景 2：SO_LINGER 0 → RST，跳过 TIME_WAIT */
static void scenario2(void)
{
    printf("\n========== 场景 2：SO_LINGER{l_onoff=1,l_linger=0} 触发 RST，双方直接 CLOSED ==========\n");
    unsigned short port = 0;
    int lfd = make_listener(&port);
    if (lfd < 0) { perror("make_listener"); return; }
    printf("监听端口 = %u\n", port);

    int cfd = connect_loopback_reuse(port, 1);
    if (cfd < 0) { perror("connect"); close(lfd); return; }
    int afd = accept(lfd, NULL, NULL);

    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    if (setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) < 0)
        perror("setsockopt SO_LINGER");

    struct sockaddr_in local;
    socklen_t ll = sizeof local;
    getsockname(cfd, (struct sockaddr *)&local, &ll);
    unsigned short lport = ntohs(local.sin_port);

    show_state(cfd, "close 之前（客户端）");
    printf("  close() —— 内核 tcp_close() 因 l_linger==0 走 tcp_send_active_reset()，发 RST\n");
    close(cfd);
    msleep(200);
    /* 服务端此时应已收到 RST：recv 返回 ECONNRESET */
    char buf[16];
    ssize_t r = recv(afd, buf, sizeof buf, 0);
    printf("  服务端 recv() 返回 %zd, errno=%s (%d) —— RST 的直接后果\n",
           r, r < 0 ? strerror(errno) : "n/a", r < 0 ? errno : 0);
    show_state(afd, "服务端在 RST 之后");

    char filter[128];
    snprintf(filter, sizeof filter, "sport = :%u or dport = :%u", lport, lport);
    printf("  ss 观测（应看不到 TIME_WAIT）：\n");
    shell_ss(filter);

    printf("  结论：TIME_WAIT 是“正常四次挥手”的产物；用 RST 终止会跳过它，\n"
           "        代价是可能丢掉尚未被对端确认的数据（tcp_close() 里\n"
           "        “tcp_send_active_reset(sk, GFP_ATOMIC)”的注释也提醒了这一点）。\n");
    close(afd);
    close(lfd);
}

/* 场景 3：观察服务端主动关闭时的状态序列 */
static void scenario3(void)
{
    printf("\n========== 场景 3：四种状态观测（LISTEN / ESTABLISHED / CLOSE_WAIT / LAST_ACK）==========\n");
    unsigned short port = 0;
    int lfd = make_listener(&port);
    if (lfd < 0) { perror("make_listener"); return; }
    show_state(lfd, "监听 socket");
    printf("  监听端口 = %u（此时服务端 close() 会让客户端收到 FIN，客户端进入 CLOSE_WAIT）\n", port);

    int cfd = connect_loopback_reuse(port, 1);
    if (cfd < 0) { perror("connect"); close(lfd); return; }
    int afd = accept(lfd, NULL, NULL);
    show_state(cfd, "客户端 ESTABLISHED");
    show_state(afd, "服务端 ESTABLISHED");

    printf("  服务端 close(afd)：服务端进入 FIN_WAIT1/LAST_ACK 流程\n");
    close(afd);
    msleep(150);
    show_state(cfd, "客户端收到 FIN 后（内核置 CLOSE_WAIT）");

    printf("  客户端不 close()，先读走 EOF：\n");
    char buf[8];
    ssize_t r = recv(cfd, buf, sizeof buf, 0);
    printf("    recv() 返回 %zd（0 = 对端已 FIN，读到 EOF）\n", r);
    show_state(cfd, "读走 EOF 之后仍在 CLOSE_WAIT（等待应用 close）");

    printf("  客户端 close()：进入 LAST_ACK，等服务端 ACK 后 CLOSED\n");
    close(cfd);
    msleep(150);
    if (tcp_state(cfd) < 0)
        printf("  客户端 socket 已销毁（fd 已关闭，无法再 getsockopt）\n");
    close(lfd);
    printf("  状态机速查（与 /proc/net/tcp 的状态码一致）：\n"
           "    01 ESTABLISHED  02 SYN_SENT  03 SYN_RECV  04 FIN_WAIT1  05 FIN_WAIT2\n"
           "    06 TIME_WAIT    07 CLOSE     08 CLOSE_WAIT 09 LAST_ACK  0A LISTEN  0B CLOSING\n");
}

int main(int argc, char **argv)
{
    int only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scenario") && i + 1 < argc)
            only = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--scenario 1|2|3]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    printf("=== timewait_demo ===\n");
    printf("uid=%d  时间=%s", (int)getuid(), ctime(&(time_t){time(NULL)}));
    printf("相关 sysctl（非 root 只读）：\n");
    cat_sysctl("/proc/sys/net/ipv4/tcp_tw_reuse");
    cat_sysctl("/proc/sys/net/ipv4/tcp_fin_timeout");
    cat_sysctl("/proc/sys/net/ipv4/tcp_max_tw_buckets");
    cat_sysctl("/proc/sys/net/ipv4/tcp_syncookies");

    if (only == 0 || only == 1) scenario1();
    if (only == 0 || only == 2) scenario2();
    if (only == 0 || only == 3) scenario3();

    printf("\n=== timewait_demo 结束 ===\n");
    return 0;
}
