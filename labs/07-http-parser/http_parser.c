/*
 * http_parser.c —— 增量式 HTTP/1.1 请求解析器实现
 *
 * 阅读建议：把 http_parser_execute 当成一个「字节驱动的状态机」来读。
 * 它只有三种动作：拼行（直到 '\n'）、按长度消费 body、按 chunk 消费数据。
 * 所有「一个报文的边界在哪里」的判断都落在状态迁移上，这一点和内核里
 * tcp_rcv_established 按 skb 推进 seq 的方式是同一类问题。
 */
#include "http_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ── 小工具 ──────────────────────────────────────────────────────── */

#define HTTP_MIN(a, b) ((a) < (b) ? (a) : (b))

/* RFC 7230 的 tchar：头字段名的合法字符集 */
static int is_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'))
        return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return 1;
    default:
        return 0;
    }
}

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ci_eq_n(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* 在逗号分隔的 token 列表里查找 token（大小写不敏感，忽略 OWS） */
static int list_contains_token(const char *val, size_t vlen, const char *tok)
{
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && (val[i] == ' ' || val[i] == '\t' || val[i] == ','))
            i++;
        size_t start = i;
        while (i < vlen && val[i] != ',') i++;
        size_t end = i;
        while (end > start && (val[end - 1] == ' ' || val[end - 1] == '\t'))
            end--;
        if (end > start && ci_eq_n(val + start, end - start, tok))
            return 1;
    }
    return 0;
}

static int str_contains_ci(const char *hay, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (ci_eq_n(hay + i, nlen, needle)) return 1;
    }
    return 0;
}

/* ── 错误记录：所有失败路径都汇聚到这里 ──────────────────────────── */
static int http_fail(http_parser_t *p, http_error_t code, const char *msg)
{
    p->error = code;
    p->error_msg = msg;
    return -1;
}

/* ── 生命周期 ────────────────────────────────────────────────────── */

void http_parser_init(http_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->max_line         = HTTP_MAX_LINE_DEFAULT;
    p->max_headers      = HTTP_MAX_HEADERS_DEFAULT;
    p->max_header_bytes = 64 * 1024;
    p->max_body         = 8 * 1024 * 1024;
    p->state            = HP_ST_START;
    p->keep_alive       = 1;
    p->line_cap         = p->max_line + 1;
    p->line             = (char *)malloc(p->line_cap);
    if (!p->line) {
        p->error = HTTP_ERR_INTERNAL;
        p->error_msg = "out of memory allocating line buffer";
        p->line_cap = 0;
        return;
    }
    p->line[0] = '\0';
}

static void clear_headers(http_parser_t *p)
{
    for (size_t i = 0; i < p->num_headers; i++) {
        free(p->headers[i].name);
        free(p->headers[i].value);
        p->headers[i].name = NULL;
        p->headers[i].value = NULL;
    }
    p->num_headers = 0;
}

void http_parser_free(http_parser_t *p)
{
    clear_headers(p);
    free(p->target);
    free(p->line);
    free(p->body);
    p->target = NULL;
    p->line = NULL;
    p->body = NULL;
}

/*
 * 同一条 TCP 连接上的下一个消息。
 * 注意：body 缓冲区保留复用（避免每条请求都重新 malloc，对应内核里
 * skb 缓存的思路），只把长度归零。
 */
void http_parser_reset(http_parser_t *p)
{
    clear_headers(p);
    free(p->target);
    p->target = NULL;
    p->target_len = 0;

    p->method = HTTP_METHOD_UNKNOWN;
    p->method_str[0] = '\0';
    p->version_major = p->version_minor = 0;
    p->host = NULL;
    p->host_len = 0;
    p->keep_alive = 1;
    p->expect_continue = 0;
    p->chunked = 0;
    p->has_content_length = 0;
    p->content_length = 0;
    p->missing_host = 0;
    p->saw_obs_fold = 0;

    p->body_len = 0;
    p->body_received = 0;

    p->line_len = 0;
    p->remaining = 0;
    p->crlf_state = 0;
    p->trailers_seen = 0;
    p->header_bytes = 0;

    p->error = HTTP_OK;
    p->error_msg = NULL;
    p->error_offset = 0;
    p->state = HP_ST_START;
}

