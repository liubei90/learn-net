/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tun.c —— /dev/net/tun 的打开与接口配置
 *
 * 内核侧对应：drivers/net/tun.c
 *   - ioctl(TUNSETIFF)  -> tun_set_iff()   创建/绑定 tun 网络设备
 *   - 用户态 read()     -> tun_chr_read_iter() -> tun_do_read()  从队列取 skb
 *   - 用户态 write()    -> tun_chr_write_iter() -> tun_get_user() 把数据装进 skb
 *   - 内核发往 tun 的包 -> tun_net_xmit() -> tun_net_xmit() 入队并唤醒用户态
 *
 * 「内核为什么要把 skb 拷给用户态」：
 *   路由表里指向 tun0 的包，在经过 ip_finish_output2 -> neigh_output ->
 *   dev_hard_start_xmit 之后会进入 tun 的 ndo_start_xmit（tun_net_xmit）。
 *   此时内核不再继续往下走（下面没有真实硬件），而是把这个 skb 放进
 *   该 tun 设备关联的 socket 接收队列，并唤醒阻塞在 read() 上的用户态进程。
 *   反过来，用户态 write() 的数据会被 tun_get_user() 包装成 skb，
 *   用 netif_rx() 重新注入协议栈的接收路径。
 *   所以 TUN 本质上是「把网络栈的收发两端接到一个用户态文件描述符上」，
 *   每次跨越都要做一次内存拷贝 + 上下文切换 —— 这就是 VPN 的性能成本来源。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_tun.h>

#include "tun.h"

int tun_alloc(const char *name)
{
	struct ifreq ifr;
	int fd;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0) {
		perror("open(/dev/net/tun)");
		fprintf(stderr,
			"提示：需要 1) 内核 tun 驱动已加载（modprobe tun）\n"
			"      2) 设备节点存在（mknod /dev/net/tun c 10 200）\n"
			"      3) CAP_NET_ADMIN\n"
			"      本机实测：/dev/net/tun 不存在且 tun 驱动未加载，"
			"故无法运行（见 README）。\n");
		return -1;
	}

	memset(&ifr, 0, sizeof ifr);
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	/* ifr_name 只有 IFNAMSIZ 字节，必须留出 NUL */
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);

	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		perror("ioctl(TUNSETIFF)");
		close(fd);
		return -1;
	}

	/* 内核可能改写了实际使用的名字，回报给调用者 */
	printf("[*] TUN 设备就绪: %s (fd=%d)\n", ifr.ifr_name, fd);
	return fd;
}

/* 取一个用于配置接口的 AF_INET 控制 socket */
static int ctrl_socket(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0)
		perror("socket(AF_INET, SOCK_DGRAM)");
	return fd;
}

int tun_configure(const char *name, const char *ip, const char *netmask)
{
	int fd;
	struct ifreq ifr;
	struct sockaddr_in *sin;

	fd = ctrl_socket();
	if (fd < 0)
		return -1;

	/* 地址 */
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &sin->sin_addr) != 1) {
		fprintf(stderr, "非法 IP: %s\n", ip);
		close(fd);
		return -1;
	}
	if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
		perror("ioctl(SIOCSIFADDR)");
		close(fd);
		return -1;
	}

	/* 掩码 */
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	sin = (struct sockaddr_in *)&ifr.ifr_netmask;
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, netmask, &sin->sin_addr) != 1) {
		fprintf(stderr, "非法掩码: %s\n", netmask);
		close(fd);
		return -1;
	}
	if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
		perror("ioctl(SIOCSIFNETMASK)");
		close(fd);
		return -1;
	}
	close(fd);
	printf("[*] 已配置 %s = %s/%s\n", name, ip, netmask);
	return 0;
}

int tun_set_up(const char *name)
{
	int fd;
	struct ifreq ifr;

	fd = ctrl_socket();
	if (fd < 0)
		return -1;

	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
		perror("ioctl(SIOCGIFFLAGS)");
		close(fd);
		return -1;
	}
	/*
	 * IFF_UP 是「管理上 up」，IFF_RUNNING 是「链路可用」。
	 * 对 tun 而言设置 IFF_UP 后内核会把 RUNNING 也带上。
	 */
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
		perror("ioctl(SIOCSIFFLAGS)");
		close(fd);
		return -1;
	}
	close(fd);
	printf("[*] 已置 %s UP\n", name);
	return 0;
}
