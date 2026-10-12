/*
 * xdp_drop.h —— XDP 示例的**共享配置**
 *
 * 为什么要有这个文件？
 *   BPF 侧（clang -target bpf 编译的 xdp_drop.bpf.c）和用户态侧
 *   （gcc 编译的 xdp_drop.c）必须就「端口号 / 程序名 / map 名」达成一致，
 *   否则会出现"程序跑起来了但读不到统计"这种最难查的静默故障。
 *   把这些约定集中到一个头文件里，是消灭这类不一致的标准做法。
 *
 * 设计约束（很重要）：
 *   这个头文件**同时**被 BPF 目标和用户态目标包含。
 *   BPF 程序用 `clang -target bpf` 编译时，能用的头文件极其有限
 *   （没有 libc、没有 asm/ 的天然搜索路径），所以这里刻意
 *   **不包含任何头文件、不使用任何类型**，只放预处理宏 —— 零依赖，两边都能用。
 *
 * 注意：下面的 map 名必须与 xdp_drop.bpf.c 里 BPF map 变量的**变量名**逐字一致，
 *       因为 libbpf 是通过 ELF/BTF 里的符号名去找 map 的
 *       （见 xdp_drop.c 里的 bpf_object__find_map_fd_by_name）。
 */
#ifndef XDP_DROP_H
#define XDP_DROP_H

/* ---- 程序 ---- */

/* 要丢弃的 UDP 目的端口（主机字节序）。BPF 侧用 bpf_htons() 转成网络序比较。 */
#define XDP_DROP_UDP_PORT   9999

/* BPF 程序名 = xdp_drop.bpf.c 里那个函数的函数名（libbpf 按名字查找） */
#define XDP_PROG_NAME       "xdp_drop_udp"

/* 程序所在 ELF section，即 SEC("xdp") 里的字符串 */
#define XDP_PROG_SEC        "xdp"

/* 编译出来的 BPF 目标文件名（用户态 loader 打开它） */
#define XDP_OBJ_PATH        "xdp_drop.bpf.o"

/* ---- map ---- */

/* ARRAY map，1 个条目：key 固定为 XDP_STATS_KEY，value 是 u64 计数 */
#define XDP_MAP_STATS       "xdp_stats"   /* 收到的 IPv4/UDP 包总数 */
#define XDP_MAP_DROPS       "xdp_drops"   /* 其中被 XDP_DROP 丢弃的数量 */
#define XDP_STATS_KEY       0

#endif /* XDP_DROP_H */