/* ── body 追加 ───────────────────────────────────────────────────── */

static int body_append(http_parser_t *p, const char *data, size_t n)
{
    if (n == 0) return 0;
    if (p->body_len + n > p->max_body) return -1;   /* 调用方已预检，双保险 */
    if (p->body_len + n + 1 > p->body_cap) {
        size_t cap = p->body_cap ? p->body_cap : 256;
        while (cap < p->body_len + n + 1) {
            if (cap > p->max_body) { cap = p->max_body + 1; break; }
            cap *= 2;
        }
        char *nb = (char *)realloc(p->body, cap);
        if (!nb) return -2;
        p->body = nb;
        p->body_cap = cap;
    }
    memcpy(p->body + p->body_len, data, n);
    p->body_len += n;
    p->body[p->body_len] = '\0';   /* 方便当 C 字符串打印；二进制内容请用 body_len */
    return 0;
}

/* ── 请求行解析 ──────────────────────────────────────────────────── */

static http_method_t method_from_str(const char *s)
{
    if (!strcmp(s, "GET"))     return HTTP_METHOD_GET;
    if (!strcmp(s, "HEAD"))    return HTTP_METHOD_HEAD;
    if (!strcmp(s, "POST"))    return HTTP_METHOD_POST;
    if (!strcmp(s, "PUT"))     return HTTP_METHOD_PUT;
    if (!strcmp(s, "DELETE"))  return HTTP_METHOD_DELETE;
    if (!strcmp(s, "CONNECT")) return HTTP_METHOD_CONNECT;
    if (!strcmp(s, "OPTIONS")) return HTTP_METHOD_OPTIONS;
    if (!strcmp(s, "TRACE"))   return HTTP_METHOD_TRACE;
    if (!strcmp(s, "PATCH"))   return HTTP_METHOD_PATCH;
    return HTTP_METHOD_UNKNOWN;
}

const char *http_method_name(http_method_t m)
{
    switch (m) {
    case HTTP_METHOD_GET:     return "GET";
    case HTTP_METHOD_HEAD:    return "HEAD";
    case HTTP_METHOD_POST:    return "POST";
    case HTTP_METHOD_PUT:     return "PUT";
    case HTTP_METHOD_DELETE:  return "DELETE";
    case HTTP_METHOD_CONNECT: return "CONNECT";
    case HTTP_METHOD_OPTIONS: return "OPTIONS";
    case HTTP_METHOD_TRACE:   return "TRACE";
    case HTTP_METHOD_PATCH:   return "PATCH";
    default:                  return "UNKNOWN";
    }
}

