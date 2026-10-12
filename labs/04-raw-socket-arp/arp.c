/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arp.c —— AF_PACKET / ARP 公共工具实现
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "arp.h"

int if_get_index(const char *ifname)
{
	unsigned int idx = if_nametoindex(ifname);

	return idx ? (int)idx : -1;
}

/*
 * 取接口 MAC：AF_INET + SOCK_DGRAM 的 ioctl 是最省事的办法。
 * （另一种途径是 RTM_GETLINK 的 IFLA_ADDRESS，见 labs/03。）
 */
int if_get_mac(const char *ifname, uint8_t mac[MAC_LEN])
{
	int fd;
	struct ifreq ifr;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket(AF_INET, SOCK_DGRAM)");
		return -1;
	}
	memset(&ifr, 0, sizeof ifr);
	/* 用 strncpy 并补 NUL：ifr_name 只有 IFNAMSIZ 字节，溢出会踩坏栈 */
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		fprintf(stderr, "SIOCGIFHWADDR(%s): %s\n", ifname, strerror(errno));
		close(fd);
		return -1;
	}
	memcpy(mac, ifr.ifr_hwaddr.sa_data, MAC_LEN);
	close(fd);
	return 0;
}

int if_get_ipv4(const char *ifname, uint32_t *ip_net)
{
	int fd;
	struct ifreq ifr;
	struct sockaddr_in *sin;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket(AF_INET, SOCK_DGRAM)");
		return -1;
	}
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
		fprintf(stderr, "SIOCGIFADDR(%s): %s\n", ifname, strerror(errno));
		close(fd);
		return -1;
	}
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	*ip_net = sin->sin_addr.s_addr;	/* 已经是网络序 */
	close(fd);
	return 0;
}

int arp_socket_open(const char *ifname, uint16_t ethertype)
{
	int fd;
	int idx;
	struct sockaddr_ll sll;
	struct packet_mreq mreq;

	fd = socket(AF_PACKET, SOCK_RAW, ethertype);
	if (fd < 0) {
		perror("socket(AF_PACKET, SOCK_RAW)");
		fprintf(stderr,
			"提示：AF_PACKET 需要 CAP_NET_RAW。\n"
			"      非特权环境下请在 user+net namespace 里运行：\n"
			"        unshare -Urn --map-root-user ...\n");
		return -1;
	}

	idx = if_get_index(ifname);
	if (idx < 0) {
		fprintf(stderr, "接口 %s 不存在\n", ifname);
		close(fd);
		return -1;
	}

	/*
	 * 用 SO_BINDTODEVICE 把套接字钉在接口上。需要 CAP_NET_RAW，
	 * 但它比「靠 bind() 过滤」更强：它会让内核在收包路径早期就
	 * 只把该接口的帧投递过来。
	 */
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
		       (socklen_t)strlen(ifname)) < 0) {
		perror("setsockopt(SO_BINDTODEVICE)");
		close(fd);
		return -1;
	}

	/* 加入该接口的「全组播/混杂」以外的原始接收：PACKET_MR_PROMISC 可选 */
	memset(&mreq, 0, sizeof mreq);
	mreq.mr_ifindex = idx;
	mreq.mr_type = PACKET_MR_PROMISC;
	/* 失败不算致命（在 netns 里通常能成功） */
	if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq,
		       sizeof mreq) < 0)
		fprintf(stderr, "warning: PACKET_MR_PROMISC 失败: %s\n",
			strerror(errno));

	/* bind() 到接口：决定 recvfrom 能收到哪个接口的帧 */
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = ethertype;
	sll.sll_ifindex = idx;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
		perror("bind(AF_PACKET)");
		close(fd);
		return -1;
	}
	return fd;
}

void mac_to_str(const uint8_t mac[MAC_LEN], char out[MAC_STR_LEN])
{
	snprintf(out, MAC_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *ipv4_to_str(uint32_t ip_net, char *out, size_t len)
{
	struct in_addr a;

	a.s_addr = ip_net;
	return inet_ntop(AF_INET, &a, out, (socklen_t)len);
}

const char *arp_oper_name(uint16_t oper_net)
{
	switch (ntohs(oper_net)) {
	case ARPOP_REQUEST:	return "REQUEST";
	case ARPOP_REPLY:	return "REPLY";
	case ARPOP_RREQUEST:	return "RARP-REQUEST";
	case ARPOP_RREPLY:	return "RARP-REPLY";
	case ARPOP_InREQUEST:	return "InARP-REQUEST";
	case ARPOP_InREPLY:	return "InARP-REPLY";
	default:		return "UNKNOWN";
	}
}

/*
 * 逐字段打印 ARP 报文。
 * 所有多字节字段都是网络序，必须 ntohs/ntohl 之后再打印，
 * 否则在大端机上看着对、小端机上打印出一堆 0x0800 之类的怪值。
 */
void arp_dump(const struct arp_msg *a)
{
	char sha[MAC_STR_LEN], tha[MAC_STR_LEN];
	char spa[INET_ADDRSTRLEN], tpa[INET_ADDRSTRLEN];
	uint32_t s_spa, s_tpa;
	uint16_t s_ptype;

	memcpy(&s_spa, a->spa, IPV4_LEN);
	memcpy(&s_tpa, a->tpa, IPV4_LEN);
	memcpy(&s_ptype, &a->ptype, sizeof s_ptype);

	mac_to_str(a->sha, sha);
	mac_to_str(a->tha, tha);
	ipv4_to_str(s_spa, spa, sizeof spa);
	ipv4_to_str(s_tpa, tpa, sizeof tpa);

	printf("    ARP: htype=%u ptype=0x%04x hlen=%u plen=%u oper=%u(%s)\n",
	       ntohs(a->htype), ntohs(s_ptype), a->hlen, a->plen,
	       ntohs(a->oper), arp_oper_name(a->oper));
	printf("         sender: mac=%s ip=%s\n", sha, spa);
	printf("         target: mac=%s ip=%s\n", tha, tpa);
}
