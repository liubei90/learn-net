/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtnl_dump.c —— 用裸 rtnetlink 复刻 `ip link` / `ip addr` / `ip route`
 *
 * 用法：
 *   ./rtnl_dump link     # 等价 ip -j link show
 *   ./rtnl_dump addr     # 等价 ip -j addr show
 *   ./rtnl_dump route    # 等价 ip -j route show
 *   ./rtnl_dump all
 *
 * 全部是「只读查询」，因此不需要任何 capability —— 这是 netlink
 * 相比 /dev/... 或 ioctl 的一大优势：读和写用同一套接口，但权限
 * 由内核按操作类型分别校验。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>		/* if_indextoname() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "rtnl.h"

/* ---------------- 小工具 ---------------- */

static const char *ifname(uint32_t idx)
{
	static char buf[IF_NAMESIZE];
	const char *p = if_indextoname(idx, buf);

	return p ? p : "?";
}

/*
 * 把 netlink 里的二进制地址按 family 转成可读字符串。
 *
 * 注意：缓冲区由调用者提供，绝不能用 static 缓冲区！
 * 否则在 printf("via=%s prefsrc=%s", gw, prefsrc) 这种一次调用里，
 * 两个指针会指向同一块内存，最终打印出同一个（最后一个写入的）值。
 * 这是 C 里非常隐蔽的一类 bug —— 实测时默认路由的网关就被打印成了
 * prefsrc。凡是「返回字符串指针」的函数都要交代清楚缓冲区归属。
 */
static const char *addr_str(int family, const void *data, size_t len,
			    char *buf, size_t buflen)
{
	if (family == AF_INET && len >= 4) {
		if (inet_ntop(AF_INET, data, buf, (socklen_t)buflen))
			return buf;
	} else if (family == AF_INET6 && len >= 16) {
		if (inet_ntop(AF_INET6, data, buf, (socklen_t)buflen))
			return buf;
	}
	return NULL;
}

/* ---------------- RTM_GETLINK ---------------- */

/*
 * ifinfomsg 后面跟一串 TLV 属性。
 * IFLA_RTA() 直接算出第一个属性的地址：NLMSG_DATA + sizeof(ifinfomsg)。
 * 消息总长里减去头部，就是属性的总长度。
 */
static int cb_link(struct nlmsghdr *h, void *arg)
{
	struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(h);
	int len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof *ifi));
	struct rtattr *tb[IFLA_MAX + 1] = { 0 };
	const char *name = "?";
	const char *oper = NULL;
	uint32_t mtu = 0;
	const char *kind = NULL;
	int have_mac = 0;

	(void)arg;
	parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

	if (tb[IFLA_IFNAME])
		name = (const char *)RTA_DATA(tb[IFLA_IFNAME]);
	if (tb[IFLA_MTU])
		mtu = *(uint32_t *)RTA_DATA(tb[IFLA_MTU]);
	if (tb[IFLA_OPERSTATE])
		oper = rtnl_operstate_name(*(uint8_t *)RTA_DATA(tb[IFLA_OPERSTATE]));
	if (tb[IFLA_ADDRESS])
		have_mac = 1;

	/*
	 * IFLA_LINKINFO 是嵌套属性：里面再有 IFLA_INFO_KIND（"veth"/"bridge"/
	 * "tun"...）和 IFLA_INFO_DATA。这里演示 parse_rtattr_nested 的用法。
	 */
	if (tb[IFLA_LINKINFO]) {
		struct rtattr *li[IFLA_INFO_MAX + 1] = { 0 };

		parse_rtattr_nested(li, IFLA_INFO_MAX, tb[IFLA_LINKINFO]);
		if (li[IFLA_INFO_KIND])
			kind = (const char *)RTA_DATA(li[IFLA_INFO_KIND]);
	}

	printf("link  ifindex=%-3d ifname=%-16s mtu=%-6u operstate=%-14s",
	       ifi->ifi_index, name, mtu,
	       oper ? oper : "(unparsed)");
	printf(" type=0x%04x", ifi->ifi_type);
	if (kind)
		printf(" kind=%s", kind);
	printf("\n");

	if (have_mac) {
		printf("        address=");
		rtnl_print_hex(tb[IFLA_ADDRESS]);
		printf("\n");
	}
	if (tb[IFLA_IFALIAS]) {
		printf("        alias=%s\n",
		       (const char *)RTA_DATA(tb[IFLA_IFALIAS]));
	}
	return 0;
}

