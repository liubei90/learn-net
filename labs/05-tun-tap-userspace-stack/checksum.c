/* SPDX-License-Identifier: GPL-2.0 */
/*
 * checksum.c —— RFC 1071 反码求和
 *
 * 为什么单独放一个文件：IPv4 首部校验和与 ICMP 校验和用的是同一个
 * 算法，只是 ICMP 的校验范围是「ICMP 头 + 数据」（不含 IP 首部）。
 * 把它独立出来才能只写一次、测一次。
 */
#include "checksum.h"

uint16_t inet_checksum(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t sum = 0;

	/* 16 位一组累加；网络序 = 大端，所以高字节在前 */
	while (len > 1) {
		sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
		p += 2;
		len -= 2;
	}
	/* 奇数长度：尾部补一个 0 字节 */
	if (len == 1)
		sum += (uint32_t)p[0] << 8;

	/* 进位回卷：把 32 位和的高 16 位折回低 16 位 */
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);

	return (uint16_t)(~sum);
}
