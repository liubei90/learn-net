/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tun.h —— TUN 设备打开与配置
 */
#ifndef TUN_H
#define TUN_H

/*
 * 打开 /dev/net/tun 并创建一个 IFF_TUN | IFF_NO_PI 的点对点三层设备。
 *   name 传入期望的接口名（如 "tun0"）；内核若该名被占用会报 EEXIST。
 * 返回 fd；失败返回 -1 并打印 errno。
 *
 * IFF_TUN    : 三层设备，读写的是「裸 IP 包」，没有以太网头。
 * IFF_TAP    : 二层设备，读写的是以太网帧。
 * IFF_NO_PI  : 不要在包前面附加 4 字节的「flags + proto」信息头。
 *              不加这个标志时，read() 得到的前 4 字节是 PI，容易踩坑。
 */
int tun_alloc(const char *name);

/*
 * 给接口配 IPv4 地址/掩码并置 UP。
 * 走的是老式 ioctl(SIOCSIFADDR/SIOCSIFNETMASK/SIOCSIFFLAGS)，
 * 需要 CAP_NET_ADMIN。等价命令见 README 的 `ip` 版本。
 */
int tun_configure(const char *name, const char *ip, const char *netmask);
int tun_set_up(const char *name);

#endif /* TUN_H */
