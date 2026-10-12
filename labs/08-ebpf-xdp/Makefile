# labs/08-ebpf-xdp —— eBPF / XDP
#
# 目标：
#   all   —— 必须退出码 0。手写指令的示例（不依赖 clang/libbpf）一定编译；
#            libbpf 风格的 XDP 示例只在工具链齐全时才编译，否则打印 SKIP。
#   run   —— 跑手写示例（本机会打印 SKIP 及原因）
#   xdp   —— 单独构建 XDP 示例
#   clean

CC      ?= gcc
CLANG   ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
LDFLAGS ?=

# Debian/Ubuntu 的 uapi 头文件（linux/types.h → asm/types.h）在 multiarch 目录下，
# 而 `clang -target bpf` 不会像 gcc 那样自动把 /usr/include/<multiarch> 加进搜索路径。
# 不加这一行，BPF 程序里 `#include <linux/bpf.h>` 会直接报 "asm/types.h: No such file"。
# 用 $(CC) -dumpmachine 拿到准确的 multiarch 三元组（本机是 x86_64-linux-gnu）。
MULTIARCH := $(shell $(CC) -dumpmachine 2>/dev/null)
BPF_CFLAGS = -O2 -g -target bpf -D__TARGET_ARCH_x86 \
             $(if $(MULTIARCH),-I/usr/include/$(MULTIARCH))

# libbpf 风格示例的依赖探测
HAVE_CLANG  := $(shell command -v $(CLANG) 2>/dev/null)
HAVE_LIBBPF := $(shell test -f /usr/include/bpf/bpf_helpers.h && echo yes)
HAVE_LIBBPF_LIB := $(shell ldconfig -p 2>/dev/null | grep -q libbpf && echo yes)

BIN_RAW = bpf_socket_filter
BIN_XDP = xdp_drop
BPF_OBJ = xdp_drop.bpf.o

.PHONY: all run xdp clean check-toolchain

all: $(BIN_RAW)
	@$(MAKE) --no-print-directory xdp

$(BIN_RAW): bpf_socket_filter.c bpf_insns.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── XDP 示例：工具链齐全才构建 ────────────────────────────────────────
xdp: xdp_drop.bpf.c xdp_drop.c xdp_drop.h
	@if [ -z "$(HAVE_CLANG)" ] || [ -z "$(HAVE_LIBBPF)" ] || [ -z "$(HAVE_LIBBPF_LIB)" ]; then \
	  echo "SKIP: 无法构建 XDP/libbpf 示例，缺少:"; \
	  [ -z "$(HAVE_CLANG)" ]      && echo "        · clang（用于把 C 编译成 eBPF 字节码，-target bpf）"; \
	  [ -z "$(HAVE_LIBBPF)" ]     && echo "        · /usr/include/bpf/bpf_helpers.h（libbpf-dev）"; \
	  [ -z "$(HAVE_LIBBPF_LIB)" ] && echo "        · libbpf 运行库（libbpf1 / libbpf-dev）"; \
	  echo "      安装（Debian/Ubuntu）: sudo apt-get install -y clang llvm libbpf-dev"; \
	  echo "      安装（RHEL 系）      : sudo dnf install -y clang llvm libbpf-devel"; \
	  echo "      装好后执行: make xdp && sudo ./$(BIN_XDP) <ifname>"; \
	  echo "      （不需要 clang/libbpf 的那个示例仍可用: ./$(BIN_RAW)）"; \
	  exit 0; \
	fi; \
	echo "工具链齐全，开始构建 XDP 示例..."; \
	$(CLANG) $(BPF_CFLAGS) -c xdp_drop.bpf.c -o $(BPF_OBJ) && \
	$(CC) $(CFLAGS) -o $(BIN_XDP) xdp_drop.c -lbpf -lelf -lz && \
	echo "构建完成: $(BPF_OBJ) $(BIN_XDP)"

check-toolchain:
	@echo "clang            : $(if $(HAVE_CLANG),$(HAVE_CLANG),<缺失>)"
	@echo "bpf_helpers.h    : $(if $(HAVE_LIBBPF),存在,<缺失>)"
	@echo "libbpf 运行库     : $(if $(HAVE_LIBBPF_LIB),存在,<缺失>)"
	@echo "内核 bpf.h       : $(if $(wildcard /usr/include/linux/bpf.h),存在,<缺失>)"
	@echo "unprivileged_bpf_disabled: $(shell cat /proc/sys/kernel/unprivileged_bpf_disabled 2>/dev/null || echo 未知)"

run: all
	@echo
	@echo "=== 运行手写指令示例 ==="
	./$(BIN_RAW)

clean:
	rm -f $(BIN_RAW) $(BIN_XDP) $(BPF_OBJ) *.o
