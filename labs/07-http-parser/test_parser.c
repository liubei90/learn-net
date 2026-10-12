/*
 * test_parser.c —— http_parser 的表驱动测试
 *
 * 测试的核心思想（也是本实验最重要的训练）：
 *   **同一个报文，无论被切成怎样的片段喂进来，解析结果必须完全一致。**
 *   TCP 是字节流，内核给你的数据边界与你写的 HTTP 边界毫无关系：
 *   一个请求可能被拆在 3 个 skb 里，也可能 3 个请求挤在 1 个 skb 里。
 *
 *   所以每个用例都会用三种喂法各跑一遍：
 *     MODE_WHOLE   —— 一次喂完整报文（理想情况）
 *     MODE_BYTE    —— 每次只喂 1 字节（最恶劣的分片）
 *     MODE_SPLITS  —— 在每一个可能的位置切一刀（穷举所有分片点）
 *   三者结果必须一致，否则就是状态机有 bug。
 */
#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 极简断言框架 ────────────────────────────────────────────────── */
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("    \033[31m✗ FAIL\033[0m ");                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* ── 用例描述 ────────────────────────────────────────────────────── */
typedef struct {
    const char        *name;
    const char        *data;
    size_t             len;              /* 0 表示用 strlen(data) */
    http_parse_status_t expect_status;
    http_error_t       expect_error;

    /* 期望解析出的字段（仅在 complete 时有意义） */
    http_method_t      method;
    const char        *target;
    int                keep_alive;
    int                chunked;
    int                has_content_length;
    unsigned long long content_length;
    const char        *body;             /* 期望 body（可为 NULL 表示空） */
    const char        *lookup_header;    /* 可选：查一个头 */
    const char        *lookup_value;

    /* 可选：覆盖解析器上限 */
    size_t             max_body;
    size_t             max_headers;
} testcase_t;

/* ── 分片喂入 ────────────────────────────────────────────────────── */

/*
 * ends[] 是每一次 execute 的「本片段结束偏移」。
 * 例如 ends={5,10} 表示先喂 data[0..5)，再喂 data[5..10)。
 */
static http_parse_status_t feed_with_bounds(http_parser_t *p, const char *data,
                                            const size_t *ends,
                                            size_t nends, size_t *consumed_total)
{
    size_t off = 0, total = 0;
    http_parse_status_t st = HTTP_PARSE_NEED_MORE;

    for (size_t i = 0; i < nends; i++) {
        size_t n = ends[i] - off;
        size_t c = 0;
        st = http_parser_execute(p, data + off, n, &c);
        total += c;
        if (st != HTTP_PARSE_NEED_MORE) break;
        if (c != n) break;             /* 还没完成却没吃干净：状态机有问题 */
        off = ends[i];
    }
    *consumed_total = total;
    return st;
}

static int body_equals(const http_parser_t *p, const char *expect)
{
    size_t elen = expect ? strlen(expect) : 0;
    if (p->body_len != elen) return 0;
    if (elen == 0) return 1;
    return memcmp(p->body, expect, elen) == 0;
}

