# labs/07 · HTTP/1.1 增量解析器与教学服务器

> **这个实验想让你亲眼看到的分界线**：内核 TCP 把「字节流」交给你，
> 而「HTTP 消息在哪里结束」这件事，内核完全不知道，只能由应用层自己算。
> 一旦你理解了这一点，就能理解为什么会有 `Content-Length`、`chunked`、
> 请求走私（request smuggling）、Slowloris 这些概念，也就能理解
> `tcp_rmem`/`somaxconn` 这些内核旋钮到底在保护什么。

---

## 1. 实验目的

1. 亲手实现一个**增量式（流式）** HTTP/1.1 请求解析器，而不是「一次读取完整报文再解析」。
2. 用「整包 / 逐字节 / 穷举所有切分点」三种喂法验证：**同一报文，任意切片，结果必须一致**。
3. 把解析器接到真实 TCP 字节流上（epoll + 非阻塞），观察 keep-alive、pipelining、
   100-continue、chunked 这些「应用层约定如何弥补内核不提供消息边界」。
4. 站在内核侧看同一条连接：`ss -tni`、`/proc/net/tcp`，把应用层行为与内核状态对应起来。

## 2. 前置知识

- 会写基本的 socket 程序（`socket/bind/listen/accept/recv/send`）。
- 知道 HTTP/1.1 报文格式（请求行、头部、body）。
- 读过 [docs/01-Linux网络子系统全景与数据流.md](../../docs/01-Linux网络子系统全景与数据流.md) 的第 3 节（接收路径）。
- C 语言：指针、状态机、结构体。

## 3. 涉及的内核机制与源码入口

| 内核侧（你在本实验里观察的） | 源码入口 | 应用侧（你在这个实验里写的） |
|---|---|---|
| TCP 把字节流放进 `sk_receive_queue` | `net/ipv4/tcp_input.c` `tcp_data_queue` | `recv()` 拿到 `n` 字节，边界无意义 |
| 跨 skb 记录接收进度 | `struct tcp_sock` 的 `rcv_nxt`/`copied_seq` | `struct conn` 里的 `http_parser_t` 跨 recv 调用保存状态 |
| 应用没读时数据留在接收队列 | `sk_rmem_alloc`、`sk->sk_rcvbuf` | 我们只在 epoll 可读时读，读不完留内核 |
| 连接建立/关闭状态 | `tcp_rcv_state_process` | keep-alive 决定何时 `close()` |
| 小包合并（Nagle）与延迟 ACK | `tcp_nagle_check`、`tcp_send_delayed_ack` | HTTP 小响应 → `TCP_NODELAY` |
| 接收缓冲上限（防慢速攻击） | `/proc/sys/net/ipv4/tcp_rmem` | 解析器的 `max_line`/`max_header_bytes`/`max_body` + idle 超时 |

> **一句话总结**：内核负责「**可靠有序的字节流**」，应用层负责「**消息边界与语义**」。
> 两边都要设上限，否则一边的失控会传导到另一边。

## 4. 文件清单

| 文件 | 作用 |
|---|---|
| `http_parser.h` | 解析器接口与全部状态定义（含中文注释解释每层语义） |
| `http_parser.c` | 增量状态机实现：请求行 / 头部 / CL body / chunked / trailer |
| `test_parser.c` | 表驱动单元测试：32 个用例 × 三种喂法（≈2.7 万断言） |
| `http_server.c` | epoll + 非阻塞教学服务器，7 条路由，Slowloris 防护 |
| `run.sh` | 一键跑完 14 段实验（编译 → 单测 → HTTP 交互 → 4xx → 内核观测） |
| `Makefile` | `all` / `test` / `run` / `clean`，`-Wall -Wextra -Wshadow` 零警告 |

## 5. 编译与运行

```bash
cd labs/07-http-parser

make            # 编译 test_parser 与 http_server（必须零警告）
make test       # 只跑单元测试
make run        # 跑完整实验（会在 127.0.0.1 上随机端口起服务，自动清理）

# 手工把玩
./http_server --port 8080 --bind 127.0.0.1
curl -i http://127.0.0.1:8080/headers
```

**权限要求：无。** 全程只监听 `127.0.0.1` 的非特权端口（由内核随机分配），
不碰 `CAP_NET_RAW`/`CAP_NET_ADMIN`，因此在任何受限环境都能跑。

