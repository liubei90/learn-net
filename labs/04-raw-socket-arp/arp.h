/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arp.h —— ARP / AF_PACKET 实验公共接口
 *
 * 这里刻意使用 __attribute__((packed)) 自己定义报文结构，而不是
 * 依赖 glibc 的 struct ether_arp：
 *   1) 报文布局就是 wire format，编译器不允许插入 padding，
 *      否则会发出一个内核/对端看不懂的帧；
 *   2) 自己写一遍才能看清 ARP 每个字段的类型与宽度。
 *
 * 注意 packed 的代价：成员可能未对齐，编译器禁止取它们的地址
 * （-Waddress-of-packed-member）。因此下面所有对报文字段的
 * 读写都通过 memcpy / 逐字段赋值完成。
 */
#ifndef ARP_H
#define ARP_H

#include <stddef.h>
#include <stdint.h>

/* ARPHRD_ETHER / ARPOP_REQUEST / ARPOP_REPLY 等常量。
 * 放在公共头里，避免每个 .c 都重复包含。 */
#include <linux/if_arp.h>

#define MAC_LEN		6
#define IPV4_LEN	4
#define MAC_STR_LEN	18	/* "aa:bb:cc:dd:ee:ff" + NUL */

/* 以太网 V2 帧头：14 字节 */
struct eth_hdr {
	uint8_t  dst[MAC_LEN];
	uint8_t  src[MAC_LEN];
	uint16_t ethertype;	/* 网络序 */
} __attribute__((packed));

/*
 * 以太网 + IPv4 场景下的 ARP 报文，固定 28 字节。
 * 通用 ARP 头是前 8 字节（htype/ptype/hlen/plen/oper），
 * 后面是 4 个变长字段；这里按 hlen=6、plen=4 展开。
 */
struct arp_msg {
	uint16_t htype;		/* 硬件类型，以太网 = 1 */
	uint16_t ptype;		/* 上层协议，IPv4 = 0x0800 */
	uint8_t  hlen;		/* 硬件地址长度 = 6 */
	uint8_t  plen;		/* 协议地址长度 = 4 */
	uint16_t oper;		/* 操作码：1=request 2=reply（网络序） */
	uint8_t  sha[MAC_LEN];	/* sender hardware address */
	uint8_t  spa[IPV4_LEN];	/* sender protocol address */
	uint8_t  tha[MAC_LEN];	/* target hardware address */
	uint8_t  tpa[IPV4_LEN];	/* target protocol address */
} __attribute__((packed));

/* ---- 接口信息查询（走 ioctl，需要 /sys 或 netns 上下文即可） ---- */
int if_get_index(const char *ifname);
int if_get_mac(const char *ifname, uint8_t mac[MAC_LEN]);
int if_get_ipv4(const char *ifname, uint32_t *ip_net);	/* 网络序 */

/*
 * 打开一个绑定到指定接口的 AF_PACKET 原始套接字。
 * ethertype 传 htons(ETH_P_ARP) 只收 ARP，传 htons(ETH_P_ALL) 收全部。
 * 需要 CAP_NET_RAW。
 */
int arp_socket_open(const char *ifname, uint16_t ethertype);

/* ---- 格式化与打印 ---- */
void mac_to_str(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN]);
const char *ipv4_to_str(uint32_t ip_net, char *out, size_t len);
const char *arp_oper_name(uint16_t oper_net);
void arp_dump(const struct arp_msg *a);

#endif /* ARP_H */
