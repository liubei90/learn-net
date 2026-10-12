/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtnl.h —— 极简 rtnetlink 客户端公共接口（不依赖 libnl）
 *
 * 设计意图：
 *   1) 把 netlink 的「传输层」（socket/send/recv/多部分消息）与
 *      「编解码层」（TLV 属性解析）分离开，业务代码只关心属性。
 *   2) 属性解析做成通用的 rtattr 表（tb[]）形式，和内核/iproute2 一致，
 *      这样可以无限复用，而不需要为每种消息写一份解析代码。
 */
#ifndef RTNL_H
#define RTNL_H

#include <stddef.h>
#include <stdint.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/*
 * 打开并 bind 一个 NETLINK_ROUTE socket。
 * 返回 fd，失败返回 -1 并打印 errno。
 *
 * 注意：对于「查询」类请求，nl_pid 填 0 即可，内核会自动分配一个
 * 端口号（等于进程 pid，或在同 pid 多 socket 时自行分配）。bind 的作用
 * 是让内核把回复投递到我们，而不是让它自己挑一个匿名地址。
 */
int nl_open(void);

/*
 * 发送一条 dump 请求并循环收取回复，直到 NLMSG_DONE。
 *
 *   type    : RTM_GETLINK / RTM_GETADDR / RTM_GETROUTE ...
 *   flags   : 一般用 NLM_F_REQUEST | NLM_F_DUMP
 *   payload : family 专属头（ifinfomsg / ifaddrmsg / rtmsg），可为 NULL
 *   plen    : payload 长度（字节）
 *   cb      : 每收到一条有效消息调用一次；返回 <0 表示中止
 *
 * 返回 0 表示收到 NLMSG_DONE，-1 表示出错。
 *
 * 为什么需要 seq：dump 是多部分（multipart）的，内核会分多个 datagram
 * 把消息发回来。seq 用于把回复归属到本次请求；如果进程内有多个 netlink
 * socket/请求并发，靠 seq 区分是必须的。
 */
int nl_dump(int fd, uint16_t type, uint16_t flags,
	    const void *payload, size_t plen,
	    int (*cb)(struct nlmsghdr *h, void *arg), void *arg);

/*
 * 通用 TLV 解析器：把属性流按 rta_type 索引进 tb[]。
 *
 * 这是 iproute2 的经典写法。RTA_OK() 会校验 rta_len 是否合法，
 * RTA_NEXT() 会按 RTA_ALIGN 前进，因此天然抵御畸形/截断消息。
 *
 * tb       : 调用者提供的数组，大小至少 max+1
 * max      : 支持的最大 type（如 IFLA_MAX / IFA_MAX / RTA_MAX）
 * rta,len  : 属性流首地址与剩余长度
 * 返回值   : 遍历结束后的指针（正常为 NULL），用于嵌套场景
 */
struct rtattr *parse_rtattr(struct rtattr **tb, int max,
			    struct rtattr *rta, int len);

/*
 * 解析嵌套属性：先取外层属性 rta 的 payload，再当成一段新的属性流解析。
 * 例：IFLA_LINKINFO 里再套 IFLA_INFO_KIND。
 */
struct rtattr *parse_rtattr_nested(struct rtattr **tb, int max,
				   struct rtattr *rta);

/* 把一段属性按十六进制打印（MAC 地址、未知二进制属性都用它）。 */
void rtnl_print_hex(const struct rtattr *rta);

/* 各种内核枚举 -> 可读字符串；未识别时返回 NULL，由调用者打印数字。 */
const char *rtnl_operstate_name(uint8_t s);
const char *rtnl_scope_name(uint8_t s);
const char *rtnl_proto_name(uint8_t p);
const char *rtnl_table_name(uint8_t t);
const char *rtnl_rtn_type_name(uint8_t t);

#endif /* RTNL_H */