## 6. 逐步实验步骤

`run.sh` 会依次执行下面 14 段；每一段都建议你手工重做一遍并观察：

1. **编译**：确认零警告（`-Wall -Wextra -Wshadow -Wwrite-strings ...`）。
2. **单元测试**：理解「三种喂法结果必须一致」的验证思路。
3. **启动服务器**：注意日志里的 `[accept]`，以及监听端口是随机分配的。
4. **`GET /hello`**：最小 HTTP 交互，看清响应头与 `Content-Length`。
5. **`GET /headers`**：让服务器回显它「解析出来的东西」——头名大小写、值裁剪、诊断标志。
6. **keep-alive**：3 个 URL 复用 1 条 TCP 连接（`num_connects` 只有第一次是 1）。
7. **`POST /echo`**：`Content-Length` 方式的消息体边界。
8. **chunked 请求体**：用 `nc` 手工构造原始报文，看长度如何写进数据流。
9. **chunked 响应**：用 `curl --raw` 看未解码的传输编码。
10. **`Expect: 100-continue`**：服务器先回 `100 Continue`，客户端再发 body。
11. **大响应（200KB）**：观察内核如何把响应切成多个 TCP 段。
12. **畸形报文**：`GET\r\n\r\n` → 400；`Content-Length` + `Transfer-Encoding` 同时出现 → 400。
13. **内核侧观测**：`ss -tn` / `ss -tni` / `/proc/net/tcp`，把应用层连接翻译成内核视图。
14. **服务器日志**：逐条请求的解析结果。

## 7. 真实预期输出（本机实测）

### 7.1 单元测试：2.7 万个断言全绿

（**输出为节选**：完整清单共 32 个用例，下面只列部分代表用例）

```
=== http_parser 单元测试 ===
报文可能被任意切片喂入，三种喂法结果必须一致

  ✓ GET 基本请求                       (whole + byte + 36 splits)
  ✓ GET 带 query 与多个头             (whole + byte + 92 splits)
  ✓ HTTP/1.0 默认短连接               (whole + byte + 17 splits)
  ✓ HTTP/1.1 + Connection: close           (whole + byte + 45 splits)
  ✓ HTTP/0.9 两段式请求行            (whole + byte + 16 splits)
  ✓ POST + Content-Length                  (whole + byte + 62 splits)
  ✓ Expect: 100-continue                   (whole + byte + 73 splits)
  ✓ chunked 两个分块 + trailer         (whole + byte + 91 splits)
  ✓ chunked 大写十六进制 + chunk 扩展 (whole + byte + 87 splits)
  ✓ obs-fold 折行头（已废弃但需兼容） (whole + byte + 45 splits)
  ✓ 错误：重复 Content-Length         (whole + byte + 68 splits)
  ✓ 错误：Content-Length 与 chunked 同时出现（请求走私） (whole + byte + 79 splits)
  ✓ 错误：chunk 数据后不是 CRLF    (whole + byte + 65 splits)
  ✓ body 含 NUL 字节                    (NUL 安全)
  ✓ 错误：单行超过 max_line         (whole + byte + 8327 splits)

=== 结果 ===
断言数：27057，失败：0，失败用例：0
全部通过
```

> **为什么「穷举切分点」这么重要**：一次 4KB 的 `recv()` 可能正好切断 `\r\n`
> （甚至在 chunked 的十六进制长度中间）。如果你的解析器在「一次喂整包」时正确、
> 但被切碎后出错，那么在生产环境它就会随机地失败——而且只在网络时序不巧时失败。

### 7.2 keep-alive：一条 TCP 连接承载 3 个请求

```
━━━ 6. keep-alive —— 3 个 URL 复用同一条 TCP 连接 ━━━
  url=http://127.0.0.1:46457/hello  num_connects=1  http_code=200
  url=http://127.0.0.1:46457/headers  num_connects=0  http_code=200
  url=http://127.0.0.1:46457/big?n=1000  num_connects=0  http_code=200
  （第一次 num_connects=1 是新建连接；后两次 =0，说明复用了同一条 TCP 连接）
```

服务器日志侧对应的是同一个 `[accept]` 后连续三次 `[req]`——内核只看到字节流，
它不知道这里面有几个 HTTP 请求：

