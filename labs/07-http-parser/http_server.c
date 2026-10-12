/*
 * http_server.c —— 用自研解析器实现的最小 HTTP/1.1 服务器（epoll + 非阻塞）
 *
 * 这个程序存在的意义，不是做一个能上线的 Web 服务器，而是把「内核协议栈」和
 * 「应用层协议」的边界摆在你面前：
 *
 *   1. 内核 TCP 只给你**字节流**。recv() 返回的 n 个字节与 HTTP 报文的边界毫无关系。
 *      所以本程序必须自己维护「每连接一个解析器状态」，跨 recv 调用记住解析位置。
 *      这就是 struct conn 里 http_parser_t parser 的作用——对应内核里
 *      struct tcp_sock 跨 skb 记住 seq/rcv_nxt 的方式。
 *
 *   2. keep-alive 让「一条 TCP 连接」承载「多条 HTTP 消息」。内核完全不知道这件事，
 *      它只看到连续的字节。所以解析器必须能被 reset 并接着解析下一条消息。
 *
 *   3. 慢速攻击（Slowloris）之所以有效，是因为内核会忠实地把「慢慢发来的头」放进
 *      接收缓冲区。防线必须在应用层（超时 + 上限），内核能帮的只有 tcp_rmem / 
 *      somaxconn / backlog 这些「总量控制」。本程序用 idle 超时 + max_line +
 *      max_header_bytes + max_body 来做这件事。
 *
 *   4. TCP_NODELAY：HTTP 响应通常是小包，Nagle 算法 + 延迟 ACK 会造成几十毫秒的
 *      额外延迟（经典交互问题）。服务器通常在 accept 后就设 TCP_NODELAY。
 */
#define _GNU_SOURCE
#include "http_parser.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS        256
#define RECV_BUF         65536
#define OUT_BUF_MAX      (8u * 1024 * 1024)
#define IDLE_TIMEOUT_SEC 10
#define BIG_MAX          (4u * 1024 * 1024)

struct conn {
    int            fd;
    http_parser_t  parser;
    char          *out;      /* 待发送的响应字节（可能有多个响应排队） */
    size_t         out_len;
    size_t         out_off;
    size_t         out_cap;
    int            want_close;
    int            sent_100; /* 是否已回过 100 Continue */
    int            epoll_out;/* 是否已注册 EPOLLOUT */
    time_t         last_active;
    char           peer[64];
};

static int              g_epfd = -1;
static int              g_listen_fd = -1;
static struct conn     *g_conns[MAX_CONNS];
static long             g_requests = 0;  /* 成功处理的请求数（退出时汇总） */
static long             g_reqno    = 0;  /* 收到的请求序号（含解析失败的） */
static long             g_bytes_in = 0;

/* ── 输出缓冲：所有响应先攒进内存，再尽量一次写出 ───────────────── */

static int ob_reserve(struct conn *c, size_t need)
{
    if (c->out_len + need <= c->out_cap) return 0;
    if (c->out_len + need > OUT_BUF_MAX) return -1;
    size_t cap = c->out_cap ? c->out_cap : 4096;
    while (cap < c->out_len + need) cap *= 2;
    char *nb = (char *)realloc(c->out, cap);
    if (!nb) return -1;
    c->out = nb;
    c->out_cap = cap;
    return 0;
}

static void ob_append(struct conn *c, const char *data, size_t len)
{
    if (ob_reserve(c, len) < 0) { c->want_close = 1; return; }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
}

static void ob_printf(struct conn *c, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) ob_append(c, tmp, (size_t)((size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1));
}

/* HTTP 响应不需要 RFC 1123 之外的精度，这里用最简单可靠的格式 */
static void http_date(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static const char *status_text(int code)
{
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Error";
    }
}

/*
 * 组装响应头。注意 Content-Length 与 chunked 二选一：
 *   - 已知长度 → Content-Length（客户端据此知道消息边界）
 *   - 未知长度 → Transfer-Encoding: chunked（长度信息编码在数据流里）
 * 内核对此一无所知，消息边界完全是这一层的约定。
 */
