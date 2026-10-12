/* SPDX-License-Identifier: GPL-2.0 */
/*
 * checksum.h —— Internet 校验和（RFC 1071 反码求和）
 */
#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/*
 * 计算 16 位反码求和校验和，返回**校验和的数值**（不是可直接塞进
 * 报文头的位模式）。
 *
 * 关键约定（实测踩过的坑）：
 *   本函数把缓冲区按**大端**解释（p[0] 是高位），返回的 C 是一个普通
 *   的 16 位整数。而报文头的 checksum 字段在 wire 上是**大端**的，
 *   所以把这个 C 写进 struct 的 uint16_t 字段时**必须 htons()**：
 *
 *       hdr->check = htons(inet_checksum(buf, hdr_len));
 *       // 写错成 hdr->check = inet_checksum(...) 会让校验和字节序颠倒，
 *       // 在 x86（小端）上表现为对端/内核一律判定校验和错误。
 *
 *   若缓冲区是裸字节数组，则把 htons(C) 写到校验和字段对应的偏移即可。
 *
 * 验证手段（两个不变量）：
 *   1) RFC 1071 测试向量：对 {0x00,0x01,0xf2,0x03,0xf4,0xf5,0xf6,0xf7}
 *      应得到 0x220d；
 *   2) 自洽性：把 htons(C) 正确填回到报文头后，对**整段**（含该字段）
 *      再调用本函数必须返回 0。
 *
 * 调用者需自行把被校验结构里的 checksum 字段先置 0。
 */
uint16_t inet_checksum(const void *data, size_t len);

#endif /* CHECKSUM_H */