/* p->line 是可写缓冲，直接原地切分 */
static int parse_request_line(http_parser_t *p, size_t n)
{
    char *line = p->line;

    line[n] = '\0';

    /* 1) 方法：到第一个空格为止 */
    size_t i = 0;
    while (i < n && line[i] != ' ') i++;
    if (i == 0 || i >= n) return http_fail(p, HTTP_ERR_BAD_REQUEST_LINE,
                                           "malformed request line");
    if (i >= sizeof(p->method_str)) return http_fail(p, HTTP_ERR_BAD_METHOD,
                                                     "method token too long");
    memcpy(p->method_str, line, i);
    p->method_str[i] = '\0';
    p->method = method_from_str(p->method_str);

    /* 2) 目标：到下一个空格为止 */
    size_t t0 = i + 1;
    while (t0 < n && line[t0] == ' ') t0++;          /* 容错多余空格 */
    if (t0 >= n) return http_fail(p, HTTP_ERR_BAD_REQUEST_LINE,
                                  "missing request target");
    size_t t1 = t0;
    while (t1 < n && line[t1] != ' ') t1++;
    size_t tlen = t1 - t0;
    if (tlen == 0) return http_fail(p, HTTP_ERR_BAD_REQUEST_LINE,
                                    "empty request target");

    free(p->target);
    p->target = (char *)malloc(tlen + 1);
    if (!p->target) return http_fail(p, HTTP_ERR_INTERNAL, "out of memory");
    memcpy(p->target, line + t0, tlen);
    p->target[tlen] = '\0';
    p->target_len = tlen;

    /* 3) 版本：可能是 HTTP/0.9 的「两段式」请求行（没有版本字段） */
    if (t1 >= n) {
        p->version_major = 0;
        p->version_minor = 9;
        p->keep_alive = 0;      /* 0.9 没有连接复用概念 */
        return 0;
    }
    size_t v0 = t1 + 1;
    while (v0 < n && line[v0] == ' ') v0++;
    char *v = line + v0;
    if (strncmp(v, "HTTP/", 5) != 0)
        return http_fail(p, HTTP_ERR_BAD_VERSION, "expected HTTP/x.y");

    char *endp = NULL;
    long maj = strtol(v + 5, &endp, 10);
    if (endp == v + 5 || *endp != '.')
        return http_fail(p, HTTP_ERR_BAD_VERSION, "bad HTTP version");
    char *endp2 = NULL;
    long min = strtol(endp + 1, &endp2, 10);
    if (endp2 == endp + 1)
        return http_fail(p, HTTP_ERR_BAD_VERSION, "bad HTTP version");
    while (*endp2 == ' ' || *endp2 == '\t') endp2++;
    if (*endp2 != '\0')
        return http_fail(p, HTTP_ERR_BAD_VERSION, "trailing junk after version");
    if (maj != 1 || (min != 0 && min != 1))
        return http_fail(p, HTTP_ERR_BAD_VERSION, "only HTTP/1.0 and 1.1 supported");

    p->version_major = (int)maj;
    p->version_minor = (int)min;
    /* HTTP/1.1 默认长连接，HTTP/1.0 默认短连接；Connection 头可覆盖 */
    p->keep_alive = (min == 1);
    return 0;
}

/* ── 头部解析 ────────────────────────────────────────────────────── */

static int header_append_continuation(http_parser_t *p, const char *cont, size_t clen)
{
    if (p->num_headers == 0)
        return http_fail(p, HTTP_ERR_BAD_HEADER, "continuation line without header");
    http_header_t *h = &p->headers[p->num_headers - 1];
    size_t old = strlen(h->value);
    char *nv = (char *)realloc(h->value, old + 1 + clen + 1);
    if (!nv) return http_fail(p, HTTP_ERR_INTERNAL, "out of memory");
    nv[old] = ' ';                       /* RFC 7230: 折行等价于一个空格 */
    memcpy(nv + old + 1, cont, clen);
    nv[old + 1 + clen] = '\0';
    h->value = nv;
    p->saw_obs_fold = 1;
    return 0;
}

