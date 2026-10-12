# SPDX-License-Identifier: GPL-2.0
#
# labs/06-kernel-module-netfilter/Makefile
#
# 外部内核模块（out-of-tree module）的标准 Kbuild 骨架。
#
# 为什么不能直接 "gcc -o netlab.ko netlab.c"？
#   内核模块不是普通可执行文件：它需要
#     1) 内核编译时生成的自动配置文件（include/generated/autoconf.h）；
#     2) 内核内部的头文件搜索路径（-I 一大堆）；
#     3) MODPOST 阶段对导出符号（EXPORT_SYMBOL）的校验和 CRC/版本信息；
#     4) 段布局（.modinfo、__ksymtab、.gnu.linkonce.this_module）与
#        vermagic（版本魔法）字符串，否则 insmod 会直接拒绝。
#   这些只有内核自带的 Kbuild（kbuild 的 Makefile + scripts/）知道怎么做，
#   所以外部模块必须借内核的构建系统来编译。

obj-m += netlab.o

# 允许覆盖：make KDIR=/path/to/kernel/build
KDIR ?= /lib/modules/$(shell uname -r)/build

# CURDIR 由 make 设为当前工作目录（make -C 之后就是本目录）。
MODDIR := $(CURDIR)

# 有些发行版把 kmod 工具放在 /sbin，而普通用户的 PATH 里没有 /sbin。
# 这里 fallback 到绝对路径，保证 make log / make load 不会莫名其妙地
# 找不到命令（本次实验环境 uid=1000，PATH 里确实没有 /sbin）。
MODINFO ?= $(shell command -v modinfo 2>/dev/null || echo /sbin/modinfo)
INSMOD  ?= $(shell command -v insmod  2>/dev/null || echo /sbin/insmod)
RMMOD   ?= $(shell command -v rmmod   2>/dev/null || echo /sbin/rmmod)

MODNAME := netlab

.PHONY: all clean load unload reload log info check-env help

# ------------------------------------------------------------------ #
# 默认目标：编译出 netlab.ko                                          #
# ------------------------------------------------------------------ #
all:
	@if [ ! -d "$(KDIR)" ]; then \
		echo "ERROR: 内核构建目录不存在: $(KDIR)"; \
		echo "       请先安装内核头文件，例如:"; \
		echo "         sudo apt install linux-headers-$$(uname -r)"; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(MODDIR) modules
	@echo
	@echo "==> 产物:"
	@ls -l $(MODDIR)/$(MODNAME).ko

clean:
	$(MAKE) -C $(KDIR) M=$(MODDIR) clean
	@rm -f $(MODDIR)/*.o $(MODDIR)/*.ko $(MODDIR)/*.mod $(MODDIR)/*.mod.c \
	       $(MODDIR)/Module.symvers $(MODDIR)/modules.order
	@rm -rf $(MODDIR)/.tmp_versions
	@echo "==> 已清理（.ko 已删除）"

# ------------------------------------------------------------------ #
# 特权操作：非 root 只打印说明，不报错（exit 0）                        #
# ------------------------------------------------------------------ #
load:
	@if [ "$$(id -u)" != "0" ]; then \
		echo "SKIP: 当前 uid=$$(id -u)，不是 root。"; \
		echo "      加载内核模块需要 CAP_SYS_MODULE，普通用户无法完成。"; \
		echo "      请在 root shell（或 sudo）里执行："; \
		echo "        sudo insmod $(MODDIR)/$(MODNAME).ko"; \
		echo "      编译本身不需要 root，make all 已经成功。"; \
		exit 0; \
	else \
		$(MAKE) -C $(KDIR) M=$(MODDIR) modules && \
		$(INSMOD) $(MODDIR)/$(MODNAME).ko && \
		echo "==> 已 insmod。查看内核日志: dmesg | tail -n 20"; \
	fi

unload:
	@if [ "$$(id -u)" != "0" ]; then \
		echo "SKIP: 当前 uid=$$(id -u)，不是 root，无法 rmmod。"; \
		echo "      请在 root shell 里执行： sudo rmmod $(MODNAME)"; \
		exit 0; \
	else \
		$(RMMOD) $(MODNAME) && echo "==> 已 rmmod。"; \
	fi

reload: unload load

log:
	@if ! command -v dmesg >/dev/null 2>&1; then \
		echo "NOTE: 本机没有 dmesg 命令（可试 journalctl -k）"; \
	elif ! dmesg >/dev/null 2>&1; then \
		echo "NOTE: dmesg 被 kernel.dmesg_restrict=$$(cat /proc/sys/kernel/dmesg_restrict 2>/dev/null) 限制。"; \
		echo "      请用 root 执行: sudo dmesg | tail -n 30"; \
		echo "      或:            sudo journalctl -k | tail -n 30"; \
	else \
		dmesg | tail -n 30; \
	fi

info:
	@if [ ! -f $(MODDIR)/$(MODNAME).ko ]; then \
		echo "还没有编译，请先 make all"; \
		exit 1; \
	fi
	$(MODINFO) $(MODDIR)/$(MODNAME).ko

# ------------------------------------------------------------------ #
# 环境自检：把"这台机器现在能不能编译/加载"一次说清楚                   #
# ------------------------------------------------------------------ #
check-env:
	@echo "uname -r      : $$(uname -r)"
	@echo "KDIR          : $(KDIR)  [$$( [ -d '$(KDIR)' ] && echo exists || echo MISSING )]"
	@echo "uid           : $$(id -u)"
	@echo "CapEff        : $$(grep CapEff /proc/self/status | awk '{print $$2}')"
	@echo "gcc           : $$(gcc --version 2>/dev/null | head -1)"
	@echo "make          : $$(make --version 2>/dev/null | head -1)"
	@echo "modinfo       : $(MODINFO)"
	@echo "insmod        : $(INSMOD)"
	@echo "unprivileged_bpf_disabled : $$(cat /proc/sys/kernel/unprivileged_bpf_disabled 2>/dev/null)"
	@if [ "$$(id -u)" != "0" ]; then \
		echo; \
		echo "结论: 可以编译 (make all)，但不能 insmod/rmmod —— 需要 root。"; \
	fi

help:
	@echo "make all        编译 netlab.ko（不需要 root）"
	@echo "make clean      清理构建产物"
	@echo "make load       insmod（需要 root；非 root 会打印说明并退出 0）"
	@echo "make unload     rmmod（需要 root）"
	@echo "make reload     unload + load"
	@echo "make log        dmesg | tail"
	@echo "make info       modinfo netlab.ko"
	@echo "make check-env  打印当前环境是否具备编译/加载条件"
