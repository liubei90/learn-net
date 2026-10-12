# SPDX-License-Identifier: GPL-2.0
#
# 最小用户态协议栈（TUN 上的 ICMP Echo 应答器）。
#
# 编译不需要 root、不需要 /dev/net/tun、也不需要 tun 驱动；
# 只有 *运行* 才需要它们。因此 all 恒退出码 0。

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings

BIN  := tun_stack
OBJS := main.o tun.o ip.o icmp.o checksum.o

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

main.o: main.c tun.h ip.h icmp.h checksum.h
	$(CC) $(CFLAGS) -c -o $@ main.c

tun.o: tun.c tun.h
	$(CC) $(CFLAGS) -c -o $@ tun.c

ip.o: ip.c ip.h checksum.h
	$(CC) $(CFLAGS) -c -o $@ ip.c

icmp.o: icmp.c icmp.h checksum.h
	$(CC) $(CFLAGS) -c -o $@ icmp.c

checksum.o: checksum.c checksum.h
	$(CC) $(CFLAGS) -c -o $@ checksum.c

clean:
	rm -f $(BIN) $(OBJS)
