# labs/01-socket-api-basics/Makefile
# 目标：make / make clean 均以 0 退出；-Wall -Wextra 下 0 警告（gcc 14.2 + glibc 2.41 实测）
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
LDFLAGS ?=
LDLIBS  ?=

BINS := tcp_echo_server tcp_client udp_echo sockopt_probe

.PHONY: all clean run

all: $(BINS)

tcp_echo_server: tcp_echo_server.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

tcp_client: tcp_client.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

udp_echo: udp_echo.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

sockopt_probe: sockopt_probe.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

run: all
	./run.sh

clean:
	rm -f $(BINS)
