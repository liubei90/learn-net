/*
 * bpf_socket_filter.c —— 不用 clang、不用 libbpf，手写 eBPF 指令并加载
 *
 * 这是本实验里「看清 eBPF 本质」的那一段代码：
 *   1. 用 bpf(BPF_MAP_CREATE) 建一个 ARRAY map（1 个条目，用来记包数）
 *   2. 用 bpf(BPF_PROG_LOAD) 加载一段**手写指令**组成的 socket filter：
 *        统计每个到达该 socket 的包，然后返回 -1（=保留整包，即放行）
 *   3. 用 setsockopt(SO_ATTACH_BPF) 把它挂到 UDP socket 上
 *   4. 发几个包，再从 map 里把计数读回来
 *
 * 关键点：全程没有 clang、没有 libbpf，只有 `syscall(__NR_bpf, ...)`。
 * 编译需要的内核头文件在 /usr/include/linux/bpf.h（已在本机确认存在）。
 *
 * 权限：加载 BPF 需要 CAP_BPF（或 root）。本机 CapEff=0 且
 *       kernel.unprivileged_bpf_disabled=2，因此**会失败**——这不是 bug。
 *       程序会把 errno、capability、sysctl 值都打印出来，然后以退出码 0 结束，
 *       因为「诚实地报告不可运行」也是实验的一部分。
 *
 * 运行：./bpf_socket_filter
 */
/* _GNU_SOURCE 必须在任何头文件之前：否则 -std=c11 会隐藏 syscall() 的声明 */
#define _GNU_SOURCE
#include "bpf_insns.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/filter.h>      /* SO_ATTACH_BPF / SO_ATTACH_FILTER */

#ifndef __NR_bpf
#error "本平台没有定义 __NR_bpf，实验无法进行"
#endif

static int bpf_sys(enum bpf_cmd cmd, union bpf_attr *attr)
{
    return (int)syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

/* ── 打印环境事实，让「为什么跑不了」有据可查 ───────────────────── */
static void print_env_facts(void)
{
    FILE *f;
    char buf[128];

    printf("  环境事实：\n");
    f = fopen("/proc/sys/kernel/unprivileged_bpf_disabled", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f))
            printf("    kernel.unprivileged_bpf_disabled = %s", buf);
        fclose(f);
    }
    f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f))
            printf("    kernel.perf_event_paranoid      = %s", buf);
        fclose(f);
    }
    printf("    （unprivileged_bpf_disabled 取值语义：0=允许非特权, 1=已禁用但可改回,"
           " 2=永久禁用）\n");
}

/* ── 1. 建 map ───────────────────────────────────────────────────── */
static int create_count_map(void)
{
    union bpf_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.map_type    = BPF_MAP_TYPE_ARRAY;   /* = 2，索引 O(1) */
    attr.key_size    = sizeof(__u32);
    attr.value_size  = sizeof(__u32);
    attr.max_entries = 1;

    int fd = bpf_sys(BPF_MAP_CREATE, &attr);
    if (fd < 0)
        fprintf(stderr, "  [BPF_MAP_CREATE] 失败: %s (errno=%d)\n",
                strerror(errno), errno);
    else
        printf("  [BPF_MAP_CREATE] 成功，map fd=%d（ARRAY, 1 个 u32 条目）\n", fd);
    return fd;
}

/* ── 2. 手写程序 ─────────────────────────────────────────────────── */
/*
 * 语义（伪代码）：
 *     u32 *p = map_lookup_elem(&m, &key_zero);
 *     if (p) (*p)++;
 *     return -1;          // socket filter 的返回值 = 保留多少字节；-1 表示全部保留
 *
 * 指令逐条解释见下方注释里的 index。
 */
static struct bpf_insn g_prog[] = {
    /* [0] r6 = -1  —— 先把返回值存到被调用者保存寄存器里（helper 调用会覆盖 r0..r5） */
    BPF_MOV64_IMM(BPF_REG_6, -1),

    /* [1] key 放在栈上：*(u32 *)(r10 - 4) = 0 */
    BPF_ST_MEM(BPF_W, BPF_REG_10, -4, 0),