static void resp_begin(struct conn *c, int code, int with_length, size_t length)
{
    char date[64];
    http_date(date, sizeof(date));
    ob_printf(c, "HTTP/1.1 %d %s\r\n", code, status_text(code));
    ob_printf(c, "Date: %s\r\n", date);
    ob_printf(c, "Server: learn-net-httpd/1.0\r\n");
    ob_printf(c, "Connection: %s\r\n", c->want_close ? "close" : "keep-alive");
    if (with_length)
        ob_printf(c, "Content-Length: %zu\r\n", length);
}

static void resp_finish_headers(struct conn *c)
{
    ob_append(c, "\r\n", 2);
}

/* ── 路由 ────────────────────────────────────────────────────────── */

static const char *INDEX_TEXT =
    "learn-net http/1.1 教学服务器\n"
    "\n"
    "可用路由：\n"
    "  GET  /                 本页\n"
    "  GET  /hello            最小响应\n"
    "  GET  /headers          回显解析出的请求行与全部头部（看解析器结果）\n"
    "  GET  /chunked          用 Transfer-Encoding: chunked 回响应\n"
    "  GET  /big?n=1048576    生成 n 字节响应（默认 65536，上限 4MiB）\n"
    "  POST /echo             回显请求体（按 Content-Length 或 chunked 解析）\n"
    "  HEAD /hello            只有头部没有 body\n"
    "\n"
    "观测建议：另开终端执行 `ss -tni` 看这条连接的 cwnd/rtt，\n"
    "或 `cat /proc/net/tcp` 看五元组与接收队列。\n";

static int query_int(const char *target, const char *key, long defval)
{
    size_t klen = strlen(key);
    const char *q = strchr(target, '?');
    while (q) {
        q++;
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            return (int)strtol(q + klen + 1, NULL, 10);
        }
        q = strchr(q, '&');
    }
    return (int)defval;
}

static void handle_headers_dump(struct conn *c)
{
    http_parser_t *p = &c->parser;
    ob_printf(c, "请求行: %s %s HTTP/%d.%d\n",
              p->method_str, p->target ? p->target : "-",
              p->version_major, p->version_minor);
    ob_printf(c, "keep_alive=%d chunked=%d content_length=%llu body_len=%zu\n",
              p->keep_alive, p->chunked, p->content_length, p->body_len);
    ob_printf(c, "missing_host=%d obs_fold=%d trailers=%zu\n\n",
              p->missing_host, p->saw_obs_fold, p->trailers_seen);
    ob_printf(c, "头部 %zu 个(按解析顺序):\n", p->num_headers);
    for (size_t i = 0; i < p->num_headers; i++)
        ob_printf(c, "  [%2zu] %-24s = %s\n", i, p->headers[i].name, p->headers[i].value);
    ob_printf(c, "\n提示: 头名按 RFC 7230 不区分大小写，"
                 "http_parser_get_header(\"HOST\") 也能取到值。\n");
}

static void handle_chunked(struct conn *c)
{
    /* 故意用 chunked：长度在数据流里，而不是头部 */
    resp_begin(c, 200, 0, 0);
    ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
    ob_printf(c, "Transfer-Encoding: chunked\r\n");
    resp_finish_headers(c);

    const char *parts[3] = {
        "第一块：内核只提供字节流，chunked 把长度写进数据流。\n",
        "第二块：每块格式为 <hex 长度>CRLF<数据>CRLF。\n",
        "第三块：以 0CRLFCRLF 结束。\n"
    };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(parts[i]);
        ob_printf(c, "%zx\r\n", n);
        ob_append(c, parts[i], n);
        ob_append(c, "\r\n", 2);
    }
    ob_append(c, "0\r\n\r\n", 5);
}

