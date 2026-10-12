/* SPDX-License-Identifier: GPL-2.0 */
/*
 * raw_send_udp.c —— 从零构造 IPv4 + UDP 报文，并演示首部校验和
 *
 * 用法: ./raw_send_udp <dst-ip> <dst-port> <message>
 *
 * 为什么用它讲校验和：
 *   IPv4 首部校验和是本 Lab 里唯一「必须自己算对、否则内核直接丢包」
 *   的计算。它用的是 RFC 1071 的 16 位反码求和（one's complement sum），
 *   理解它之后再看 TCP/UDP 校验和（多了伪首部）就顺了。
 *
 * 权限：SOCK_RAW + IPPROTO_RAW 需要 CAP_NET_RAW。
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

#include <netinet/ip.h>
#include <netinet/udp.h>

/*
 * RFC 1071 反码求和校验和。
 *
 * 步骤：把数据按 16 位大端累加，进位回卷（fold），最后取反。
 * 要点：
 *   - 奇数长度要补一个 0 字节（padding）；
 *   - 累加用 32 位，最后把高 16 位折回低 16 位；
 *   - 计算前校验和字段必须置 0。
 */
static uint16_t checksum(const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t sum = 0;

	while (len > 1) {
		/* 网络序是大端，所以高位字节先来 */
		sum += ((uint32_t)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len == 1)			/* 奇数尾部：补 0 */
		sum += (uint32_t)p[0] << 8;

	/* 把溢出的进位折回低 16 位（可能要折两次） */
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)(~sum);
}

int main(int argc, char **argv)
{
	int fd;
	struct sockaddr_in dst;
	uint8_t pkt[4096];
	struct iphdr *ip = (struct iphdr *)pkt;
	struct udphdr *udp;
	const char *msg;
	size_t msg_len, udp_len, total;
	uint16_t ip_csum;

	if (argc != 4) {
		fprintf(stderr, "用法: %s <dst-ip> <dst-port> <message>\n",
			argv[0]);
		return 2;
	}
	msg = argv[3];
	msg_len = strlen(msg);
	udp_len = sizeof(struct udphdr) + msg_len;
	total = sizeof(struct iphdr) + udp_len;
	if (total > sizeof pkt) {
		fprintf(stderr, "消息过长\n");
		return 2;
	}

	memset(pkt, 0, sizeof pkt);

	/* ---- IPv4 首部 ---- */
	ip->version  = 4;
	ip->ihl      = sizeof(struct iphdr) / 4;	/* 5 -> 20 字节 */
	ip->tos      = 0;
	ip->tot_len  = htons((uint16_t)total);
	ip->id       = htons(0x1234);
	/*
	 * 不分片标志。注意 ip_off 是网络序，且低 13 位是分片偏移，
	 * 高 3 位是 flags —— 所以位运算前要先转主机序，写完再转回去。
	 */
	ip->frag_off = htons(IP_DF);
	ip->ttl      = 64;
	ip->protocol = IPPROTO_UDP;
	ip->check    = 0;			/* 计算前必须置 0 */
	inet_pton(AF_INET, argv[1], &ip->daddr);
	ip->saddr    = htonl(INADDR_ANY);	/* 让内核填源地址 */

	/* ---- UDP 首部 ---- */
	udp = (struct udphdr *)(pkt + sizeof(struct iphdr));
	udp->source = htons(40000);
	udp->dest   = htons((uint16_t)atoi(argv[2]));
	udp->len    = htons((uint16_t)udp_len);
	udp->check  = 0;	/* UDP 校验和置 0 = 不校验（IPv4 允许） */
	memcpy(pkt + sizeof(struct iphdr) + sizeof(struct udphdr), msg, msg_len);

	/* ---- 计算 IPv4 首部校验和 ---- */
	ip_csum = checksum(ip, sizeof(struct iphdr));
	ip->check = ip_csum;	/* checksum() 已返回网络序结果 */

	printf("[*] IPv4 首部 20 字节，校验和 = 0x%04x\n", ntohs(ip_csum));
	printf("[*] 目标 %s:%s，UDP 长度 %zu，IP 总长 %zu\n",
	       argv[1], argv[2], udp_len, total);

	/*
	 * IPPROTO_RAW 意味着「我自己已经写好 IP 首部」，内核不再加首部、
	 * 但仍然会查路由表选出口设备与源地址。
	 */
	fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	if (fd < 0) {
		perror("socket(AF_INET, SOCK_RAW, IPPROTO_RAW)");
		fprintf(stderr, "提示：需要 CAP_NET_RAW（本机不可用，见 README）。\n");
		return 1;
	}
	{
		int one = 1;

		if (setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &one, sizeof one) < 0)
			perror("setsockopt(IP_HDRINCL)");
	}

	memset(&dst, 0, sizeof dst);
	dst.sin_family = AF_INET;
	inet_pton(AF_INET, argv[1], &dst.sin_addr);

	if (sendto(fd, pkt, total, 0, (struct sockaddr *)&dst, sizeof dst) < 0) {
		perror("sendto");
		close(fd);
		return 1;
	}
	printf("[*] 已发送 %zu 字节\n", total);
	close(fd);
	return 0;
}