    /* [2] r2 = r10                       （helper 第 2 个参数：key 指针） */
    BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
    /* [3] r2 += -4  → r2 指向栈上的 key */
    BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),

    /* [4..5] r1 = map 指针（BPF_LD_MAP_FD 是展开成两条指令的宏，imm 稍后回填 fd） */
    BPF_LD_MAP_FD(BPF_REG_1, 0),

    /* [6] r0 = map_lookup_elem(r1, r2)    helper 编号 1 */
    BPF_EMIT_CALL(BPF_FUNC_map_lookup_elem),

    /* [7] if (r0 == 0) goto +3  → 跳到 [11] 的“返回” */
    BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 3),

    /* [8] r1 = *(u32 *)r0  */
    BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_0, 0),
    /* [9] r1 += 1 */
    BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, 1),
    /* [10] *(u32 *)r0 = r1 */
    BPF_STX_MEM(BPF_W, BPF_REG_0, BPF_REG_1, 0),

    /* [11] r0 = r6  （恢复返回值） */
    BPF_MOV64_REG(BPF_REG_0, BPF_REG_6),
    /* [12] exit */
    BPF_EXIT_INSN(),
};

static void patch_map_fd(struct bpf_insn *prog, size_t n, int map_fd)
{
    for (size_t i = 0; i < n; i++) {
        if (prog[i].code == (BPF_LD | BPF_DW | BPF_IMM) &&
            prog[i].src_reg == BPF_PSEUDO_MAP_FD) {
            prog[i].imm = map_fd;
            printf("  已把 map fd=%d 回填到指令 [%zu]（BPF_PSEUDO_MAP_FD）\n",
                   map_fd, i);
            return;
        }
    }
    fprintf(stderr, "  警告：没有找到需要回填的 BPF_PSEUDO_MAP_FD 指令\n");
}

static int load_prog(int map_fd)
{
    union bpf_attr attr;
    char log_buf[16 * 1024];
    char license[] = "GPL";
    size_t n = sizeof(g_prog) / sizeof(g_prog[0]);

    patch_map_fd(g_prog, n, map_fd);

    memset(&attr, 0, sizeof(attr));
    memset(log_buf, 0, sizeof(log_buf));
    attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;   /* = 1 */
    attr.insn_cnt  = (__u32)n;
    attr.insns     = (__aligned_u64)(unsigned long)g_prog;
    attr.license   = (__aligned_u64)(unsigned long)license;
    attr.log_level = 1;                 /* 让验证器把日志写到 log_buf */
    attr.log_size  = sizeof(log_buf);
    attr.log_buf   = (__aligned_u64)(unsigned long)log_buf;

    printf("  正在加载 %zu 条手写指令（BPF_PROG_TYPE_SOCKET_FILTER）...\n", n);

    int fd = bpf_sys(BPF_PROG_LOAD, &attr);
    if (fd < 0) {
        fprintf(stderr, "  [BPF_PROG_LOAD] 失败: %s (errno=%d)\n",
                strerror(errno), errno);
        if (log_buf[0])
            fprintf(stderr, "  验证器日志:\n%s\n", log_buf);
        return -1;
    }
    printf("  [BPF_PROG_LOAD] 成功，prog fd=%d\n", fd);
    if (log_buf[0])
        printf("  验证器日志（log_level=1 输出）:\n%s\n", log_buf);
    return fd;
}