/* ---------------- RTM_GETADDR ---------------- */

static int cb_addr(struct nlmsghdr *h, void *arg)
{
	struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(h);
	int len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof *ifa));
	struct rtattr *tb[IFA_MAX + 1] = { 0 };
	char lbuf[INET6_ADDRSTRLEN], pbuf[INET6_ADDRSTRLEN];
	const char *local = NULL, *peer = NULL;

	(void)arg;
	parse_rtattr(tb, IFA_MAX, IFA_RTA(ifa), len);

	if (tb[IFA_LOCAL])
		local = addr_str(ifa->ifa_family, RTA_DATA(tb[IFA_LOCAL]),
				 RTA_PAYLOAD(tb[IFA_LOCAL]),
				 lbuf, sizeof lbuf);
	if (tb[IFA_ADDRESS])
		peer = addr_str(ifa->ifa_family, RTA_DATA(tb[IFA_ADDRESS]),
				RTA_PAYLOAD(tb[IFA_ADDRESS]),
				pbuf, sizeof pbuf);

	/*
	 * 语义细节（实测踩过的坑）：
	 *   - IPv4：内核填 IFA_LOCAL（本端）+ IFA_ADDRESS；
	 *   - IPv6：内核常常只填 IFA_ADDRESS，没有 IFA_LOCAL。
	 * 所以「本端地址」必须做回退，否则 IPv6 地址会全部打印成 '-'。
	 * iproute2 的 print_addrinfo() 里也是这么处理的。
	 *
	 * 另外，在点对点链路（TUN/PPP）里 IFA_ADDRESS 是「对端」地址，
	 * IFA_LOCAL 才是本端 —— 这就是 iproute2 输出里 local/peer 的由来。
	 */
	if (!local)
		local = peer;
	printf("addr  ifindex=%-3d ifname=%-16s family=%-2d prefixlen=%-3u scope=%-9s",
	       ifa->ifa_index, ifname(ifa->ifa_index), ifa->ifa_family,
	       ifa->ifa_prefixlen,
	       rtnl_scope_name(ifa->ifa_scope) ? rtnl_scope_name(ifa->ifa_scope) : "-");
	printf(" local=%-24s", local ? local : "-");
	if (peer && local && strcmp(peer, local) != 0)
		printf(" peer=%s", peer);
	if (tb[IFA_LABEL])
		printf(" label=%s", (const char *)RTA_DATA(tb[IFA_LABEL]));
	printf(" flags=0x%02x\n", ifa->ifa_flags);
	return 0;
}

/* ---------------- RTM_GETROUTE ---------------- */

