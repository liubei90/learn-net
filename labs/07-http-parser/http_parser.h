/*
 * http_parser.h —— 教学用增量式 HTTP/1.1 请求解析器
 *
 * 设计目标（与 Linux 网络子系统的对照是本教程的重点）：
 *   内核 TCP 层只提供「一条可靠、有序的字节流」（sk_receive_queue 里的 sk_buff 链）。
 *   它**不提供任何消息边界**。HTTP 的「请求在哪里结束」完全由应用层自己判断。
 *   所以一个 HTTP 解析器本质上是一个「字节流 → 消息」的状态机：
 *   你喂给它任意大小的片段（1 字节、4KB、1MB），它必须得出同样的结论。
 *
 *   这就是为什么本解析器刻意采用「增量 + 状态机 + 不假设一次喂完整包」的接口，
 *   与内核里的 tcp_rcv_established（按 skb 分片处理字节流）在思路上完全一致。
 *
 * 覆盖的 HTTP/1.1 特性：
 *   - 请求行解析（方法 / 目标 / 版本），兼容 HTTP/0.9 两段式请求行
 *   - Header 解析：大小写不敏感查找、值裁剪、obs-fold 折行、重复头
 *   - 消息体：Content-Length 与 Transfer-Encoding: chunked（含 chunk 扩展与 trailer）
 *   - Connection 语义：1.1 默认 keep-alive、1.0 默认 close、Connection 头覆盖
 *   - Expect: 100-continue
 *   - 防护性上限：单行长度、头数量、头总字节、body 大小
 *     （对应内核侧的 tcp_rmem / somaxconn 等「必须设上限」的同类思考）
 *
 * 明确不做（保持教学聚焦）：
 *   HTTP/2、multipart、content-encoding 解码、URL 解码、响应解析。
 */
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_MAX_HEADERS_DEFAULT 100
#define HTTP_MAX_LINE_DEFAULT    8192

/* ── 方法 ─────────────────────────────────────────────────────────── */
typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_CONNECT,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_TRACE,
    HTTP_METHOD_PATCH
} http_method_t;

/* ── 解析器返回状态 ───────────────────────────────────────────────── */
typedef enum {
    HTTP_PARSE_ERROR     = -1,  /* 报文非法，连接应返回 4xx 并关闭 */
    HTTP_PARSE_NEED_MORE =  0,  /* 数据不够，继续喂 */
    HTTP_PARSE_COMPLETE  =  1   /* 一条完整消息（含 body）已解析完 */
} http_parse_status_t;

/* ── 错误码 ───────────────────────────────────────────────────────── */
typedef enum {
    HTTP_OK = 0,
    HTTP_ERR_INTERNAL,
    HTTP_ERR_LINE_TOO_LONG,
    HTTP_ERR_TOO_MANY_HEADERS,
    HTTP_ERR_HEADERS_TOO_LARGE,
    HTTP_ERR_BAD_REQUEST_LINE,
    HTTP_ERR_BAD_METHOD,
    HTTP_ERR_BAD_VERSION,
    HTTP_ERR_BAD_HEADER,
    HTTP_ERR_BAD_CONTENT_LENGTH,
    HTTP_ERR_DUPLICATE_CONTENT_LENGTH,
    HTTP_ERR_CONTENT_LENGTH_WITH_CHUNKED,
    HTTP_ERR_BAD_CHUNK_SIZE,
    HTTP_ERR_BAD_CHUNK_TERMINATOR,
    HTTP_ERR_BODY_TOO_LARGE
} http_error_t;

/* ── 内部状态机 ───────────────────────────────────────────────────── */
typedef enum {
    HP_ST_START = 0,      /* 等待下一条消息 */
    HP_ST_REQUEST_LINE,   /* 正在收集请求行 */
    HP_ST_HEADERS,        /* 正在收集头部行 */
    HP_ST_BODY_CL,        /* 正在读 Content-Length 指定的消息体 */
    HP_ST_CHUNK_SIZE,     /* 正在读 chunk-size 行 */
    HP_ST_CHUNK_DATA,     /* 正在读一个 chunk 的数据 */
    HP_ST_CHUNK_CRLF,     /* 正在读 chunk 数据后的 CRLF */
    HP_ST_TRAILERS,       /* 正在读 trailer 头部 */
    HP_ST_DONE            /* 消息已完成，等待调用者 reset */
} http_state_t;

typedef struct {
    char *name;    /* NUL 结尾的副本（便于打印） */
    char *value;   /* NUL 结尾的副本 */
} http_header_t;

typedef struct {
    /* ── 已解析出的消息字段 ── */
    http_method_t method;
    char          method_str[16];
    char         *target;        /* 原始 request-target，未做 %xx 解码 */
    size_t        target_len;
    int           version_major;
    int           version_minor;

    http_header_t headers[HTTP_MAX_HEADERS_DEFAULT];
    size_t        num_headers;

    const char   *host;          /* 指向 Host 头的 value（随 headers 生命周期） */
    size_t        host_len;
    int           keep_alive;    /* 1=连接可复用 */
    int           expect_continue;
    int           chunked;
    int           has_content_length;
    unsigned long long content_length;
    int           missing_host;  /* HTTP/1.1 缺 Host：只告警，不拒绝（教学用） */
    int           saw_obs_fold;  /* 使用了已废弃的折行头 */

    /* ── 消息体（已解码：chunked 会被还原成原始字节） ── */
    char              *body;     /* 可能含 '\0'，必须用 body_len */
    size_t             body_len;
    size_t             body_cap;
    unsigned long long body_received;

    /* ── 解析状态 ── */
    http_state_t state;
    char        *line;           /* 当前正在拼装的行缓冲 */
    size_t       line_len;
    size_t       line_cap;
    unsigned long long remaining; /* CL 剩余或当前 chunk 剩余 */
    int          crlf_state;      /* chunk 后 CRLF 的中间状态 */
    size_t       trailers_seen;

    /* ── 可配置上限（防止一个恶意连接吃掉所有内存） ── */
    size_t max_line;
    size_t max_headers;
    size_t max_header_bytes;
    size_t max_body;
    size_t header_bytes;

    /* ── 错误诊断 ── */
    http_error_t error;
    const char  *error_msg;
    size_t       error_offset;   /* 出错位置（当前 execute 调用内的偏移） */

    unsigned long long messages_parsed;
} http_parser_t;

/* 生命周期 */
void http_parser_init(http_parser_t *p);
void http_parser_free(http_parser_t *p);
/* 同一条连接上解析下一条消息（keep-alive / pipelining 必需） */
void http_parser_reset(http_parser_t *p);

/*
 * 核心入口：把 data[0..len) 喂给解析器。
 *   consumed 回填「本次消费了多少字节」——pipelining 时调用者需要用它
 *   把剩余字节接着喂给下一个消息。
 * 返回 HTTP_PARSE_COMPLETE 后必须调用 http_parser_reset 才能继续。
 */
http_parse_status_t http_parser_execute(http_parser_t *p, const char *data,
                                        size_t len, size_t *consumed);

/* 辅助 */
const char *http_parser_strerror(const http_parser_t *p);
const char *http_method_name(http_method_t m);
/* 大小写不敏感查找（HTTP 头名不区分大小写），返回 value 或 NULL */
const char *http_parser_get_header(const http_parser_t *p, const char *name);

#endif /* HTTP_PARSER_H */
