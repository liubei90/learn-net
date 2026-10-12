# SPDX-License-Identifier: GPL-2.0
#
# ARP / AF_PACKET 实验。
#
# 编译不需要任何特权，`all` 恒退出码 0。
# 运行需要 CAP_NET_RAW：见 README（本机用 unshare -Urn 跑）。

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings

# 注意：这里 *不* 加 -D_GNU_SOURCE。
# 每个 .c 文件在包含任何头文件之前自己 #define _GNU_SOURCE，
# 若命令行再加一次就会触发 "redefined" 警告（-Werror 下直接失败）。

BINS := arp_sniff arp_who_has arp_gratuitous raw_send_udp

.PHONY: all clean

all: $(BINS)

# 公共工具
arp.o: arp.c arp.h
	$(CC) $(CFLAGS) -c -o $@ arp.c

arp_sniff: arp_sniff.c arp.o arp.h
	$(CC) $(CFLAGS) -o $@ arp_sniff.c arp.o

arp_who_has: arp_who_has.c arp.o arp.h
	$(CC) $(CFLAGS) -o $@ arp_who_has.c arp.o

arp_gratuitous: arp_gratuitous.c arp.o arp.h
	$(CC) $(CFLAGS) -o $@ arp_gratuitous.c arp.o

# raw_send_udp 不需要 arp.o（只演示 IP/UDP 与校验和）
raw_send_udp: raw_send_udp.c
	$(CC) $(CFLAGS) -o $@ raw_send_udp.c

clean:
	rm -f $(BINS) arp.o