static void handle_big(struct conn *c)
{
    long n = query_int(c->parser.target ? c->parser.target : "/big", "n", 65536);
    if (n < 0) n = 0;
    if ((unsigned long)n > BIG_MAX) n = BIG_MAX;

    resp_begin(c, 200, 1, (size_t)n);
    ob_printf(c, "Content-Type: application/octet-stream\r\n");
    resp_finish_headers(c);
    if (c->parser.method == HTTP_METHOD_HEAD) return;   /* HEAD 只回头 */

    /* 逐行填充可读内容，便于观察大响应在 TCP 上的分片 */
    char line[80];
    long written = 0, lineno = 0;
    while (written < n) {
        int len = snprintf(line, sizeof(line), "line %08ld ........................\n", lineno++);
        if (len <= 0) break;
        size_t take = (size_t)len;
        if (written + (long)take > n) take = (size_t)(n - written);
        ob_append(c, line, take);
        written += (long)take;
    }
}

static void handle_echo(struct conn *c)
{
    http_parser_t *p = &c->parser;
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "收到 %zu 字节请求体（方式：%s）\n\n",
                     p->body_len, p->chunked ? "chunked" : "Content-Length");
    size_t total = (size_t)n + p->body_len;

    resp_begin(c, 200, 1, total);
    ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
    resp_finish_headers(c);
    ob_append(c, head, (size_t)n);
    if (p->body_len) ob_append(c, p->body, p->body_len);
}

static void handle_request(struct conn *c)
{
    http_parser_t *p = &c->parser;
    const char *t = p->target ? p->target : "/";

    g_requests++;
    long no = ++g_reqno;
    fprintf(stdout, "[req %ld] %s %s HTTP/%d.%d  headers=%zu body=%zu keep_alive=%d\n",
            no, p->method_str, t, p->version_major, p->version_minor,
            p->num_headers, p->body_len, p->keep_alive);
    fflush(stdout);

    if (!p->keep_alive)
        c->want_close = 1;

    /* 精确匹配路径（忽略 query） */
    size_t tlen = strcspn(t, "?");
    char path[512];
    if (tlen >= sizeof(path)) tlen = sizeof(path) - 1;
    memcpy(path, t, tlen);
    path[tlen] = '\0';

    if (!strcmp(path, "/")) {
        size_t len = strlen(INDEX_TEXT);
        resp_begin(c, 200, 1, len);
        ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
        resp_finish_headers(c);
        if (p->method != HTTP_METHOD_HEAD) ob_append(c, INDEX_TEXT, len);
    } else if (!strcmp(path, "/hello")) {
        const char *body = "hello, world\n";
        size_t len = strlen(body);
        resp_begin(c, 200, 1, len);
        ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
        resp_finish_headers(c);
        if (p->method != HTTP_METHOD_HEAD) ob_append(c, body, len);
    } else if (!strcmp(path, "/headers")) {
        /* 先把内容写进一个临时缓冲以计算 Content-Length？为教学简洁，
           这里直接用一个足够大的结构：先算长度不方便，改用 chunked。 */
        resp_begin(c, 200, 0, 0);
        ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
        ob_printf(c, "Transfer-Encoding: chunked\r\n");
        resp_finish_headers(c);
        /* 复用输出缓冲的技巧：先在末尾生成内容，再整体包成 chunk */
        size_t start = c->out_len;
        handle_headers_dump(c);
        size_t body_len = c->out_len - start;
        char hdr[32];
        int hl = snprintf(hdr, sizeof(hdr), "%zx\r\n", body_len);
        /* 在正文前插入 chunk 头：把正文整体后移 */
        if (ob_reserve(c, (size_t)hl + 5) == 0) {
            memmove(c->out + start + hl, c->out + start, body_len);
            memcpy(c->out + start, hdr, (size_t)hl);
            c->out_len += (size_t)hl;
            ob_append(c, "\r\n0\r\n\r\n", 7);
        }
    } else if (!strcmp(path, "/chunked")) {
        handle_chunked(c);
    } else if (!strcmp(path, "/big")) {
        handle_big(c);
    } else if (!strcmp(path, "/echo")) {
        if (p->method != HTTP_METHOD_POST && p->method != HTTP_METHOD_PUT) {
            const char *b = "请用 POST 调用 /echo\n";
            resp_begin(c, 501, 1, strlen(b));
            ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
            resp_finish_headers(c);
            ob_append(c, b, strlen(b));
        } else {
            handle_echo(c);
        }
    } else {
        char body[256];
        int n = snprintf(body, sizeof(body), "404 Not Found: %s\n", path);
        resp_begin(c, 404, 1, (size_t)n);
        ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
        resp_finish_headers(c);
        ob_append(c, body, (size_t)n);
    }
}