static void verify_one(const testcase_t *tc, const char *data, size_t len,
                       const size_t *ends, size_t nends, const char *mode)
{
    http_parser_t p;
    http_parser_init(&p);
    if (tc->max_body)    p.max_body = tc->max_body;
    if (tc->max_headers) p.max_headers = tc->max_headers;

    size_t consumed = 0;
    http_parse_status_t st = feed_with_bounds(&p, data, ends, nends, &consumed);

    CHECK(st == tc->expect_status,
          "[%s/%s] status=%d 期望=%d (err=%s)", tc->name, mode, (int)st,
          (int)tc->expect_status, http_parser_strerror(&p));

    if (tc->expect_status == HTTP_PARSE_ERROR) {
        CHECK(p.error == tc->expect_error,
              "[%s/%s] error=%d 期望=%d 实际信息=%s", tc->name, mode,
              (int)p.error, (int)tc->expect_error, http_parser_strerror(&p));
    } else if (tc->expect_status == HTTP_PARSE_COMPLETE) {
        CHECK(consumed == len,
              "[%s/%s] consumed=%zu 期望=%zu（完整报文应被全部消费）",
              tc->name, mode, consumed, len);
        CHECK(p.method == tc->method,
              "[%s/%s] method=%s 期望=%s", tc->name, mode,
              http_method_name(p.method), http_method_name(tc->method));
        if (tc->target)
            CHECK(p.target && strcmp(p.target, tc->target) == 0,
                  "[%s/%s] target=%s 期望=%s", tc->name, mode,
                  p.target ? p.target : "(null)", tc->target);
        CHECK(p.keep_alive == tc->keep_alive,
              "[%s/%s] keep_alive=%d 期望=%d", tc->name, mode,
              p.keep_alive, tc->keep_alive);
        CHECK(p.chunked == tc->chunked,
              "[%s/%s] chunked=%d 期望=%d", tc->name, mode, p.chunked, tc->chunked);
        CHECK(p.has_content_length == tc->has_content_length,
              "[%s/%s] has_content_length=%d 期望=%d", tc->name, mode,
              p.has_content_length, tc->has_content_length);
        if (tc->has_content_length)
            CHECK(p.content_length == tc->content_length,
                  "[%s/%s] content_length=%llu 期望=%llu", tc->name, mode,
                  p.content_length, tc->content_length);
        CHECK(body_equals(&p, tc->body),
              "[%s/%s] body_len=%zu 期望=%zu body=\"%.*s\"", tc->name, mode,
              p.body_len, tc->body ? strlen(tc->body) : 0,
              (int)(p.body_len > 60 ? 60 : p.body_len), p.body ? p.body : "");
        if (tc->lookup_header) {
            const char *v = http_parser_get_header(&p, tc->lookup_header);
            CHECK(v && strcmp(v, tc->lookup_value) == 0,
                  "[%s/%s] header %s=\"%s\" 期望=\"%s\"", tc->name, mode,
                  tc->lookup_header, v ? v : "(null)", tc->lookup_value);
        }
        CHECK(p.messages_parsed == 1,
              "[%s/%s] messages_parsed=%llu 期望=1", tc->name, mode,
              p.messages_parsed);
    }

    http_parser_free(&p);
}

static int g_case_reported_bad = 0;

static void verify_case(const testcase_t *tc)
{
    size_t len = tc->len ? tc->len : strlen(tc->data);
    int before = g_failures;

    /* ① 一次性喂完整报文 */
    size_t ends_whole[1] = { len };
    verify_one(tc, tc->data, len, ends_whole, 1, "whole");

    /* ② 每次只喂 1 字节 */
    size_t nbyte = len ? len : 1;
    size_t *ends_byte = (size_t *)calloc(nbyte, sizeof(size_t));
    if (!ends_byte) { fprintf(stderr, "out of memory\n"); exit(2); }
    for (size_t i = 0; i < len; i++) ends_byte[i] = i + 1;
    verify_one(tc, tc->data, len, ends_byte, len, "byte");
    free(ends_byte);

    /* ③ 穷举所有切分点：先喂前 k 字节，再喂剩下的 */
    size_t *ends_two = (size_t *)calloc(2, sizeof(size_t));
    if (!ends_two) { fprintf(stderr, "out of memory\n"); exit(2); }
    for (size_t k = 1; k < len; k++) {
        ends_two[0] = k;
        ends_two[1] = len;
        verify_one(tc, tc->data, len, ends_two, 2, "split");
    }
    free(ends_two);

    if (g_failures == before) {
        printf("  \033[32m✓\033[0m %-38s (whole + byte + %zu splits)\n",
               tc->name, len > 0 ? len - 1 : 0);
    } else {
        g_case_reported_bad++;
    }
}

/* ── 用例表 ──────────────────────────────────────────────────────── */

