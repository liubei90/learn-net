/*
 * xdp_drop.bpf.c —— 标准 libbpf 风格的 XDP 程序（需要 clang + libbpf）
 *
 * 本机没有 clang/libbpf，因此这个文件**不会被编译**（Makefile 会打印 SKIP）。
 * 它存在的意义是让你看到一个「真实项目里 XDP 程序长什么样」：
 *   · SEC("xdp") 把函数放进 .xdp 段
 *   · 上下文是 struct xdp_md，只有原始数据指针，没有 skb
 *   · **每一次访问包数据之前都必须做 data_end 边界检查**，否则验证器拒绝加载
 *   · 返回 enum xdp_action（XDP_PASS / XDP_DROP / XDP_TX / XDP_REDIRECT）
 *
 * 编译（在装了 clang + libbpf-dev 的机器上）：
 *   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -c xdp_drop.bpf.c -o xdp_drop.bpf.o
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* BPF 侧与用户态侧共享的约定（端口号、程序名、map 名）。
 * xdp_drop.h 是零依赖的纯宏头文件，因此在 -target bpf 环境下也能安全包含。 */
#include "xdp_drop.h"

/* 统计用的 map：1 个条目，值类型 u64。用 ARRAY 是因为索引固定、无锁且 O(1)。
 * 注意：变量名 xdp_stats / xdp_drops 必须与 xdp_drop.h 里的 XDP_MAP_* 保持一致，
 *       libbpf 就是靠这两个符号名去找 map 的。 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} xdp_stats SEC(".maps");

/* 被丢弃的包数单独统计 —— 这一点很重要：
 * XDP_DROP 丢掉的包**不会**出现在网卡 rx_dropped 里，只能靠程序自己计数。 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} xdp_drops SEC(".maps");

/* 要丢弃的 UDP 目的端口（主机字节序）来自 xdp_drop.h，两边共用一个定义 */

static __always_inline void bump(void *map)
{
    __u32 key = 0;
    __u64 *v = bpf_map_lookup_elem(map, &key);
    if (v)
        __sync_fetch_and_add(v, 1);
}

SEC("xdp")
int xdp_drop_udp(struct xdp_md *ctx)
{
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    /* ① 以太网头：先检查长度，再访问 —— 这是验证器强制要求的写法 */
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    /* 只处理 IPv4（h_proto 是网络字节序，用 bpf_htons 统一） */
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    /* ② IP 头 */
    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;

    if (iph->protocol != IPPROTO_UDP)
        return XDP_PASS;

    /* ③ 用 IHL 算出 L4 头位置（IP 头长度可变！固定写 20 是常见错误） */
    __u32 ihl = (__u32)iph->ihl * 4;
    if (ihl < sizeof(struct iphdr))
        return XDP_PASS;

    struct udphdr *uh = (void *)iph + ihl;
    if ((void *)(uh + 1) > data_end)
        return XDP_PASS;

    /* 先把「收到的 IPv4/UDP 包」计数（总入口计数） */
    bump(&xdp_stats);

    if (uh->dest == bpf_htons(XDP_DROP_UDP_PORT)) {
        bump(&xdp_drops);
        return XDP_DROP;          /* 这个包连 skb 都不会被分配 */
    }

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