/* 解析错误 → 合适的 4xx 状态码，然后关闭连接（报文边界已不可信） */
static void handle_parse_error(struct conn *c)
{
    int code = 400;
    switch (c->parser.error) {
    case HTTP_ERR_BODY_TOO_LARGE:       code = 413; break;
    case HTTP_ERR_LINE_TOO_LONG:        code = 414; break;
    case HTTP_ERR_TOO_MANY_HEADERS:
    case HTTP_ERR_HEADERS_TOO_LARGE:    code = 431; break;
    default:                            code = 400; break;
    }
    fprintf(stdout, "[req %ld] 解析失败: %s（error=%d），回 %d 并关闭\n",
            ++g_reqno, http_parser_strerror(&c->parser),
            (int)c->parser.error, code);
    fflush(stdout);

    c->want_close = 1;
    char body[256];
    int n = snprintf(body, sizeof(body), "%d %s\n原因: %s\n", code,
                     status_text(code), http_parser_strerror(&c->parser));
    /* 清掉可能已经排队的部分响应，只回错误 */
    c->out_len = c->out_off = 0;
    resp_begin(c, code, 1, (size_t)n);
    ob_printf(c, "Content-Type: text/plain; charset=utf-8\r\n");
    resp_finish_headers(c);
    ob_append(c, body, (size_t)n);
}

/* ── 连接管理 ────────────────────────────────────────────────────── */

static void conn_close(struct conn *c)
{
    if (!c) return;
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    http_parser_free(&c->parser);
    free(c->out);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i] == c) { g_conns[i] = NULL; break; }
    }
    free(c);
}

static void conn_update_events(struct conn *c)
{
    int want_out = (c->out_off < c->out_len);
    if (want_out == c->epoll_out) return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP;
    if (want_out) ev.events |= EPOLLOUT;
    ev.data.ptr = c;
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
    c->epoll_out = want_out;
}

/* 尽量把输出缓冲写出去；返回 0 正常，-1 应关闭连接 */
static int conn_flush(struct conn *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            conn_update_events(c);
            return 0;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    /* 全部写完：复位缓冲，准备下一条响应 */
    c->out_len = c->out_off = 0;
    conn_update_events(c);
    if (c->want_close) return -1;
    return 0;
}

static int body_pending(const http_parser_t *p)
{
    switch (p->state) {
    case HP_ST_BODY_CL:
    case HP_ST_CHUNK_SIZE:
    case HP_ST_CHUNK_DATA:
    case HP_ST_CHUNK_CRLF:
    case HP_ST_TRAILERS:
        return 1;
    default:
        return 0;
    }
}

/* 返回 0 正常，-1 应关闭 */
static int conn_on_readable(struct conn *c)
{
    char buf[RECV_BUF];

    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            g_bytes_in += n;
            c->last_active = time(NULL);
            size_t off = 0;
            while (off < (size_t)n) {
                size_t consumed = 0;
                http_parse_status_t st = http_parser_execute(
                    &c->parser, buf + off, (size_t)n - off, &consumed);
                off += consumed;

                if (st == HTTP_PARSE_ERROR) {
                    handle_parse_error(c);
                    return 0;                 /* 让 flush 把 4xx 写出去 */
                }
                if (st == HTTP_PARSE_COMPLETE) {
                    handle_request(c);
                    http_parser_reset(&c->parser);
                    c->sent_100 = 0;
                    if (c->want_close) return 0;
                    continue;
                }
                /* NEED_MORE：如果客户端在等 100-continue，现在回它 */
                if (c->parser.expect_continue && !c->sent_100 && body_pending(&c->parser)) {
                    ob_append(c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
                    c->sent_100 = 1;
                }
                break;
            }
            continue;                          /* 继续读，直到 EAGAIN */
        }
        if (n == 0) return -1;                 /* 对端关闭 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

static void conn_accept(void)
{
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    int fd = accept4(g_listen_fd, (struct sockaddr *)&sa, &slen,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;

    struct conn *c = (struct conn *)calloc(1, sizeof(*c));
    if (!c) { close(fd); return; }
    c->fd = fd;
    c->last_active = time(NULL);
    http_parser_init(&c->parser);
    snprintf(c->peer, sizeof(c->peer), "%s:%u", inet_ntoa(sa.sin_addr),
             (unsigned)ntohs(sa.sin_port));

    /* HTTP 响应多为小包：关掉 Nagle，避免与延迟 ACK 叠加出几十毫秒延迟 */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* 慢速攻击防护之一：限制单连接内核接收缓冲，减少被占用的内存 */
    int rcvbuf = 64 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int slot = -1;
    for (int i = 0; i < MAX_CONNS; i++)
        if (!g_conns[i]) { slot = i; break; }
    if (slot < 0) {
        fprintf(stderr, "连接数已达上限 %d，拒绝 %s\n", MAX_CONNS, c->peer);
        close(fd);
        http_parser_free(&c->parser);
        free(c);
        return;
    }
    g_conns[slot] = c;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.ptr = c;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD");
        g_conns[slot] = NULL;
        close(fd);
        http_parser_free(&c->parser);
        free(c);
        return;
    }
    fprintf(stdout, "[accept] %s (fd=%d)\n", c->peer, fd);
    fflush(stdout);
}

