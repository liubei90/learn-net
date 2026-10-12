/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arp_gratuitous.c —— 主动发送「免费 ARP」(gratuitous ARP)
 *
 * 用法: ./arp_gratuitous <ifname> <ip> --i-own-this-network
 *
 * ★★★ 合规与安全警告 ★★★
 * 本程序发送的是「未请求的 ARP」，会让同广播域内其他主机更新它们的
 * 邻居缓存（neigh table）。滥用它可以实现 ARP 欺骗 / 中间人攻击，
 * 在未经授权的网络上是**违法**的。
 *   1. 只能在你自己的实验网络、或你获得书面授权的网络中运行；
 *   2. 建议在独立 netns / veth 里做（见 README）；
 *   3. 必须显式给出 --i-own-this-network 才会真正发包，
 *      避免误操作。
 *
 * 免费 ARP 的正当用途：
 *   - 高可用切换（keepalived/VRRP）：主机接管 VIP 后立刻广播，
 *     让交换机和其他主机的 ARP 缓存指向新的 MAC；
 *   - 网卡/地址变更后主动通告；
 *   - 探测 IP 冲突（发送者期望「不应该」有人应答）。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "arp.h"

int main(int argc, char **argv)
{
	const char *ifname, *ip_str;
	int confirmed = 0;
	int fd, idx, i;
	uint8_t my_mac[MAC_LEN];
	uint8_t bcast[MAC_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint32_t ip;
	uint8_t frame[sizeof(struct eth_hdr) + sizeof(struct arp_msg)];
	struct eth_hdr *eth = (struct eth_hdr *)frame;
	struct arp_msg *arp = (struct arp_msg *)(frame + sizeof(struct eth_hdr));
	struct sockaddr_ll sll;
	char b1[INET_ADDRSTRLEN], m[MAC_STR_LEN];

	if (argc < 4) {
		fprintf(stderr,
			"用法: %s <ifname> <ip> --i-own-this-network\n\n"
			"  发送免费 ARP（无请求的 ARP 通告）。\n"
			"  仅在你自己拥有/获得授权的二层广播域内使用！\n",
			argv[0]);
		return 2;
	}
	ifname = argv[1];
	ip_str = argv[2];
	for (i = 3; i < argc; i++)
		if (!strcmp(argv[i], "--i-own-this-network"))
			confirmed = 1;

	if (!confirmed) {
		fprintf(stderr,
			"拒绝执行：缺少 --i-own-this-network。\n"
			"免费 ARP 会改写同网段主机的邻居缓存，滥用属违法行为。\n"
			"确认这是你自己的网络后，请显式加上该参数。\n");
		return 3;
	}

	if (if_get_mac(ifname, my_mac) < 0)
		return 1;
	if (inet_pton(AF_INET, ip_str, &ip) != 1) {
		fprintf(stderr, "非法 IP: %s\n", ip_str);
		return 2;
	}
	idx = if_get_index(ifname);
	if (idx < 0) {
		fprintf(stderr, "接口 %s 不存在\n", ifname);
		return 1;
	}

	/* 以太网头：广播 */
	memcpy(eth->dst, bcast, MAC_LEN);
	memcpy(eth->src, my_mac, MAC_LEN);
	eth->ethertype = htons(ETH_P_ARP);

	/*
	 * 免费 ARP 的经典形态：ARP REQUEST，且
	 * sender IP == target IP（都是本机要通告的地址），
	 * target MAC 留 0（广播）。
	 *
	 * 另一种形态是「ARP REPLY 且目标 MAC 为广播」，两者都能刷新
	 * 邻居缓存；REQUEST 形式更常见（keepalived 用的就是它），
	 * 因为它还能顺带做地址冲突检测。
	 */
	memset(arp, 0, sizeof *arp);
	arp->htype = htons(ARPHRD_ETHER);
	arp->ptype = htons(ETH_P_IP);
	arp->hlen  = MAC_LEN;
	arp->plen  = IPV4_LEN;
	arp->oper  = htons(ARPOP_REQUEST);
	memcpy(arp->sha, my_mac, MAC_LEN);
	memcpy(arp->spa, &ip, IPV4_LEN);
	/* tha = 00:00:00:00:00:00 */
	memcpy(arp->tpa, &ip, IPV4_LEN);	/* 关键：tpa == spa */

	mac_to_str(my_mac, m);
	ipv4_to_str(ip, b1, sizeof b1);
	printf("[!] 已确认授权。接口=%s 通告 %s is-at %s\n", ifname, b1, m);

	fd = arp_socket_open(ifname, htons(ETH_P_ARP));
	if (fd < 0)
		return 1;

	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ARP);
	sll.sll_ifindex = idx;
	sll.sll_halen = MAC_LEN;
	memcpy(sll.sll_addr, bcast, MAC_LEN);

	/* 按惯例连发 3 次，抵抗单帧丢失 */
	for (i = 0; i < 3; i++) {
		ssize_t n = sendto(fd, frame, sizeof frame, 0,
				   (struct sockaddr *)&sll, sizeof sll);

		if (n < 0) {
			perror("sendto");
			close(fd);
			return 1;
		}
		printf("[*] gratuitous ARP #%d 已发送 (%zd 字节)\n", i + 1, n);
		sleep(1);
	}

	printf("[*] 完成。可用 `ip neigh show dev %s` 在邻居侧观察缓存变化。\n",
	       ifname);
	close(fd);
	return 0;
}
