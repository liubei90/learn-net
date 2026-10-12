/* SPDX-License-Identifier: GPL-2.0 */
/*
 * icmp.h —— 最小 ICMP Echo 应答构造
 */
#ifndef ICMP_H
#define ICMP_H

#include <stddef.h>
#include <stdint.h>

/*
 * 用收到的 ICMP Echo Request 构造 Echo Reply。
 *   req/req_len : 指向 ICMP 报文（**不含** IP 首部）及其长度
 *   out/out_cap : 输出缓冲区
 * 返回写入 out 的字节数，失败返回 0。
 *
 * 规则：type 由 8(ECHO) 改成 0(ECHOREPLY)，code=0，
 *       id/sequence 原样保留（ping 靠它匹配），负载原样回显，
 *       最后重算 ICMP 校验和（范围 = ICMP 头 + 负载，不含 IP 首部）。
 */
size_t icmp_build_echo_reply(const uint8_t *req, size_t req_len,
			     uint8_t *out, size_t out_cap);

#endif /* ICMP_H */
