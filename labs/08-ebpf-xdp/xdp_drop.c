/*
 * xdp_drop.c —— XDP 程序的用户态加载器（标准 libbpf 风格）
 *
 * 需要 libbpf。本机没有，因此不参与构建（Makefile 会 SKIP）。
 *
 * 在装有 clang + libbpf-dev 的机器上：
 *   make xdp
 *   sudo ./xdp_drop <ifname>        # 例如 sudo ./xdp_drop lo
 *
 * 它会：
 *   1. 打开 xdp_drop.bpf.o，加载 XDP 程序
 *   2. 用 bpf_xdp_attach 把程序挂到指定网卡的 **native** 模式上
 *   3. 每秒打印一次 map 里的统计
 *   4. Ctrl-C 时卸载（detach）
 *
 * 它演示了 XDP 生命周期里最容易忘的两件事：
 *   · 程序必须**显式 detach**，否则网卡上一直挂着你的程序；
 *   · XDP_DROP 丢的包不会出现在常规网卡统计里，必须读自己的 map。
 */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

/* 与 BPF 侧共享的约定（程序名 / map 名 / 目标文件名 / 丢弃端口）。
 * 全放在 xdp_drop.h 里，避免用户态和 BPF 侧"各写一份常量"导致静默不一致。 */
#include "xdp_drop.h"

static volatile sig_atomic_t g_stop = 0;
static int g_ifindex = 0;
static int g_xdp_fd = -1;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int bump_libbpf_verbose(enum libbpf_print_level level,
                               const char *fmt, va_list args)
{
    if (level == LIBBPF_DEBUG)
        return 0;
    return vfprintf(stderr, fmt, args);
}

static long read_map_u64(int map_fd)
{
    __u32 key = 0;
    __u64 value = 0;
    if (bpf_map_lookup_elem(map_fd, &key, &value) < 0)
        return -1;
    return (long)value;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "用法: %s <ifname>  例如: sudo %s lo\n", argv[0], argv[0]);
        return 2;
    }
    const char *ifname = argv[1];
    g_ifindex = (int)if_nametoindex(ifname);
    if (g_ifindex == 0) {
        fprintf(stderr, "找不到网卡 %s\n", ifname);
        return 1;
    }

    libbpf_set_print(bump_libbpf_verbose);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct bpf_object *obj = bpf_object__open_file(XDP_OBJ_PATH, NULL);
    if (!obj) {
        fprintf(stderr, "打开 %s 失败（先执行 make xdp）\n", XDP_OBJ_PATH);
        return 1;
    }
    if (bpf_object__load(obj)) {
        fprintf(stderr, "加载 BPF 对象失败（需要 CAP_BPF/CAP_NET_ADMIN 或 root）\n");
        bpf_object__close(obj);
        return 1;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name(obj, XDP_PROG_NAME);
    if (!prog) {
        fprintf(stderr, "找不到程序 %s\n", XDP_PROG_NAME);
        bpf_object__close(obj);
        return 1;
    }
    g_xdp_fd = bpf_program__fd(prog);

    /* 关键的挂载动作：等价于 `ip link set dev <ifname> xdp obj xdp_drop.bpf.o sec xdp` */
    int err = bpf_xdp_attach(g_ifindex, g_xdp_fd, XDP_FLAGS_UPDATE_IF_NOEXIST, NULL);
    if (err) {
        fprintf(stderr, "bpf_xdp_attach 失败: %s（需要 CAP_NET_ADMIN）\n", strerror(-err));
        bpf_object__close(obj);
        return 1;
    }
    printf("已把 XDP 程序挂到 %s (ifindex=%d)，native 模式。Ctrl-C 卸载。\n",
           ifname, g_ifindex);

    int stats_fd = bpf_object__find_map_fd_by_name(obj, XDP_MAP_STATS);
    int drops_fd = bpf_object__find_map_fd_by_name(obj, XDP_MAP_DROPS);

    while (!g_stop) {
        sleep(1);
        long total = stats_fd >= 0 ? read_map_u64(stats_fd) : -1;
        long drops = drops_fd >= 0 ? read_map_u64(drops_fd) : -1;
        printf("收到 IPv4/UDP 包 %ld，其中被 XDP_DROP 丢弃 %ld（丢弃阈值: UDP 端口 %d）\n",
               total, drops, XDP_DROP_UDP_PORT);
        fflush(stdout);
    }

    if (bpf_xdp_detach(g_ifindex, XDP_FLAGS_UPDATE_IF_NOEXIST, NULL))
        fprintf(stderr, "警告：detach 失败，网卡上可能仍挂着程序\n");
    else
        printf("\n已卸载 XDP 程序。\n");

    bpf_object__close(obj);
    return 0;
}