static const testcase_t cases[] = {
    /* ── 正常路径 ── */
    { "GET 基本请求", "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 1, 0, 0, 0, NULL,
      "host", "example.com", 0, 0 },

    { "GET 带 query 与多个头",
      "GET /search?q=linux&page=2 HTTP/1.1\r\n"
      "Host: a.test\r\nUser-Agent: learn-net/1.0\r\nAccept: */*\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/search?q=linux&page=2",
      1, 0, 0, 0, NULL, "User-Agent", "learn-net/1.0", 0, 0 },

    { "HEAD 请求",
      "HEAD /index.html HTTP/1.1\r\nHost: a\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_HEAD, "/index.html", 1, 0, 0, 0,
      NULL, NULL, NULL, 0, 0 },

    { "HTTP/1.0 默认短连接",
      "GET / HTTP/1.0\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "HTTP/1.0 + Connection: keep-alive",
      "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 1, 0, 0, 0, NULL,
      "connection", "keep-alive", 0, 0 },

    { "HTTP/1.1 + Connection: close",
      "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "HTTP/1.1 缺 Host（只告警不拒绝）",
      "GET / HTTP/1.1\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 1, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "HTTP/0.9 两段式请求行",
      "GET /index.html\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/index.html", 0, 0, 0, 0,
      NULL, NULL, NULL, 0, 0 },

    { "bare LF 行结束（宽容模式）",
      "GET / HTTP/1.1\nHost: a\n\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 1, 0, 0, 0, NULL,
      "host", "a", 0, 0 },

    /* ── body：Content-Length ── */
    { "POST + Content-Length",
      "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\nhello world",
      0, HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_POST, "/echo", 1, 0, 1, 11,
      "hello world", "content-length", "11", 0, 0 },

    { "POST + Content-Length: 0（无 body，头部结束即完成）",
      "POST /e HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_POST, "/e", 1, 0, 1, 0, NULL,
      NULL, NULL, 0, 0 },

    { "Expect: 100-continue",
      "POST /up HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
      "Content-Length: 3\r\n\r\nabc", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_POST, "/up", 1, 0, 1, 3, "abc",
      "expect", "100-continue", 0, 0 },

    /* ── body：chunked ── */
    { "chunked 两个分块 + trailer",
      "POST /e HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n1\r\n!\r\n0\r\nX-Trailer: 1\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_POST, "/e", 1, 1, 0, 0, "hello!",
      NULL, NULL, 0, 0 },

    { "chunked 大写十六进制 + chunk 扩展",
      "POST /e HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
      "A;name=value\r\n0123456789\r\n0\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_POST, "/e", 1, 1, 0, 0,
      "0123456789", NULL, NULL, 0, 0 },

    { "chunked + bare LF",
      "POST /e HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\nhello\n0\n\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_POST, "/e", 1, 1, 0, 0, "hello",
      NULL, NULL, 0, 0 },

    { "chunked 空 body",
      "POST /e HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_POST, "/e", 1, 1, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    /* ── 头部细节 ── */
    { "obs-fold 折行头（已废弃但需兼容）",
      "GET / HTTP/1.1\r\nHost: a\r\nX-Long: one\r\n two\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 1, 0, 0, 0, NULL,
      "x-long", "one two", 0, 0 },

    { "头名大小写不敏感查找",
      "GET / HTTP/1.1\r\nhOsT: Example.COM\r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 1, 0, 0, 0, NULL,
      "HOST", "Example.COM", 0, 0 },

    { "头值两侧空白被裁剪",
      "GET / HTTP/1.1\r\nHost:   a.test   \r\n\r\n", 0,
      HTTP_PARSE_COMPLETE, HTTP_OK, HTTP_METHOD_GET, "/", 1, 0, 0, 0, NULL,
      "host", "a.test", 0, 0 },

    /* ── 错误路径 ── */
    { "错误：请求行缺少 target",
      "GET\r\n\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BAD_REQUEST_LINE, 0, NULL, 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "错误：头部缺冒号",
      "GET / HTTP/1.1\r\nHost a\r\n\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BAD_HEADER, 0, NULL, 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "错误：头名含非法字符（空格）",
      "GET / HTTP/1.1\r\nBad Name: x\r\n\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BAD_HEADER, 0, NULL, 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "错误：重复 Content-Length",
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
      "Content-Length: 4\r\n\r\nabc", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_DUPLICATE_CONTENT_LENGTH, 0, NULL, 0, 0, 0, 0,
      NULL, NULL, NULL, 0, 0 },

    { "错误：Content-Length 与 chunked 同时出现（请求走私）",
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
      "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_CONTENT_LENGTH_WITH_CHUNKED, 0, NULL, 0, 0, 0,
      0, NULL, NULL, NULL, 0, 0 },

    { "错误：Content-Length 非数字",
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3x\r\n\r\nabc", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BAD_CONTENT_LENGTH, 0, NULL, 0, 0, 0, 0,
      NULL, NULL, NULL, 0, 0 },

    { "错误：chunk size 非法",
      "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
      "zz\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BAD_CHUNK_SIZE, 0, NULL, 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "错误：chunk 数据后不是 CRLF",
      "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhelloXX", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BAD_CHUNK_TERMINATOR, 0, NULL, 0, 0, 0, 0,
      NULL, NULL, NULL, 0, 0 },

    { "错误：不支持的 HTTP 版本",
      "GET / HTTP/2.0\r\n\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BAD_VERSION, 0, NULL, 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 0 },

    { "错误：body 超过上限",
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_BODY_TOO_LARGE, 0, NULL, 0, 0, 0, 0, NULL,
      NULL, NULL, 4, 0 },

    { "错误：头数量超过上限",
      "GET / HTTP/1.1\r\nHost: a\r\nX-1: 1\r\nX-2: 2\r\n\r\n", 0,
      HTTP_PARSE_ERROR, HTTP_ERR_TOO_MANY_HEADERS, 0, NULL, 0, 0, 0, 0, NULL,
      NULL, NULL, 0, 2 },
};

/* ── 特殊用例：body 里含 NUL（证明 body_len 才是权威，strlen 会误判） ── */
static void test_binary_body(void)
{
    const char head[] = "PUT /blob HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n";
    const char raw[5] = { 'A', '\0', 'B', '\0', 'C' };
    char buf[128];
    size_t hlen = sizeof(head) - 1;
    memcpy(buf, head, hlen);
    memcpy(buf + hlen, raw, 5);
    size_t len = hlen + 5;

    http_parser_t p;
    http_parser_init(&p);
    size_t c = 0;
    http_parse_status_t st = http_parser_execute(&p, buf, len, &c);
    CHECK(st == HTTP_PARSE_COMPLETE, "[binary] 应完成，st=%d err=%s", (int)st,
          http_parser_strerror(&p));
    CHECK(p.method == HTTP_METHOD_PUT, "[binary] method=%s",
          http_method_name(p.method));
    CHECK(p.body_len == 5, "[binary] body_len=%zu 期望=5", p.body_len);
    CHECK(p.body && memcmp(p.body, raw, 5) == 0,
          "[binary] body 内容含 NUL 时被破坏");
    /* 关键点：strlen(p.body) 只有 1，说明绝不能用 strlen 判断 body 长度 */
    CHECK(p.body && strlen(p.body) == 1,
          "[binary] strlen(body)=%zu（应为 1，证明必须用 body_len）",
          p.body ? strlen(p.body) : 0);
    http_parser_free(&p);
    if (g_failures == 0)
        printf("  \033[32m✓\033[0m %-38s (NUL 安全)\n", "body 含 NUL 字节");
}

/* ── 特殊用例：单行超长（需要动态构造） ── */
static void test_line_too_long(void)
{
    size_t big = HTTP_MAX_LINE_DEFAULT + 100;
    char *buf = (char *)malloc(big + 64);
    int n = snprintf(buf, big + 64, "GET / HTTP/1.1\r\nHost: a\r\nX-Big: ");
    memset(buf + n, 'a', big);
    n += (int)big;
    memcpy(buf + n, "\r\n\r\n", 5);
    size_t len = (size_t)n + 4;

    testcase_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.name = "错误：单行超过 max_line";
    tc.data = buf;
    tc.len = len;
    tc.expect_status = HTTP_PARSE_ERROR;
    tc.expect_error = HTTP_ERR_LINE_TOO_LONG;
    verify_case(&tc);
    free(buf);
}

/* ── 特殊用例：pipelining（一个 TCP 段里挤了两个请求） ── */
static void test_pipelining(void)
{
    /* 一个 TCP 段里挤了两个请求：内核只交给你一段字节流，切分是应用层的事 */
    const char *req1 = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    const char *req2 = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t len1 = strlen(req1);
    size_t len = len1 + strlen(req2);
    char *two = (char *)malloc(len + 1);
    memcpy(two, req1, len1);
    memcpy(two + len1, req2, strlen(req2) + 1);

    http_parser_t p;
    http_parser_init(&p);

    size_t c = 0;
    http_parse_status_t st = http_parser_execute(&p, two, len, &c);
    CHECK(st == HTTP_PARSE_COMPLETE, "[pipelining] 第一个请求应完成，实际 st=%d", (int)st);
    CHECK(p.target && strcmp(p.target, "/a") == 0,
          "[pipelining] 第一个 target=%s 期望=/a", p.target ? p.target : "(null)");
    CHECK(c == len1, "[pipelining] 第一个请求应恰好消费 %zu 字节，实际=%zu", len1, c);

    /* 关键：内核只给了一个字节流，第二条请求的字节已经在手里了 */
    http_parser_reset(&p);
    size_t c2 = 0;
    st = http_parser_execute(&p, two + c, len - c, &c2);
    CHECK(st == HTTP_PARSE_COMPLETE, "[pipelining] 第二个请求应完成，实际 st=%d", (int)st);
    CHECK(p.target && strcmp(p.target, "/b") == 0,
          "[pipelining] 第二个 target=%s 期望=/b", p.target ? p.target : "(null)");

    /* 未 reset 就继续喂：必须返回 COMPLETE 且不消费字节（防止把 body 读串） */
    http_parser_reset(&p);
    st = http_parser_execute(&p, two, len, &c2);
    CHECK(st == HTTP_PARSE_COMPLETE, "[pipelining] 首次喂入应完成");
    size_t c3 = 12345;
    st = http_parser_execute(&p, "GET /c HTTP/1.1\r\n\r\n", 18, &c3);
    CHECK(st == HTTP_PARSE_COMPLETE && c3 == 0,
          "[pipelining] 未 reset 时应不消费字节并返回 COMPLETE（c3=%zu）", c3);

    free(two);
    http_parser_free(&p);
}

/* ── 特殊用例：把「同一连接上的 keep-alive 复用」串起来 ── */
static void test_keepalive_sequence(void)
{
    const char *reqs[3] = {
        "POST /1 HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi",
        "POST /2 HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n",
        "GET /3 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
    };
    const char *bodies[3] = { "hi", "abc", NULL };

    http_parser_t p;
    http_parser_init(&p);

    for (int i = 0; i < 3; i++) {
        size_t len = strlen(reqs[i]);
        size_t c = 0;
        http_parse_status_t st = http_parser_execute(&p, reqs[i], len, &c);
        CHECK(st == HTTP_PARSE_COMPLETE, "[keepalive#%d] 应完成，st=%d err=%s",
              i, (int)st, http_parser_strerror(&p));
        char expect_target[8];
        snprintf(expect_target, sizeof(expect_target), "/%d", i + 1);
        CHECK(p.target && strcmp(p.target, expect_target) == 0,
              "[keepalive#%d] target=%s 期望=%s", i, p.target ? p.target : "(null)",
              expect_target);
        CHECK(body_equals(&p, bodies[i]), "[keepalive#%d] body 不匹配（len=%zu）",
              i, p.body_len);
        if (i < 2)
            CHECK(p.keep_alive == 1, "[keepalive#%d] 应保持长连接", i);
        else
            CHECK(p.keep_alive == 0, "[keepalive#%d] Connection: close 应关闭", i);
        http_parser_reset(&p);
    }
    http_parser_free(&p);
}

int main(void)
{
    printf("=== http_parser 单元测试 ===\n");
    printf("报文可能被任意切片喂入，三种喂法结果必须一致\n\n");

    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++)
        verify_case(&cases[i]);

    test_binary_body();
    test_line_too_long();
    test_pipelining();
    test_keepalive_sequence();

    printf("\n=== 结果 ===\n");
    printf("断言数：%d，失败：%d，失败用例：%d\n",
           g_checks, g_failures, g_case_reported_bad);
    if (g_failures == 0) {
        printf("\033[32m全部通过\033[0m\n");
        return 0;
    }
    printf("\033[31m存在失败断言\033[0m\n");
    return 1;
}
