/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtnl.c —— rtnetlink 传输层 + 通用 TLV 属性解析
 *
 * 只依赖 libc 与内核 UAPI 头文件，不使用 libnl，目的是把
 * 「netlink 报文长什么样」这件事完全暴露出来。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include <linux/if.h>		/* IF_OPER_* */
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "rtnl.h"

/* 收包缓冲区：dump 出来的消息可能很大，一次 recv 尽量多收。 */
#define NL_RECV_BUF (256 * 1024)

int nl_open(void)
{
	int fd;
	struct sockaddr_nl local;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) {
		perror("socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)");
		return -1;
	}

	/*
	 * 调大接收缓冲区：dump 是「内核一次性把一堆消息推给用户态」，
	 * 如果用户态读得慢，socket 接收队列会溢出，内核丢弃消息并回
	 * ENOBUFS（表现为 dump 结果缺项）。真实运维里 netlink 丢消息
	 * 绝大多数就是这个原因，而不是权限问题。
	 */
	{
		int rcvbuf = 4 * 1024 * 1024;
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) < 0)
			perror("setsockopt(SO_RCVBUF)");	/* 非致命，继续 */
	}

	memset(&local, 0, sizeof local);
	local.nl_family = AF_NETLINK;	/* nl_pid = 0 -> 内核分配 */
	if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) {
		perror("bind(netlink)");
		close(fd);
		return -1;
	}
	return fd;
}

int nl_dump(int fd, uint16_t type, uint16_t flags,
	    const void *payload, size_t plen,
	    int (*cb)(struct nlmsghdr *h, void *arg), void *arg)
{
	static uint32_t seq_counter = 1;
	struct sockaddr_nl kernel;
	struct nlmsghdr *nh;
	char *req = NULL;
	char *buf = NULL;
	size_t reqlen;
	uint32_t seq = seq_counter++;
	int ret = -1;

	/*
	 * 请求 = nlmsghdr + family 专属头。用 NLMSG_SPACE() 而不是直接
	 * sizeof 相加，是因为 nlmsg_len 只算头部+负载，而实际要发送的
	 * 字节数要按 NLMSG_ALIGN 对齐到 4 字节边界。
	 */
	reqlen = NLMSG_SPACE(plen);
	req = calloc(1, reqlen);
	if (!req) {
		perror("calloc");
		return -1;
	}
	nh = (struct nlmsghdr *)req;
	nh->nlmsg_len = NLMSG_LENGTH(plen);
	nh->nlmsg_type = type;
	nh->nlmsg_flags = flags;
	nh->nlmsg_seq = seq;
	nh->nlmsg_pid = 0;	/* 发给内核 */
	if (plen && payload)
		memcpy(NLMSG_DATA(nh), payload, plen);

	memset(&kernel, 0, sizeof kernel);
	kernel.nl_family = AF_NETLINK;
	if (sendto(fd, req, reqlen, 0, (struct sockaddr *)&kernel, sizeof kernel) < 0) {
		perror("sendto(netlink)");
		goto out;
	}

	buf = malloc(NL_RECV_BUF);
	if (!buf) {
		perror("malloc");
		goto out;
	}

	for (;;) {
		ssize_t n;
		int rem;
		struct nlmsghdr *h;

		n = recv(fd, buf, NL_RECV_BUF, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recv(netlink)");
			goto out;
		}
		if (n == 0)	/* 内核不会这么干，但防御一下 */
			goto out;

		/*
		 * 一个 datagram 里可能包含多条消息（多部分消息）。
		 * NLMSG_NEXT 会推进指针并递减 rem，注意 rem 必须是有符号
		 * 的 int：NLMSG_OK 内部会做 (int) 比较。
		 */
		rem = (int)n;
		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, rem);
		     h = NLMSG_NEXT(h, rem)) {
			/* 只处理属于本次请求的消息 */
			if (h->nlmsg_seq != seq)
				continue;

			if (h->nlmsg_type == NLMSG_DONE) {
				ret = 0;	/* dump 正常结束 */
				goto out;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e =
					(struct nlmsgerr *)NLMSG_DATA(h);
				/*
				 * error == 0 表示 ACK（成功确认），不是错误。
				 * 这一点非常容易写错：很多人把 NLMSG_ERROR
				 * 一律当失败处理。
				 */
				if (e->error != 0) {
					fprintf(stderr,
						"netlink 错误: %s (errno=%d)\n",
						strerror(-e->error), e->error);
					goto out;
				}
				continue;
			}
			if (h->nlmsg_type == NLMSG_NOOP ||
			    h->nlmsg_type == NLMSG_OVERRUN)
				continue;

			if (cb && cb(h, arg) < 0)
				goto out;
		}
	}