static int cb_route(struct nlmsghdr *h, void *arg)
{
	struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(h);
	int len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof *rtm));
	struct rtattr *tb[RTA_MAX + 1] = { 0 };
	char gbuf[INET6_ADDRSTRLEN], pbuf[INET6_ADDRSTRLEN];
	char dbuf[INET6_ADDRSTRLEN];
	const char *dst = NULL, *gw = NULL, *prefsrc = NULL;
	const char *type, *proto, *scope, *table;
	uint32_t oif = 0, prio = 0;
	char dstbuf[INET6_ADDRSTRLEN + 8];

	(void)arg;
	parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);

	if (tb[RTA_DST]) {
		const char *s = addr_str(rtm->rtm_family, RTA_DATA(tb[RTA_DST]),
					 RTA_PAYLOAD(tb[RTA_DST]),
					 dbuf, sizeof dbuf);
		if (s) {
			snprintf(dstbuf, sizeof dstbuf, "%s/%u", s, rtm->rtm_dst_len);
			dst = dstbuf;
		}
	} else {
		/* 没有 RTA_DST 且前缀长度为 0 -> 默认路由 */
		snprintf(dstbuf, sizeof dstbuf, "default");
		dst = dstbuf;
	}
	if (tb[RTA_GATEWAY])
		gw = addr_str(rtm->rtm_family, RTA_DATA(tb[RTA_GATEWAY]),
			      RTA_PAYLOAD(tb[RTA_GATEWAY]), gbuf, sizeof gbuf);
	if (tb[RTA_PREFSRC])
		prefsrc = addr_str(rtm->rtm_family, RTA_DATA(tb[RTA_PREFSRC]),
				   RTA_PAYLOAD(tb[RTA_PREFSRC]),
				   pbuf, sizeof pbuf);
	if (tb[RTA_OIF])
		oif = *(uint32_t *)RTA_DATA(tb[RTA_OIF]);
	if (tb[RTA_PRIORITY])
		prio = *(uint32_t *)RTA_DATA(tb[RTA_PRIORITY]);

	type = rtnl_rtn_type_name(rtm->rtm_type);
	proto = rtnl_proto_name(rtm->rtm_protocol);
	scope = rtnl_scope_name(rtm->rtm_scope);
	/*
	 * 表 id：优先看 RTA_TABLE（32 位，能表达 >255 的表），
	 * 否则退回 rtmsg.rtm_table（8 位，历史遗留）。
	 */
	if (tb[RTA_TABLE])
		table = rtnl_table_name((uint8_t)*(uint32_t *)RTA_DATA(tb[RTA_TABLE]));
	else
		table = rtnl_table_name(rtm->rtm_table);
	if (!table)
		table = "(other)";

	printf("route dst=%-22s ", dst);
	printf("via=%-16s dev=%-16s ", gw ? gw : "-",
	       oif ? ifname(oif) : "-");
	printf("table=%-8s proto=%-8s scope=%-8s type=%-11s",
	       table, proto ? proto : "?", scope ? scope : "?", type ? type : "?");
	if (prio)
		printf(" metric=%u", prio);
	if (prefsrc)
		printf(" prefsrc=%s", prefsrc);
	printf(" dst_len=%u\n", rtm->rtm_dst_len);
	return 0;
}

/* ---------------- main ---------------- */

static void usage(const char *prog)
{
	fprintf(stderr, "用法: %s {link|addr|route|all}\n", prog);
}

static int do_link(int fd)
{
	struct ifinfomsg ifi;

	memset(&ifi, 0, sizeof ifi);
	ifi.ifi_family = AF_UNSPEC;	/* 不限定 family：link 是 L2 概念 */
	return nl_dump(fd, RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP,
		       &ifi, sizeof ifi, cb_link, NULL);
}

static int do_addr(int fd)
{
	struct ifaddrmsg ifa;

	memset(&ifa, 0, sizeof ifa);
	ifa.ifa_family = AF_UNSPEC;	/* v4 + v6 都要 */
	return nl_dump(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP,
		       &ifa, sizeof ifa, cb_addr, NULL);
}

static int do_route(int fd)
{
	struct rtmsg rtm;

	memset(&rtm, 0, sizeof rtm);
	/*
	 * family=AF_UNSPEC + 不指定 table：内核会把所有 address family
	 * 的所有路由表都 dump 出来（包括 local 表），因此输出条数通常
	 * 比 `ip route show`（只看 main 表）多。
	 */
	rtm.rtm_family = AF_UNSPEC;
	return nl_dump(fd, RTM_GETROUTE, NLM_F_REQUEST | NLM_F_DUMP,
		       &rtm, sizeof rtm, cb_route, NULL);
}

int main(int argc, char **argv)
{
	int fd, ret = 0;
	const char *cmd;

	if (argc != 2) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[1];

	fd = nl_open();
	if (fd < 0)
		return 1;

	if (!strcmp(cmd, "link")) {
		ret = do_link(fd);
	} else if (!strcmp(cmd, "addr")) {
		ret = do_addr(fd);
	} else if (!strcmp(cmd, "route")) {
		ret = do_route(fd);
	} else if (!strcmp(cmd, "all")) {
		printf("==== RTM_GETLINK ====\n");
		ret = do_link(fd);
		printf("\n==== RTM_GETADDR ====\n");
		if (!ret)
			ret = do_addr(fd);
		printf("\n==== RTM_GETROUTE ====\n");
		if (!ret)
			ret = do_route(fd);
	} else {
		usage(argv[0]);
		close(fd);
		return 2;
	}

	close(fd);
	if (ret < 0) {
		fprintf(stderr, "%s: netlink dump 失败\n", cmd);
		return 1;
	}
	return 0;
}
