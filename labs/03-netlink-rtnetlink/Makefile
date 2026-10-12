# SPDX-License-Identifier: GPL-2.0
#
# 手写 rtnetlink 查询工具。刻意不链接 libnl：
# 依赖越少，越能看清 netlink 报文的真实布局。
#
# all 目标只是构建，不执行任何需要特权的操作，因此退出码恒为 0。

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
CFLAGS  += -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
CPPFLAGS += -D_GNU_SOURCE

BIN     := rtnl_dump
OBJS    := rtnl.o rtnl_dump.o

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

rtnl.o: rtnl.c rtnl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ rtnl.c

rtnl_dump.o: rtnl_dump.c rtnl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ rtnl_dump.c

# 本机无需特权即可运行（只读查询）
run: $(BIN)
	./$(BIN) all

clean:
	rm -f $(BIN) $(OBJS)
