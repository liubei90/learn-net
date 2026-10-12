/* SPDX-License-Identifier: GPL-2.0 */
/*
 * main.c —— 最小用户态协议栈：TUN 上的 ICMP Echo 应答器
 *
 * 数据流（用户态视角）：
 *   ping -I tun0 10.0.0.1
 *     -> 内核路由到 tun0 -> tun_net_xmit -> 队列 + 唤醒
 *     -> 我们 read() 拿到裸 IP 包
 *     -> ipv4_parse() 校验/解析
 *     -> 若是 ICMP Echo Request，icmp_build_echo_reply() 构造应答
 *     -> 改好 IP 首部（源/目的互换 + 重算校验和）
 *     -> write() 回 tun fd -> tun_get_user -> netif_rx -> 内核协议栈
 *     -> ping 收到应答
 *
 * 编译不需要 /dev/net/tun 存在；运行需要（见 README）。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/ip_icmp.h>

#include "checksum.h"
#include "icmp.h"
#include "ip.h"
#include "tun.h"

#define BUF_SIZE 65536

static volatile sig_atomic_t stop;

static void on_sigint(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "tun0";
	const char *ip = (argc > 2) ? argv[2] : "10.0.0.1";
	const char *mask = (argc > 3) ? argv[3] : "255.255.255.0";
	int fd;
	unsigned long served = 0;

	fd = tun_alloc(dev);
	if (fd < 0)
		return 1;
	/* 配置失败不致命：也可以事先用 ip 命令配好 */
	if (tun_configure(dev, ip, mask) < 0)
		fprintf(stderr, "[!] 自动配置失败，请手动执行 README 里的 ip 命令\n");
	else
		tun_set_up(dev);

	signal(SIGINT, on_sigint);
	printf("[*] 用户态协议栈就绪，等待 %s 上的 IP 包... (Ctrl-C 退出)\n", dev);

	while (!stop) {
		static uint8_t buf[BUF_SIZE];
		static uint8_t out[BUF_SIZE];
		struct ipv4_info info;
		struct iphdr *oip;
		const uint8_t *payload;
		struct icmphdr *icmp_in;
		ssize_t n;
		size_t reply_len;

		n = read(fd, buf, sizeof buf);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read(tun)");
			break;
		}
		if (n == 0)
			continue;

		printf("\n[<] 收到 %zd 字节\n", n);
		if (ipv4_parse(buf, (size_t)n, &info) < 0)
			continue;	/* 原因已由 ipv4_parse 打印 */

		{
			char s[INET_ADDRSTRLEN], d[INET_ADDRSTRLEN];
			struct in_addr a, b;

			a.s_addr = info.saddr;
			b.s_addr = info.daddr;
			inet_ntop(AF_INET, &a, s, sizeof s);
			inet_ntop(AF_INET, &b, d, sizeof d);
			printf("    IPv4 %s -> %s  proto=%u ttl=%u id=%u len=%zu\n",
			       s, d, info.protocol, info.ttl, info.id,
			       info.total_len);
		}

		if (info.protocol != IPPROTO_ICMP) {
			printf("    非 ICMP（proto=%u），本最小栈不处理\n",
			       info.protocol);
			continue;
		}
		payload = buf + info.hdr_len;
		icmp_in = (struct icmphdr *)payload;
		printf("    ICMP type=%u code=%u\n", icmp_in->type, icmp_in->code);
		if (icmp_in->type != ICMP_ECHO) {
			printf("    非 Echo Request，忽略\n");
			continue;
		}

		/*
		 * 构造应答。先把请求整包复制出来，再改 IP 首部：
		 * 源/目的互换，TTL 重设为 64，重算首部校验和。
		 */
		memcpy(out, buf, info.total_len);
		oip = (struct iphdr *)out;
		oip->saddr = info.daddr;
		oip->daddr = info.saddr;
		oip->ttl = 64;
		oip->check = 0;
		/* htons()：把校验和数值写成 wire 要求的大端（见 checksum.h） */
		oip->check = htons(inet_checksum(out, info.hdr_len));

		reply_len = icmp_build_echo_reply(payload, info.payload_len,
						 out + info.hdr_len,
						 sizeof out - info.hdr_len);
		if (reply_len == 0)
			continue;

		{
			ssize_t w = write(fd, out, info.hdr_len + reply_len);

			if (w < 0) {
				perror("write(tun)");
				break;
			}
			served++;
			printf("[>] 已回送 %zd 字节 ICMP Echo Reply（累计 %lu）\n",
			       w, served);
		}
	}

	close(fd);
	printf("\n[*] 退出，共应答 %lu 个 Echo Request\n", served);
	return 0;
}
