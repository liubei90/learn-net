/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arp_who_has.c —— 构造并发送一个真实的 ARP 请求，然后等应答
 *
 * 用法: ./arp_who_has <ifname> <target-ip> [source-ip]
 *   例: ./arp_who_has veth0 10.9.9.2
 *
 * 这就是 `arping` 的最小实现：内核自己的 ARP 逻辑（net/ipv4/arp.c 的
 * arp_solicit）也会发出同样的帧，我们只是手工拼一遍，从而看清
 * 「who-has」报文到底长什么样。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "arp.h"

int main(int argc, char **argv)
{
	const char *ifname;
	const char *target_str;
	uint8_t my_mac[MAC_LEN];
	uint8_t bcast[MAC_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint32_t my_ip, target_ip;
	int fd, idx, i;
	struct sockaddr_ll sll;
	uint8_t frame[sizeof(struct eth_hdr) + sizeof(struct arp_msg)];
	struct eth_hdr *eth = (struct eth_hdr *)frame;
	struct arp_msg *arp = (struct arp_msg *)(frame + sizeof(struct eth_hdr));
	char b1[INET_ADDRSTRLEN], b2[INET_ADDRSTRLEN], m[MAC_STR_LEN];

	if (argc < 3) {
		fprintf(stderr, "用法: %s <ifname> <target-ip> [source-ip]\n",
			argv[0]);
		return 2;
	}
	ifname = argv[1];
	target_str = argv[2];

	if (if_get_mac(ifname, my_mac) < 0)
		return 1;
	if (argc >= 4) {
		/* 允许显式指定源 IP（例如在本机还没配地址时做实验） */
		struct in_addr a;

		if (inet_pton(AF_INET, argv[3], &a) != 1) {
			fprintf(stderr, "非法源 IP: %s\n", argv[3]);
			return 2;
		}
		my_ip = a.s_addr;
	} else if (if_get_ipv4(ifname, &my_ip) < 0) {
		return 1;
	}
	if (inet_pton(AF_INET, target_str, &target_ip) != 1) {
		fprintf(stderr, "非法目标 IP: %s\n", target_str);
		return 2;
	}

	idx = if_get_index(ifname);
	if (idx < 0) {
		fprintf(stderr, "接口 %s 不存在\n", ifname);
		return 1;
	}

	/* ---- 组装以太网头 ---- */
	memcpy(eth->dst, bcast, MAC_LEN);	/* ARP 请求是广播 */
	memcpy(eth->src, my_mac, MAC_LEN);
	eth->ethertype = htons(ETH_P_ARP);

	/* ---- 组装 ARP 请求 ---- */
	/*
	 * 逐字段赋值而不是写一个初始化列表：packed 结构体成员可能未对齐，
	 * 且我们希望显式地表达每个字段的字节序。
	 */
	memset(arp, 0, sizeof *arp);
	arp->htype = htons(ARPHRD_ETHER);	/* 1  以太网 */
	arp->ptype = htons(ETH_P_IP);		/* 0x0800 IPv4 */
	arp->hlen  = MAC_LEN;
	arp->plen  = IPV4_LEN;
	arp->oper  = htons(ARPOP_REQUEST);	/* 1  request */
	memcpy(arp->sha, my_mac, MAC_LEN);
	memcpy(arp->spa, &my_ip, IPV4_LEN);
	/* tha 全 0：我们还不知道对端 MAC —— 这正是要问的问题 */
	memcpy(arp->tpa, &target_ip, IPV4_LEN);

	mac_to_str(my_mac, m);
	ipv4_to_str(my_ip, b1, sizeof b1);
	ipv4_to_str(target_ip, b2, sizeof b2);
	printf("[*] 接口=%s 本机=%s/%s\n", ifname, b1, m);
	printf("[*] 发送 ARP 请求: who-has %s tell %s (广播)\n", b2, b1);

	fd = arp_socket_open(ifname, htons(ETH_P_ARP));
	if (fd < 0)
		return 1;

	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ARP);
	sll.sll_ifindex = idx;
	sll.sll_halen = MAC_LEN;
	memcpy(sll.sll_addr, bcast, MAC_LEN);

	for (i = 0; i < 3; i++) {
		ssize_t n = sendto(fd, frame, sizeof frame, 0,
				   (struct sockaddr *)&sll, sizeof sll);

		if (n < 0) {
			perror("sendto");
			close(fd);
			return 1;
		}
		printf("[*] 已发送 %zd 字节（第 %d 次）\n", n, i + 1);
		if (i < 2)
			sleep(1);
	}

	/* ---- 等应答：只接受来自目标 IP 的 ARP REPLY ---- */
	printf("[*] 等待应答（5 秒）...\n");
	{
		struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
		uint8_t buf[2048];

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		for (;;) {
			struct sockaddr_ll from;
			socklen_t flen = sizeof from;
			ssize_t n = recvfrom(fd, buf, sizeof buf, 0,
					     (struct sockaddr *)&from, &flen);
			struct arp_msg *r;
			uint32_t r_spa;
			uint16_t r_oper;

			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					printf("[!] 超时：没有收到 ARP 应答\n");
					printf("    常见原因：目标 IP 不存在、接口没 UP、\n");
					printf("    不在同一广播域、或被 arp_ignore 抑制。\n");
					break;
				}
				perror("recvfrom");
				break;
			}
			if (n < (ssize_t)(sizeof(struct eth_hdr) + sizeof(struct arp_msg)))
				continue;
			r = (struct arp_msg *)(buf + sizeof(struct eth_hdr));
			memcpy(&r_oper, &r->oper, sizeof r_oper);
			memcpy(&r_spa, r->spa, IPV4_LEN);
			if (ntohs(r_oper) != ARPOP_REPLY)
				continue;
			if (r_spa != target_ip)
				continue;

			printf("[+] 收到 ARP 应答！\n");
			arp_dump(r);
			printf("[+] 结论: %s is-at ", target_str);
			mac_to_str(r->sha, m);
			printf("%s\n", m);
			break;
		}
	}

	close(fd);
	return 0;
}