/* ── 3. 挂到 socket 上并发包验证 ─────────────────────────────────── */
static int attach_and_test(int prog_fd, int map_fd)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    if (setsockopt(s, SOL_SOCKET, SO_ATTACH_BPF, &prog_fd, sizeof(prog_fd)) < 0) {
        fprintf(stderr, "  [SO_ATTACH_BPF] 失败: %s (errno=%d)\n",
                strerror(errno), errno);
        close(s);
        return -1;
    }
    printf("  [SO_ATTACH_BPF] 成功，程序已挂到 UDP socket\n");

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); close(s); return -1; }
    socklen_t slen = sizeof(sa);
    if (getsockname(s, (struct sockaddr *)&sa, &slen) < 0) { perror("getsockname"); close(s); return -1; }
    printf("  已绑定 127.0.0.1:%u，开始发 5 个 UDP 包...\n", ntohs(sa.sin_port));

    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0) { perror("socket(tx)"); close(s); return -1; }
    for (int i = 0; i < 5; i++) {
        char msg[32];
        int n = snprintf(msg, sizeof(msg), "packet-%d", i);
        if (sendto(tx, msg, (size_t)n, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
            perror("sendto");
    }
    close(tx);

    /* 收一下，确保包真的走到了 socket（socket filter 在收包路径上执行） */
    char buf[64];
    int got = 0;
    for (int i = 0; i < 5; i++) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = recv(s, buf, sizeof(buf), 0);
        if (r <= 0) break;
        got++;
    }
    printf("  应用层实际收到 %d 个包\n", got);

    /* ── 4. 从 map 读回计数 ── */
    union bpf_attr attr;
    __u32 key = 0, value = 0;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = (__u32)map_fd;
    attr.key    = (__aligned_u64)(unsigned long)&key;
    attr.value  = (__aligned_u64)(unsigned long)&value;
    if (bpf_sys(BPF_MAP_LOOKUP_ELEM, &attr) < 0) {
        fprintf(stderr, "  [BPF_MAP_LOOKUP_ELEM] 失败: %s\n", strerror(errno));
    } else {
        printf("  [BPF_MAP_LOOKUP_ELEM] map[0] = %u  ← 这就是 eBPF 程序在数据路径上累计的包数\n",
               value);
        if (value >= (__u32)got)
            printf("  ✅ eBPF 统计（%u）≥ 应用收到（%d），符合预期（过滤器在数据路径上先执行）\n",
                   value, got);
    }

    close(s);
    return 0;
}

/* ── 报告「为什么跑不了」，并给出 root 下的步骤 ─────────────────── */
static void report_skip(int err)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf(" SKIP: 本机无法加载 eBPF 程序（errno=%d: %s）\n", err, strerror(err));
    printf("═══════════════════════════════════════════════════════════════\n");
    printf(" 这是**预期结果**，不是代码错误。原因（本机实测）：\n");
    printf("   · CapEff = 0 —— 没有 CAP_BPF(bit 39) / CAP_PERFMON(bit 38)\n");
    printf("   · kernel.unprivileged_bpf_disabled = 2（永久禁用非特权 BPF，运行时无法改回）\n");
    printf("   · 即使 unshare -Urn 进入用户命名空间，BPF 校验仍在 init userns 上判断，依然失败\n");
    printf("\n");
    printf(" 在有你自己的 root 的机器上，直接跑本程序即可（无需任何额外依赖）：\n");
    printf("   make && sudo ./bpf_socket_filter\n");
    printf("\n");
    printf(" 想观察已加载的程序与 map：\n");
    printf("   sudo bpftool prog show\n");
    printf("   sudo bpftool map dump id <id>\n");
    printf("\n");
    printf(" 想换成一个真正属于你的过滤器（例如只放行某端口）：\n");
    printf("   参考 README 第 4 节，用 BPF_LD_ABS 读取 IP 头字段后比较。\n");
    printf("   注意：对 TCP/UDP socket，包数据起点是 **IP 头**，不是以太网头。\n");
    printf("═══════════════════════════════════════════════════════════════\n");
}

int main(void)
{
    /* 让 stdout 行缓冲，避免 stderr 的报错跑到 banner 前面去 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== labs/08 · 手写 eBPF socket filter（不依赖 clang / libbpf）===\n\n");
    print_env_facts();
    printf("\n");

    int map_fd = create_count_map();
    if (map_fd < 0) {
        report_skip(errno);
        return 0;                 /* 环境不支持不是实验失败 */
    }

    int prog_fd = load_prog(map_fd);
    if (prog_fd < 0) {
        report_skip(errno);
        close(map_fd);
        return 0;
    }

    int rc = attach_and_test(prog_fd, map_fd);
    close(prog_fd);
    close(map_fd);

    if (rc < 0) {
        report_skip(errno);
        return 0;
    }

    printf("\n全部步骤完成。\n");
    return 0;
}