```
[accept] 127.0.0.1:58692 (fd=5)
[req 3] GET /hello HTTP/1.1  headers=3 body=0 keep_alive=1
[req 4] GET /headers HTTP/1.1  headers=3 body=0 keep_alive=1
[req 5] GET /big?n=1000 HTTP/1.1  headers=3 body=0 keep_alive=1
```

### 7.3 `/headers`：解析器实际看到的东西

```
━━━ 5. GET /headers —— 回显解析结果（头名大小写、裁剪、诊断标志） ━━━
请求行: GET /headers HTTP/1.1
keep_alive=1 chunked=0 content_length=0 body_len=0
missing_host=0 obs_fold=0 trailers=0

头部 5 个(按解析顺序):
  [ 0] Host                     = demo.test
  [ 1] User-Agent               = curl/8.14.1
  [ 2] X-Mixed-CASE             = Yes
  [ 3] X-Padded                 = value with spaces
  [ 4] Accept                   = */*

提示: 头名按 RFC 7230 不区分大小写，http_parser_get_header("HOST") 也能取到值。
```

注意 `X-Padded:    value with spaces    ` 两侧的空白被裁剪掉了——这是 RFC 7230 的 OWS 规则。

### 7.4 chunked 请求体（`nc` 直发原始报文）

```
━━━ 8. chunked 请求体 —— 手工构造原始 HTTP 报文（nc 直发） ━━━
HTTP/1.1 200 OK
Server: learn-net-httpd/1.0
Connection: close
Content-Length: 54

收到 6 字节请求体（方式：chunked）

hello!
```

发的原始报文是：

```
POST /echo HTTP/1.1
Host: raw.test
Connection: close
Transfer-Encoding: chunked

5
hello
1
!
0

```
→ 解析器把 `5\r\nhello\r\n` 和 `1\r\n!\r\n` 解码成 6 字节 `hello!`。

### 7.5 chunked 响应（`curl --raw`）

```
━━━ 9. chunked 响应 —— --raw 看未解码的传输编码 ━━━
HTTP/1.1 200 OK
Transfer-Encoding: chunked

4b
第一块：内核只提供字节流，chunked 把长度写进数据流。

3c
第二块：每块格式为 <hex 长度>CRLF<数据>CRLF。

24
第三块：以 0CRLFCRLF 结束。

0

```
`4b` = 75，`3c` = 60，`24` = 36——**长度写在数据流里**，因为服务器生成正文前不知道总长度。

### 7.6 100-continue

```
━━━ 10. Expect: 100-continue —— 服务器先回 100 再收 body ━━━
HTTP/1.1 100 Continue

HTTP/1.1 200 OK
Content-Length: 75

收到 19 字节请求体（方式：Content-Length）

body-after-continue
```
一次性响应里出现两个状态行：先 `100 Continue`，再最终 `200 OK`。

### 7.7 畸形报文 → 4xx

```
━━━ 12. 畸形报文 —— 解析器拒绝并回 4xx ━━━
--- (a) 请求行缺 target ---
HTTP/1.1 400 Bad Request
Connection: close

400 Bad Request
原因: malformed request line
--- (b) 请求走私：Content-Length 与 Transfer-Encoding 同时出现 ---
HTTP/1.1 400 Bad Request
Connection: close

400 Bad Request
原因: both Transfer-Encoding and Content-Length present
```

> **为什么必须拒绝 (b)**：如果按 `Content-Length` 切分还是按 `chunked` 切分，
> 上游代理与下游服务器可能得出**不同的消息边界**，攻击者就能把「第二个请求」
> 夹带在第一个请求的 body 里，这就是 HTTP 请求走私。宁可拒绝，不可猜测。

### 7.8 内核侧观测：HTTP 之下的 TCP 长什么样

