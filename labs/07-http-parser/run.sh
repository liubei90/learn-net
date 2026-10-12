#!/usr/bin/env bash
#
# run.sh —— 一键跑完 labs/07 的全部实验
#
# 设计要点：
#   * 不依赖 root，全程只监听 127.0.0.1（非特权端口由内核随机分配）
#   * 单元测试 + 真实 HTTP 交互 + 内核侧观测（ss / /proc/net/tcp）三段
#   * 服务器日志单独收集，最后打印，让你看到「解析器实际看到了什么」
set -euo pipefail

cd "$(dirname "$0")"

PORT="$(python3 -c 'import socket
s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"

LOG="$(mktemp -t learnnet-httpd.XXXXXX.log)"
SRV_PID=""

cleanup() {
  if [[ -n "$SRV_PID" ]] && kill -0 "$SRV_PID" 2>/dev/null; then
    kill -INT "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
  fi
  rm -f "$LOG"
}
trap cleanup EXIT

hr() { printf '\n\033[1;36m━━━ %s ━━━\033[0m\n' "$1"; }

# ── 1. 编译 ─────────────────────────────────────────────────────────
hr "1. 编译（零警告）"
make -s clean all
echo "已生成：$(ls -1 test_parser http_server | tr '\n' ' ')"

# ── 2. 单元测试 ─────────────────────────────────────────────────────
hr "2. 单元测试：整包 / 逐字节 / 穷举切分点"
./test_parser

# ── 3. 启动服务器 ───────────────────────────────────────────────────
hr "3. 启动教学服务器"
./http_server --port "$PORT" >"$LOG" 2>&1 &
SRV_PID=$!

for _ in $(seq 1 50); do
  if python3 -c "import socket,sys
s=socket.socket()
s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then
    break
  fi
  sleep 0.1
done
echo "服务器就绪：http://127.0.0.1:$PORT/  (pid=$SRV_PID)"

# ── 4. 最朴素的请求 ─────────────────────────────────────────────────
hr "4. GET /hello —— 最小 HTTP 交互"
curl -sS -i "http://127.0.0.1:$PORT/hello"

# ── 5. 看解析器看到了什么 ───────────────────────────────────────────
hr "5. GET /headers —— 回显解析结果（头名大小写、裁剪、诊断标志）"
curl -sS "http://127.0.0.1:$PORT/headers" \
     -H 'Host: demo.test' \
     -H 'X-Mixed-CASE: Yes' \
     -H 'X-Padded:    value with spaces    ' \
     -H 'Accept: */*'

# ── 6. keep-alive：一条 TCP 连接跑多个请求 ──────────────────────────
hr "6. keep-alive —— 3 个 URL 复用同一条 TCP 连接"
curl -sS -o /dev/null \
     -w '  url=%{url_effective}  num_connects=%{num_connects}  http_code=%{http_code}\n' \
     "http://127.0.0.1:$PORT/hello" \
     -o /dev/null \
     "http://127.0.0.1:$PORT/headers" \
     -o /dev/null \
     "http://127.0.0.1:$PORT/big?n=1000"
echo "  （第一次 num_connects=1 是新建连接；后两次 =0，说明复用了同一条 TCP 连接）"

# ── 7. POST + Content-Length ────────────────────────────────────────
hr "7. POST /echo —— Content-Length 方式的消息体"
curl -sS -i -X POST --data-binary 'hello from curl' "http://127.0.0.1:$PORT/echo"

# ── 8. POST + chunked（用 nc 手工构造原始报文） ─────────────────────
hr "8. chunked 请求体 —— 手工构造原始 HTTP 报文（nc 直发）"
printf 'POST /echo HTTP/1.1\r\nHost: raw.test\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n1\r\n!\r\n0\r\n\r\n' \
  | timeout 3 nc 127.0.0.1 "$PORT" || true

# ── 9. chunked 响应 ─────────────────────────────────────────────────
hr "9. chunked 响应 —— --raw 看未解码的传输编码"
curl -sS -i --raw "http://127.0.0.1:$PORT/chunked"

# ── 10. Expect: 100-continue ────────────────────────────────────────
hr "10. Expect: 100-continue —— 服务器先回 100 再收 body"
curl -sS -i -H 'Expect: 100-continue' --data-binary 'body-after-continue' \
     "http://127.0.0.1:$PORT/echo"

# ── 11. 大响应（观察 TCP 分片） ─────────────────────────────────────
hr "11. 大响应 —— 200KB，观察内核把它切成多少段"
curl -sS -o /dev/null -w '  size_download=%{size_download}  speed=%{speed_download}B/s\n' \
     "http://127.0.0.1:$PORT/big?n=200000"

# ── 12. 畸形报文 → 4xx ─────────────────────────────────────────────
hr "12. 畸形报文 —— 解析器拒绝并回 4xx"
echo "--- (a) 请求行缺 target ---"
printf 'GET\r\n\r\n' | timeout 3 nc 127.0.0.1 "$PORT" || true
echo "--- (b) 请求走私：Content-Length 与 Transfer-Encoding 同时出现 ---"
printf 'POST /echo HTTP/1.1\r\nHost: evil\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n' \
  | timeout 3 nc 127.0.0.1 "$PORT" || true

# ── 13. 内核侧观测 ──────────────────────────────────────────────────
hr "13. 内核侧观测 —— HTTP 之下的 TCP 长什么样"
# 起一个保持连接的客户端，然后从内核角度看它
( exec 3<>/dev/tcp/127.0.0.1/"$PORT"
  printf 'GET / HTTP/1.1\r\nHost: holder\r\n\r\n' >&3
  sleep 2
  exec 3<&- 3>&- ) &
HOLDER=$!
sleep 0.6
echo "--- ss -tnp 到本服务器的连接（内核视角的五元组与状态） ---"
ss -tn "sport = :$PORT" 2>/dev/null || ss -tn 2>/dev/null | head -20
echo
echo "--- ss -tni 该连接的 TCP 细节（cwnd / rtt / 窗口 / 拥塞算法） ---"
ss -tni "sport = :$PORT" 2>/dev/null | head -12
echo
echo "--- /proc/net/tcp 中该端口对应的行（十六进制地址:端口，状态码） ---"
PORT_HEX="$(printf '%04X' "$PORT")"
awk -v ph="$PORT_HEX" 'NR==1 || index($2, ":" ph)' /proc/net/tcp
wait "$HOLDER" 2>/dev/null || true

# ── 14. 服务器日志 ──────────────────────────────────────────────────
hr "14. 服务器日志 —— 解析器实际看到的东西"
cat "$LOG"

hr "完成"
echo "所有实验均在本机 127.0.0.1 完成，全程未使用 root 权限。"
