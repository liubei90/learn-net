# labs/02-tcp-state-machine/Makefile
# 目标：make / make clean 均以 0 退出；-Wall -Wextra 下 0 警告（gcc 14.2 实测）
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
LDFLAGS ?=
LDLIBS  ?=

BINS := backlog_probe timewait_demo

.PHONY: all clean run

all: $(BINS)

backlog_probe: backlog_probe.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

timewait_demo: timewait_demo.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

run: all
	./run.sh

clean:
	rm -f $(BINS)
