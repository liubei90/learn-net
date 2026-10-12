/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ip.h —— 最小 IPv4 首部解析
 */
#ifndef IP_H
#define IP_H

#include <stddef.h>
#include <stdint.h>

/* 解析结果：只保留我们真正需要的字段 */
struct ipv4_info {
	size_t   hdr_len;	/* 首部长度（字节，含选项） */
	size_t   total_len;	/* IP 总长（首部 + 负载） */
	size_t   payload_len;	/* 负载长度 */
	uint8_t  protocol;
	uint32_t saddr;		/* 网络序 */
	uint32_t daddr;		/* 网络序 */
	uint16_t id;
	uint8_t  ttl;
};

/*
 * 校验并解析一个 IPv4 包。
 * 返回 0 成功，-1 失败（版本错、IHL 非法、长度不一致、首部校验和错）。
 * 失败时通过 fprintf 说明原因，便于观察为什么丢包。
 */
int ipv4_parse(const uint8_t *buf, size_t len, struct ipv4_info *out);

#endif /* IP_H */
