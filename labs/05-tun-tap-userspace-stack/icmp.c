/* SPDX-License-Identifier: GPL-2.0 */
/*
 * icmp.c —— 构造 ICMP Echo Reply
 *
 * 对应内核：net/ipv4/icmp.c 的 icmp_echo() / icmp_reply()。
 * 内核版还处理速率限制（icmpv4_xrlim_allow）、广播地址、
 * 时间戳请求等多种类型；这里只做 Echo。
 */
#include <stdio.h>
#include <string.h>

#include <netinet/ip_icmp.h>
#include <netinet/in.h>

#include "checksum.h"
#include "icmp.h"

/* ICMP Echo 的固定头长度：type(1)+code(1)+checksum(2)+id(2)+seq(2) */
#define ICMP_ECHO_HDR_LEN 8

size_t icmp_build_echo_reply(const uint8_t *req, size_t req_len,
			     uint8_t *out, size_t out_cap)
{
	struct icmphdr *icmp_out;
	size_t total;

	if (req_len < ICMP_ECHO_HDR_LEN) {
		fprintf(stderr, "[icmp] 报文太短，无法作为 Echo Request\n");
		return 0;
	}
	/* 回显长度 = 请求长度（头 + 负载），不能超过输出缓冲 */
	total = req_len;
	if (total > out_cap) {
		fprintf(stderr, "[icmp] 输出缓冲不足 (%zu > %zu)\n", total, out_cap);
		return 0;
	}

	/* 整体拷贝，再改必要字段：负载原样回显 */
	memcpy(out, req, total);

	icmp_out = (struct icmphdr *)out;
	icmp_out->type = ICMP_ECHOREPLY;	/* 8 -> 0 */
	icmp_out->code = 0;
	/*
	 * id 与 sequence 保持原值。ping 用它们匹配请求与应答；
	 * 若改了，ping 会认为收到「不属于自己的包」而丢弃。
	 */
	icmp_out->checksum = 0;			/* 计算前置 0 */

	/*
	 * ICMP 校验和范围是 ICMP 报文本身（头 + 数据），不含 IP 首部。
	 * 这点和 TCP/UDP 不同：后者还要加 12 字节伪首部
	 *（源 IP、目的 IP、协议号、TCP/UDP 长度）。
	 *
	 * 注意 htons()：inet_checksum() 返回的是校验和的**数值**，
	 * 而 wire 上的该字段是大端，写入 16 位字段时必须转成网络序，
	 * 否则校验和字节序颠倒（见 checksum.h 的说明）。
	 */
	icmp_out->checksum = htons(inet_checksum(out, total));

	return total;
}