static int parse_header_line(http_parser_t *p, size_t n)
{
    char *line = p->line;
    line[n] = '\0';

    /* obs-fold：以 SP/HTAB 开头的行是上一个头的续行（已废弃，但仍要兼容） */
    if (n > 0 && (line[0] == ' ' || line[0] == '\t')) {
        const char *cont = line;
        size_t clen = n;
        while (clen && (*cont == ' ' || *cont == '\t')) { cont++; clen--; }
        return header_append_continuation(p, cont, clen);
    }

    char *colon = (char *)memchr(line, ':', n);
    if (!colon)
        return http_fail(p, HTTP_ERR_BAD_HEADER, "header line without ':'");

    size_t nlen = (size_t)(colon - line);
    if (nlen == 0)
        return http_fail(p, HTTP_ERR_BAD_HEADER, "empty header name");
    for (size_t k = 0; k < nlen; k++) {
        if (!is_tchar((unsigned char)line[k]))
            return http_fail(p, HTTP_ERR_BAD_HEADER, "illegal char in header name");
    }

    char *val = colon + 1;
    size_t vlen = n - nlen - 1;
    while (vlen && (*val == ' ' || *val == '\t')) { val++; vlen--; }
    while (vlen && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) vlen--;

    if (p->num_headers >= p->max_headers)
        return http_fail(p, HTTP_ERR_TOO_MANY_HEADERS, "too many headers");
    p->header_bytes += nlen + vlen;
    if (p->header_bytes > p->max_header_bytes)
        return http_fail(p, HTTP_ERR_HEADERS_TOO_LARGE, "header section too large");

    http_header_t *h = &p->headers[p->num_headers];
    h->name = (char *)malloc(nlen + 1);
    h->value = (char *)malloc(vlen + 1);
    if (!h->name || !h->value) {
        free(h->name); free(h->value);
        h->name = h->value = NULL;
        return http_fail(p, HTTP_ERR_INTERNAL, "out of memory");
    }
    memcpy(h->name, line, nlen);   h->name[nlen] = '\0';
    memcpy(h->value, val, vlen);   h->value[vlen] = '\0';
    p->num_headers++;

    /* ── 语义字段：这些决定「body 有多长、连接还能不能用」 ── */

    if (ci_eq_n(line, nlen, "content-length")) {
        if (p->has_content_length)
            return http_fail(p, HTTP_ERR_DUPLICATE_CONTENT_LENGTH,
                             "duplicate Content-Length");
        if (vlen == 0)
            return http_fail(p, HTTP_ERR_BAD_CONTENT_LENGTH, "empty Content-Length");
        unsigned long long v = 0;
        for (size_t k = 0; k < vlen; k++) {
            if (val[k] < '0' || val[k] > '9')
                return http_fail(p, HTTP_ERR_BAD_CONTENT_LENGTH,
                                 "non-digit in Content-Length");
            if (v > (ULLONG_MAX - (unsigned long long)(val[k] - '0')) / 10ULL)
                return http_fail(p, HTTP_ERR_BAD_CONTENT_LENGTH,
                                 "Content-Length overflow");
            v = v * 10ULL + (unsigned long long)(val[k] - '0');
        }
        p->has_content_length = 1;
        p->content_length = v;
    } else if (ci_eq_n(line, nlen, "transfer-encoding")) {
        if (str_contains_ci(val, vlen, "chunked"))
            p->chunked = 1;
    } else if (ci_eq_n(line, nlen, "connection")) {
        if (list_contains_token(val, vlen, "close"))
            p->keep_alive = 0;
        else if (list_contains_token(val, vlen, "keep-alive"))
            p->keep_alive = 1;
    } else if (ci_eq_n(line, nlen, "expect")) {
        if (list_contains_token(val, vlen, "100-continue"))
            p->expect_continue = 1;
    } else if (ci_eq_n(line, nlen, "host")) {
        p->host = h->value;
        p->host_len = vlen;
    }
    return 0;
}

const char *http_parser_get_header(const http_parser_t *p, const char *name)
{
    for (size_t i = 0; i < p->num_headers; i++) {
        if (ci_eq_n(p->headers[i].name, strlen(p->headers[i].name), name))
            return p->headers[i].value;
    }
    return NULL;
}

/* 头部结束：决定接下来读什么 */
static int finish_headers(http_parser_t *p)
{
    /*
     * RFC 7230 §3.3.3：同时出现 Transfer-Encoding 与 Content-Length 时，
     * 必须视为错误（或忽略 CL）。这是 HTTP 请求走私（request smuggling）
     * 的经典入口——上游代理按 CL 切分、下游按 chunked 切分，两边看到不同的
     * 报文边界。这里选择直接拒绝，是最安全的做法。
     */
    if (p->chunked && p->has_content_length)
        return http_fail(p, HTTP_ERR_CONTENT_LENGTH_WITH_CHUNKED,
                         "both Transfer-Encoding and Content-Length present");

    if (p->version_major == 1 && p->version_minor == 1 && !p->host)
        p->missing_host = 1;   /* RFC 7230 要求 1.1 必须带 Host；教学场景只告警 */

    if (p->chunked) {
        p->state = HP_ST_CHUNK_SIZE;
        p->line_len = 0;
        return 0;
    }
    if (p->has_content_length) {
        if (p->content_length > p->max_body)
            return http_fail(p, HTTP_ERR_BODY_TOO_LARGE, "body exceeds max_body");
        if (p->content_length == 0) {
            p->state = HP_ST_DONE;
            return 1;          /* 没有 body，消息已完成 */
        }
        p->remaining = p->content_length;
        p->state = HP_ST_BODY_CL;
        return 0;
    }
    /* 既没有 CL 也没有 chunked：请求按定义没有 body */
    p->state = HP_ST_DONE;
    return 1;
}