```
━━━ 13. 内核侧观测 ━━━
--- ss -tnp 到本服务器的连接 ---
State Recv-Q Send-Q Local Address:Port  Peer Address:Port
ESTAB 0      0          127.0.0.1:46457    127.0.0.1:58770

--- ss -tni 该连接的 TCP 细节 ---
	 cubic wscale:7,7 rto:204 rtt:0.019/0.01 ato:40 mss:32768 pmtu:65535
	 cwnd:10 bytes_sent:794 bytes_acked:794 bytes_received:32 segs_out:4 segs_in:6
	 send 137970526316bps lastsnd:640 lastrcv:640 lastack:640
	 pacing_rate 265462278480bps delivery_rate 23831272720bps delivered:2 app_limited
	 rcv_space:65483 rcv_ssthresh:65483 minrtt:0.011 snd_wnd:64768 rcv_wnd:65536

--- /proc/net/tcp 中该端口对应的行（十六进制地址:端口，状态码） ---
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   1: 0100007F:B579 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000 0 308938
  21: 0100007F:B579 0100007F:E592 01 00000000:00000000 00:00000000 00000000  1000 0 308939
 201: 0100007F:B579 0100007F:E590 06 00000000:00000000 03:0000172E 00000000     0 0 0
```

逐条解读（这是本章最该学会的「翻译」能力）：

| 字段 | 含义 |
|---|---|
| `st=0A` | `TCP_LISTEN`（监听套接字，`rem_address` 全 0） |
| `st=01` | `TCP_ESTABLISHED`（这就是那条 keep-alive 连接） |
| `st=06` | `TCP_TIME_WAIT`（主动关闭方，`tm->when` 显示 0x172E ≈ 59s 倒计时） |
| `0100007F:B579` | 小端十六进制 IPv4 `127.0.0.1` + 端口 `0xB579 = 46457` |
| `tx_queue:rx_queue` | 发送/接收队列的**未确认字节数**（`0:0` 表示都排空了） |
| `cubic` | 拥塞控制算法；`cwnd:10` 是 10 个 MSS 的拥塞窗口 |
| `rcv_wnd:65536` | 接收窗口；`app_limited` 表示发送速率受应用层限制而非拥塞窗口 |

> 环回连接的 `mss:32768` / `pmtu:65535` 是个提醒：**`lo` 的 MTU 是 65536**，
> 所以在本机测不出分片和 PMTU 问题。这正是「本机测试正常、上线却出问题」的常见原因之一。

### 7.9 服务器日志：13 条请求的完整轨迹

```
[accept] 127.0.0.1:48748 (fd=5)
[req 3] GET /hello HTTP/1.1  headers=3 body=0 keep_alive=1
[req 4] GET /headers HTTP/1.1  headers=3 body=0 keep_alive=1
[req 5] GET /big?n=1000 HTTP/1.1  headers=3 body=0 keep_alive=1
[accept] 127.0.0.1:48762 (fd=5)
[req 6] POST /echo HTTP/1.1  headers=5 body=15 keep_alive=1
[accept] 127.0.0.1:48770 (fd=5)
[req 7] POST /echo HTTP/1.1  headers=3 body=6 keep_alive=0
[accept] 127.0.0.1:48780 (fd=5)
[req 8] GET /chunked HTTP/1.1  headers=3 body=0 keep_alive=1
[accept] 127.0.0.1:48786 (fd=5)
[req 9] POST /echo HTTP/1.1  headers=6 body=19 keep_alive=1
[accept] 127.0.0.1:48802 (fd=5)
[req 10] GET /big?n=200000 HTTP/1.1  headers=3 body=0 keep_alive=1
[accept] 127.0.0.1:48808 (fd=5)
[req 11] 解析失败: malformed request line（error=5），回 400 并关闭
[accept] 127.0.0.1:48818 (fd=5)
[req 12] 解析失败: both Transfer-Encoding and Content-Length present（error=11），回 400 并关闭
[accept] 127.0.0.1:48830 (fd=5)
[req 13] GET / HTTP/1.1  headers=1 body=0 keep_alive=1
```

`req 11/12` 是两条畸形报文（`nc` 直发），`req 13` 是第 13 段里为观测内核状态而保持的裸连接。

## 8. 观察与思考题

1. `http_parser_execute` 为什么返回 `consumed`？如果去掉这个参数，pipelining 会怎样出错？
2. 逐字节喂入时，`\r` 和 `\n` 被分在两次调用里——状态机里哪个字段保证了它不会误判？
3. `body_len` 与 `strlen(body)` 在什么情况下不等？为什么解析器必须提供 `body_len`？
4. 如果只实现 `Content-Length` 不支持 `chunked`，会有哪些真实场景挂掉？
5. 为什么收到 `Content-Length` + `Transfer-Encoding` 时要**拒绝**而不是「按其中一个处理」？
6. `conn_flush()` 为什么可能在一次 `send()` 里只写出去一部分？剩余部分在内核哪里？
7. 服务器的 Slowloris 防护（10s idle 超时 + `max_line`）中，哪一部分是内核无法替你做的？
   （提示：`tcp_rmem` 只能限制总内存，不能限制「一个连接慢慢发头」的时间）
