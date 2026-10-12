/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ip.c —— IPv4 首部校验与解析
 *
 * 对应内核：net/ipv4/ip_input.c 的 ip_rcv_core() 做的事情里，
 * 我们只复刻了最关键的三步：
 *   1) 长度/版本合法性检查（ip_rcv_core -> ipv4 首部检查）
 *   2) 首部校验和验证（ip_rcv_core 里 ip_fast_csum 部分）
 *   3) 取出 protocol，决定交给谁（ip_local_deliver_finish 的分发）
 * 真正内核还要处理分片、选项、netfilter 钩子等，这里大幅简化。
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "checksum.h"
#include "ip.h"

/* 复用 glibc 的 struct iphdr：它已经处理好了 version/ihl 的位域字节序 */
#include <netinet/in.h>
#include <netinet/ip.h>

int ipv4_parse(const uint8_t *buf, size_t len, struct ipv4_info *out)
{
	const struct iphdr *ip = (const struct iphdr *)buf;
	size_t hdr_len, total_len;

	if (len < sizeof(struct iphdr)) {
		fprintf(stderr, "[ip] 包太短 (%zu < %zu)\n", len,
			sizeof(struct iphdr));
		return -1;
	}
	if (ip->version != 4) {
		fprintf(stderr, "[ip] 不是 IPv4 (version=%u)\n", ip->version);
		return -1;
	}
	/*
	 * IHL 以 4 字节为单位。最小值 5（=20 字节，无选项）。
	 * 必须判 IHL>=5，否则下面的首部长度会小于 struct iphdr，越界。
	 */
	if (ip->ihl < 5) {
		fprintf(stderr, "[ip] IHL 非法 (%u < 5)\n", ip->ihl);
		return -1;
	}
	hdr_len = (size_t)ip->ihl * 4;
	if (len < hdr_len) {
		fprintf(stderr, "[ip] 缓冲区不足首部 (%zu < %zu)\n", len, hdr_len);
		return -1;
	}

	total_len = ntohs(ip->tot_len);
	/*
	 * tot_len 由对端填写，属不可信输入。必须两条都查：
	 *   - total_len >= hdr_len（否则 payload 长度为负）
	 *   - total_len <= len（否则我们读不到完整包）
	 * 内核里对应的是 pskb_may_pull / ip_rcv_core 的长度校验。
	 */
	if (total_len < hdr_len || total_len > len) {
		fprintf(stderr, "[ip] 长度不一致 (tot_len=%zu, hdr=%zu, buf=%zu)\n",
			total_len, hdr_len, len);
		return -1;
	}

	/* 首部校验和验证：对整段首部（含 check 字段）求校验和应为 0 */
	if (inet_checksum(buf, hdr_len) != 0) {
		fprintf(stderr, "[ip] 首部校验和错误\n");
		return -1;
	}

	out->hdr_len = hdr_len;
	out->total_len = total_len;
	out->payload_len = total_len - hdr_len;
	out->protocol = ip->protocol;
	out->saddr = ip->saddr;
	out->daddr = ip->daddr;
	out->id = ntohs(ip->id);
	out->ttl = ip->ttl;
	return 0;
}