/* chunk-size 行："5" / "1a;ext=val" / "0" */
static int parse_chunk_size(http_parser_t *p, size_t n)
{
    char *line = p->line;
    line[n] = '\0';

    unsigned long long sz = 0;
    size_t digits = 0;
    while (digits < n) {
        int d = hexval((unsigned char)line[digits]);
        if (d < 0) break;
        if (sz > (ULLONG_MAX >> 4))
            return http_fail(p, HTTP_ERR_BAD_CHUNK_SIZE, "chunk size overflow");
        sz = (sz << 4) | (unsigned long long)d;
        digits++;
    }
    if (digits == 0)
        return http_fail(p, HTTP_ERR_BAD_CHUNK_SIZE, "missing chunk size");
    if (digits < n && line[digits] != ';')
        return http_fail(p, HTTP_ERR_BAD_CHUNK_SIZE, "junk after chunk size");

    if (sz == 0) {                 /* 最后一个 chunk，后面是 trailer */
        p->state = HP_ST_TRAILERS;
        p->line_len = 0;
        return 0;
    }
    if (p->body_received + sz > p->max_body)
        return http_fail(p, HTTP_ERR_BODY_TOO_LARGE, "chunked body exceeds max_body");
    p->remaining = sz;
    p->state = HP_ST_CHUNK_DATA;
    return 0;
}

/* 处理一条完整行（已剥掉结尾的 CRLF/LF） */
static int handle_line(http_parser_t *p)
{
    size_t n = p->line_len;

    /* 去掉行尾的 '\r'，同时把「行内出现裸 CR 或 NUL」视为非法 */
    if (n > 0 && p->line[n - 1] == '\r') n--;
    for (size_t k = 0; k < n; k++) {
        if (p->line[k] == '\r')
            return http_fail(p, HTTP_ERR_BAD_HEADER, "bare CR inside line");
        if (p->line[k] == '\0')
            return http_fail(p, HTTP_ERR_BAD_HEADER, "NUL inside line");
    }

    switch (p->state) {
    case HP_ST_REQUEST_LINE:
        if (parse_request_line(p, n) < 0) return -1;
        p->line_len = 0;
        if (p->version_major == 0 && p->version_minor == 9) {
            p->state = HP_ST_DONE;    /* HTTP/0.9：没有头部，请求到此结束 */
            return 1;
        }
        p->state = HP_ST_HEADERS;
        p->header_bytes = 0;
        return 0;

    case HP_ST_HEADERS:
        if (n == 0) {                 /* 空行 = 头部结束 */
            p->line_len = 0;
            return finish_headers(p);
        }
        if (parse_header_line(p, n) < 0) return -1;
        p->line_len = 0;
        return 0;

    case HP_ST_CHUNK_SIZE:
        if (parse_chunk_size(p, n) < 0) return -1;
        p->line_len = 0;
        return 0;

    case HP_ST_TRAILERS:
        p->line_len = 0;
        if (n == 0) {                 /* trailer 结束 = 消息结束 */
            p->state = HP_ST_DONE;
            return 1;
        }
        p->trailers_seen++;           /* 教学实现只计数，不解析 trailer 内容 */
        return 0;

    default:
        return http_fail(p, HTTP_ERR_INTERNAL, "unexpected state");
    }
}

/* ── 主状态机 ────────────────────────────────────────────────────── */

