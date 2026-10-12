# labs/07-http-parser —— HTTP/1.1 增量解析器与教学服务器
#
# all 目标：编译单元测试 + 教学服务器（必须零警告，否则视为失败）

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith \
           -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes
LDFLAGS ?=

BINS = test_parser http_server

.PHONY: all clean test run

all: $(BINS)

test_parser: test_parser.o http_parser.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

http_server: http_server.o http_parser.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c http_parser.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 单元测试：同一报文用「整包 / 逐字节 / 穷举切分点」三种喂法，结果必须一致
test: test_parser
	./test_parser

run: all
	./run.sh

clean:
	rm -f $(BINS) *.o
