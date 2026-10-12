/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arp_sniff.c —— 用 AF_PACKET/SOCK_RAW 抓帧并解析 ARP
 *
 * 用法: ./arp_sniff <ifname> [count]
 *   count 省略时无限循环（Ctrl-C 退出），给定时收到 count 个 ARP 帧后退出。
 *
 * 为什么用 SOCK_RAW 而不是 SOCK_DGRAM：
 *   SOCK_DGRAM 模式下内核会把以太网头剥掉，我们就看不到 src/dst MAC；
 *   SOCK_RAW 直接把整帧（含 14 字节以太网头）交给用户态。
 *
 * 为什么这需要权限：AF_PACKET 走的是 dev_add_pack 注册的
 * packet_type，能绕过正常协议栈直接看到 L2 帧，属于「监听」能力，
 * 因此内核要求 CAP_NET_RAW。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "arp.h"

static volatile sig_atomic_t stop;

static void on_sigint(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char **argv)
{
	int fd;
	int want = 0;		/* 0 = 不限 */
	int seen = 0;
	const char *ifname;
	uint8_t mac[MAC_LEN];

	if (argc < 2) {
		fprintf(stderr, "用法: %s <ifname> [count]\n", argv[0]);
		return 2;
	}
	ifname = argv[1];
	if (argc >= 3)
		want = atoi(argv[2]);

	if (if_get_mac(ifname, mac) < 0)
		return 1;
	{
		char m[MAC_STR_LEN];

		mac_to_str(mac, m);
		printf("[*] 监听接口 %s (本机 MAC %s)，ethertype=ETH_P_ALL\n",
		       ifname, m);
	}

	/* ETH_P_ALL：L2 上所有帧都递给我们（不限于 ARP） */
	fd = arp_socket_open(ifname, htons(ETH_P_ALL));
	if (fd < 0)
		return 1;

	signal(SIGINT, on_sigint);
	printf("[*] 等待帧...\n");

	while (!stop) {
		uint8_t buf[2048];
		struct sockaddr_ll sll;
		socklen_t slen = sizeof sll;
		ssize_t n;
		struct eth_hdr *eth;
		uint16_t etype;
		char smac[MAC_STR_LEN], dmac[MAC_STR_LEN];

		n = recvfrom(fd, buf, sizeof buf, 0,
			     (struct sockaddr *)&sll, &slen);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recvfrom");
			break;
		}
		if (n < (ssize_t)sizeof(struct eth_hdr))
			continue;

		eth = (struct eth_hdr *)buf;
		memcpy(&etype, &eth->ethertype, sizeof etype);
		etype = ntohs(etype);

		mac_to_str(eth->src, smac);
		mac_to_str(eth->dst, dmac);
		printf("[帧] len=%-4zd ifindex=%d %s -> %s ethertype=0x%04x\n",
		       n, sll.sll_ifindex, smac, dmac, etype);

		/*
		 * 只对 ARP 做逐字段解析。注意 ARP 报文长度校验：
		 * 必须确认缓冲区里真的有完整的 28 字节，否则越界读。
		 */
		if (etype == ETH_P_ARP &&
		    n >= (ssize_t)(sizeof(struct eth_hdr) + sizeof(struct arp_msg)))
			arp_dump((const struct arp_msg *)(buf + sizeof(struct eth_hdr)));
		else if (etype == ETH_P_IP)
			printf("    IPv4 帧（本 Lab 不解析，交给内核协议栈）\n");
		else if (etype == ETH_P_IPV6)
			printf("    IPv6 帧\n");

		if (etype == ETH_P_ARP) {
			seen++;
			if (want && seen >= want) {
				printf("[*] 已收到 %d 个 ARP 帧，退出\n", seen);
				break;
			}
		}
	}

	close(fd);
	return 0;
}