http_parse_status_t http_parser_execute(http_parser_t *p, const char *data,
                                        size_t len, size_t *consumed)
{
    size_t i = 0;

    if (consumed) *consumed = 0;

    if (!p->line && p->line_cap == 0) {           /* init 失败的残留 */
        p->error = HTTP_ERR_INTERNAL;
        p->error_msg = "parser not initialized";
        return HTTP_PARSE_ERROR;
    }
    /* 上一条消息还没被 reset 就继续喂：不消费任何字节，让调用者先处理 */
    if (p->state == HP_ST_DONE)
        return HTTP_PARSE_COMPLETE;

    while (i < len) {
        switch (p->state) {
        case HP_ST_START:
            p->state = HP_ST_REQUEST_LINE;
            p->line_len = 0;
            break;

        /* ── 三种「按行收集」的状态 ── */
        case HP_ST_REQUEST_LINE:
        case HP_ST_HEADERS:
        case HP_ST_CHUNK_SIZE:
        case HP_ST_TRAILERS: {
            unsigned char c = (unsigned char)data[i++];
            p->error_offset = i - 1;
            if (p->line_len >= p->max_line) {
                http_fail(p, HTTP_ERR_LINE_TOO_LONG, "line exceeds max_line");
                if (consumed) *consumed = i;
                return HTTP_PARSE_ERROR;
            }
            if (c == '\n') {
                int r = handle_line(p);   /* 内部负责按需清零 line_len */
                if (r < 0) {
                    if (consumed) *consumed = i;
                    return HTTP_PARSE_ERROR;
                }
                if (r > 0) {
                    p->messages_parsed++;
                    if (consumed) *consumed = i;
                    return HTTP_PARSE_COMPLETE;
                }
            } else {
                p->line[p->line_len++] = (char)c;
            }
            break;
        }

        /* ── 按长度消费 body：Content-Length 与 chunk 数据共用这条路径 ── */
        case HP_ST_BODY_CL:
        case HP_ST_CHUNK_DATA: {
            size_t avail = len - i;
            size_t want = (p->remaining < (unsigned long long)avail)
                          ? (size_t)p->remaining : avail;
            p->error_offset = i;
            int r = body_append(p, data + i, want);
            if (r == -1) {
                http_fail(p, HTTP_ERR_BODY_TOO_LARGE, "body exceeds max_body");
                if (consumed) *consumed = i;
                return HTTP_PARSE_ERROR;
            }
            if (r == -2) {
                http_fail(p, HTTP_ERR_INTERNAL, "out of memory growing body");
                if (consumed) *consumed = i;
                return HTTP_PARSE_ERROR;
            }
            i += want;
            p->remaining -= want;
            p->body_received += want;
            if (p->remaining == 0) {
                if (p->state == HP_ST_BODY_CL) {
                    p->state = HP_ST_DONE;
                    p->messages_parsed++;
                    if (consumed) *consumed = i;
                    return HTTP_PARSE_COMPLETE;
                }
                p->state = HP_ST_CHUNK_CRLF;
                p->crlf_state = 0;
            }
            break;
        }

        /* ── chunk 数据后必须跟 CRLF ── */
        case HP_ST_CHUNK_CRLF: {
            unsigned char c = (unsigned char)data[i];
            p->error_offset = i;
            if (p->crlf_state == 0) {
                if (c == '\r') {
                    p->crlf_state = 1;
                    i++;
                } else if (c == '\n') {      /* 容忍 bare LF */
                    i++;
                    p->state = HP_ST_CHUNK_SIZE;
                    p->line_len = 0;
                } else {
                    http_fail(p, HTTP_ERR_BAD_CHUNK_TERMINATOR,
                              "expected CRLF after chunk data");
                    if (consumed) *consumed = i;
                    return HTTP_PARSE_ERROR;
                }
            } else {
                if (c != '\n') {
                    http_fail(p, HTTP_ERR_BAD_CHUNK_TERMINATOR,
                              "expected LF after CR");
                    if (consumed) *consumed = i;
                    return HTTP_PARSE_ERROR;
                }
                i++;
                p->crlf_state = 0;
                p->state = HP_ST_CHUNK_SIZE;
                p->line_len = 0;
            }
            break;
        }

        case HP_ST_DONE:
            if (consumed) *consumed = i;
            return HTTP_PARSE_COMPLETE;

        default:
            http_fail(p, HTTP_ERR_INTERNAL, "unknown parser state");
            if (consumed) *consumed = i;
            return HTTP_PARSE_ERROR;
        }
    }

    if (consumed) *consumed = i;
    return HTTP_PARSE_NEED_MORE;
}

/* ── 错误描述 ────────────────────────────────────────────────────── */

const char *http_parser_strerror(const http_parser_t *p)
{
    if (p->error == HTTP_OK) return "no error";
    return p->error_msg ? p->error_msg : "unknown error";
}