static void conn_sweep_idle(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CONNS; i++) {
        struct conn *c = g_conns[i];
        if (!c) continue;
        if (now - c->last_active > IDLE_TIMEOUT_SEC) {
            /* Slowloris：客户端慢慢发头，内核会老实缓存，应用层必须超时踢掉 */
            fprintf(stdout, "[timeout] %s 空闲 %lds，关闭（慢速攻击防护）\n",
                    c->peer, (long)(now - c->last_active));
            fflush(stdout);
            conn_close(c);
        }
    }
}

/* ── 启动 ────────────────────────────────────────────────────────── */

static void on_signal(int sig)
{
    (void)sig;
    if (g_listen_fd >= 0) close(g_listen_fd);
    fprintf(stdout, "\n[exit] 共处理 %ld 个请求，收到 %ld 字节\n",
            g_requests, g_bytes_in);
    fflush(stdout);
    exit(0);
}

int main(int argc, char **argv)
{
    int port = 8080;
    const char *bind_addr = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else {
            fprintf(stderr, "用法: %s [--port N] [--bind IP]\n", argv[0]);
            return 2;
        }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "非法端口\n"); return 2; }

    signal(SIGPIPE, SIG_IGN);       /* 别让对端关闭把进程打死（写用 MSG_NOSIGNAL 兜底） */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_listen_fd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "非法地址: %s\n", bind_addr);
        return 1;
    }
    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        return 1;
    }
    /* backlog 就是内核的「全连接队列」上限，受 /proc/sys/net/core/somaxconn 限制 */
    if (listen(g_listen_fd, 128) < 0) { perror("listen"); return 1; }

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;                 /* NULL 代表监听套接字 */
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0) {
        perror("epoll_ctl listen");
        return 1;
    }

    printf("learn-net httpd 监听 http://%s:%d/  (pid=%d)\n", bind_addr, port, getpid());
    printf("Ctrl-C 退出；建议另开终端执行 `ss -tni` 观察内核侧连接状态\n");
    fflush(stdout);

    struct epoll_event events[64];
    for (;;) {
        int n = epoll_wait(g_epfd, events, 64, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            struct conn *c = (struct conn *)events[i].data.ptr;
            if (!c) { conn_accept(); continue; }

            uint32_t e = events[i].events;
            if (e & EPOLLOUT) {
                if (conn_flush(c) < 0) { conn_close(c); continue; }
            }
            if (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                if (conn_on_readable(c) < 0) { conn_close(c); continue; }
                if (c->out_off < c->out_len) {
                    if (conn_flush(c) < 0) { conn_close(c); continue; }
                }
            }
        }
        if (n >= 0) conn_sweep_idle();
    }

    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i]) conn_close(g_conns[i]);
    close(g_listen_fd);
    close(g_epfd);
    return 0;
}