out:
	free(req);
	free(buf);
	return ret;
}

struct rtattr *parse_rtattr(struct rtattr **tb, int max,
			    struct rtattr *rta, int len)
{
	while (RTA_OK(rta, len)) {
		/*
		 * rta_type 可能被恶意/异常构造得很大，必须先判上界，
		 * 否则就是越界写 tb[]。这是解析不可信输入的基本纪律。
		 */
		if (rta->rta_type <= (unsigned)max && tb)
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta, len);
	}
	return rta;
}

struct rtattr *parse_rtattr_nested(struct rtattr **tb, int max,
				   struct rtattr *rta)
{
	if (!rta)
		return NULL;
	/* 递归：把外层属性的 payload 当成一段新的属性流 */
	return parse_rtattr(tb, max, (struct rtattr *)RTA_DATA(rta),
			    (int)RTA_PAYLOAD(rta));
}

void rtnl_print_hex(const struct rtattr *rta)
{
	const unsigned char *p = (const unsigned char *)RTA_DATA(rta);
	size_t len = RTA_PAYLOAD(rta);
	size_t i;

	for (i = 0; i < len; i++)
		printf("%02x%s", p[i], (i + 1 < len) ? ":" : "");
}

/* IF_OPER_* 来自 include/uapi/linux/if.h */
const char *rtnl_operstate_name(uint8_t s)
{
	static const char *const names[] = {
		"UNKNOWN", "NOTPRESENT", "DOWN", "LOWERLAYERDOWN",
		"TESTING", "DORMANT", "UP",
	};
	if (s < sizeof names / sizeof names[0])
		return names[s];
	return NULL;
}

/* RT_SCOPE_* 来自 include/uapi/linux/rtnetlink.h */
const char *rtnl_scope_name(uint8_t s)
{
	switch (s) {
	case RT_SCOPE_UNIVERSE: return "universe";
	case RT_SCOPE_SITE:	return "site";
	case RT_SCOPE_LINK:	return "link";
	case RT_SCOPE_HOST:	return "host";
	case RT_SCOPE_NOWHERE:	return "nowhere";
	default:		return NULL;
	}
}

/* RTPROT_* —— 路由是谁装的（内核/静态/DHCP/路由守护进程...） */
const char *rtnl_proto_name(uint8_t p)
{
	switch (p) {
	case RTPROT_UNSPEC:	return "unspec";
	case RTPROT_REDIRECT:	return "redirect";
	case RTPROT_KERNEL:	return "kernel";
	case RTPROT_BOOT:	return "boot";
	case RTPROT_STATIC:	return "static";
	case RTPROT_RA:		return "ra";
	case RTPROT_DHCP:	return "dhcp";
	default:		return NULL;
	}
}

/* RT_TABLE_* —— 路由表 id。Linux 支持 0..2^32-1，但有 4 张约定俗成的表。 */
const char *rtnl_table_name(uint8_t t)
{
	switch (t) {
	case RT_TABLE_UNSPEC:	return "unspec";
	case RT_TABLE_COMPAT:	return "compat";
	case RT_TABLE_DEFAULT:	return "default";
	case RT_TABLE_MAIN:	return "main";
	case RT_TABLE_LOCAL:	return "local";
	default:		return NULL;
	}
}

/* RTN_* —— 路由类型，决定了这条路由「命中后怎么处理包」 */
const char *rtnl_rtn_type_name(uint8_t t)
{
	switch (t) {
	case RTN_UNSPEC:	return "unspec";
	case RTN_UNICAST:	return "unicast";
	case RTN_LOCAL:		return "local";
	case RTN_BROADCAST:	return "broadcast";
	case RTN_ANYCAST:	return "anycast";
	case RTN_MULTICAST:	return "multicast";
	case RTN_BLACKHOLE:	return "blackhole";
	case RTN_UNREACHABLE:	return "unreachable";
	case RTN_PROHIBIT:	return "prohibit";
	case RTN_THROW:		return "throw";
	case RTN_NAT:		return "nat";
	case RTN_XRESOLVE:	return "xresolve";
	default:		return NULL;
	}
}