8. 为什么服务器在 `accept` 之后就设 `TCP_NODELAY`？和第 7.8 节的 `app_limited` 有什么联系？
9. 环回测试里 `pmtu:65535`。如果要复现真实的 MTU 与分片问题，实验环境该怎么搭？
   （提示：本机可以用 `unshare -Urnmpf --mount-proc` 建 veth 并改 MTU 来复现，
   注意 `/sys` 无法重挂，MTU 要用 `ip link show` 验证，见
   [docs/00-环境准备与学习方法.md](../../docs/00-环境准备与学习方法.md)）
10. 如果把 `k` 条请求 pipelining 一次性发来，本服务器的响应顺序会不会乱？为什么？

<details>
<summary>参考答案要点</summary>

1. 一个 `recv()` 缓冲区里可能含「1.7 个请求」。`consumed` 告诉你第一个请求用到第几字节，
   剩下的字节属于下一个请求；没有它就只能丢弃或重复解析。
2. `HP_ST_REQUEST_LINE/HEADERS/CHUNK_SIZE/TRAILERS` 状态下的 `line` 缓冲 + `line_len`：
   `\r` 先被追加进缓冲，下一次调用遇到 `\n` 才触发行处理，因此 CRLF 跨调用天然安全。
3. body 里含 `\0` 时（如二进制上传、表单里的 NUL）。`strlen` 会在第一个 NUL 处截断，
   所以长度必须由 `body_len` 单独维护——测试用例「body 含 NUL 字节」就是在验证这一点。
4. 流式上传、边生成边发送、gzip 压缩流等「发送前不知道长度」的场景。
5. 因为两侧的解析器可能对「以哪个为准」判断不同，导致协议解析器差异（HTTP desync），
   即请求走私。安全做法是拒绝（本实现）或剥离 CL，绝不允许两者并存被"猜"。
6. 内核 socket 发送缓冲区（`sk_sndbuf`）满了；剩余字节留在用户态缓冲（本程序是 `c->out`），
   等 `EPOLLOUT` 可写时继续 `send`。内核侧的表现是 `ss -tmi` 的 `wmem`。
7. 超时与「按行/按头/按体积设上限」。内核只做总量控制（`tcp_rmem`、`somaxconn`），
   它并不知道「这个连接已经发了 8 分钟、只发了 3 个字节的头」。
8. 小响应若被 Nagle 攒着，又叠加对端的延迟 ACK，会产生几十毫秒额外延迟；
   `TCP_NODELAY` 立即发送。`app_limited` 说明发送速率由应用供给速度决定，而不是拥塞窗口。
9. 用 netns + veth，把 veth 一端 MTU 调小（如 1280）并在上面跑。本机可以这样做：
    `unshare -Urnmpf --mount-proc bash` 后用 `ip link add v0 type veth peer name v1`
    + `ip link set v0 mtu 1280`（实测可行；注意 `/sys` 无法重挂，MTU 要用 `ip link show` 看）。
   若要观察分片计数，记得让对端落在**另一个 netns** 里，否则内核按本机地址直接本地投递、不会分片。
10. 不会乱。解析器是单连接单线程顺序状态机，响应按解析顺序追加到 `c->out`，
    天然保证 FIFO（HTTP/1.1 也要求响应按请求顺序返回）。
</details>

## 9. 清理

```bash
make clean            # 删除可执行文件与 .o
# run.sh 内部用 trap 自动 kill 服务器并删除临时日志，无需手工清理
```

## 10. 延伸阅读

- RFC 7230 §3.3（消息体长度：`Content-Length` / `chunked` 与两者冲突的处理）
- RFC 9112（HTTP/1.1 的最新整理版，含 request smuggling 的安全建议）
- `Documentation/networking/` 里 TCP 相关文档；`man 7 tcp`、`man 7 socket`
- 与本章互补的观测章：[docs/09-内核网络观测与性能调优.md](../../docs/09-内核网络观测与性能调优.md)
