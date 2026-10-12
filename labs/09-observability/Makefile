# =============================================================================
# labs/09-observability/Makefile
#
#   make all            语法检查（bash -n 每个 .sh；python3 -m py_compile 每个 .py）
#   make run            依次运行本机**可以直接跑**的脚本
#   make verify-states  运行受控实验，验证 idiag_states 的位序号约定
#   make clean          清理 Python 字节码缓存
#   make help           显示本帮助
#
# 设计原则：所有目标都不需要 root；需要特权的实验会自行打印 SKIP 并返回 0，
# 因此 `make all` 与 `make run` 在受限环境下也必须成功结束。
# =============================================================================

SHELL := /bin/bash

# shell 脚本清单（顺序即阅读顺序）
SH_SCRIPTS := \
	01-socket-overview.sh \
	02-proc-net-snapshot.sh \
	03-softnet-and-drops.sh \
	04-snmp-counters.sh \
	07-watch-tcp-state.sh

# Python 脚本清单
PY_SCRIPTS := 05-socket-diag.py

.PHONY: all syntax run verify-states clean help

# 默认目标
all: syntax

# ---------------------------------------------------------------------------
# 语法检查：不执行、只解析。这是 `make all` 的唯一职责。
# ---------------------------------------------------------------------------
syntax:
	@echo "=============================================================="
	@echo " 语法检查 (make all)"
	@echo "=============================================================="
	@fail=0; \
	for f in $(SH_SCRIPTS); do \
		if [[ ! -f "$$f" ]]; then \
			echo "  ❌ 缺少脚本: $$f"; fail=1; continue; \
		fi; \
		if bash -n "$$f" 2>/tmp/mk_err_$$$$; then \
			echo "  ✅ bash -n 通过: $$f"; \
		else \
			echo "  ❌ bash -n 失败: $$f"; cat /tmp/mk_err_$$$$; fail=1; \
		fi; \
		rm -f /tmp/mk_err_$$$$; \
	done; \
	for f in $(PY_SCRIPTS); do \
		if [[ ! -f "$$f" ]]; then \
			echo "  ❌ 缺少脚本: $$f"; fail=1; continue; \
		fi; \
		if python3 -m py_compile "$$f"; then \
			echo "  ✅ py_compile 通过: $$f"; \
		else \
			echo "  ❌ py_compile 失败: $$f"; fail=1; \
		fi; \
	done; \
	echo "--------------------------------------------------------------"; \
	if [[ $$fail -eq 0 ]]; then \
		echo " 全部语法检查通过。"; \
	else \
		echo " 存在语法错误，见上。"; exit 1; \
	fi
	@# py_compile 会生成 __pycache__，这里立即清理，保持工作区干净
	@rm -rf __pycache__
	@echo "=============================================================="

# ---------------------------------------------------------------------------
# 运行本机可跑的脚本。07 用较短参数以免 make run 太慢。
# 每个脚本都应自行处理"跑不了"的情况并返回 0。
# ---------------------------------------------------------------------------
run:
	@echo "=============================================================="
	@echo " 运行本机可跑的观测脚本 (make run)"
	@echo "=============================================================="
	./01-socket-overview.sh
	@echo
	./02-proc-net-snapshot.sh
	@echo
	./03-softnet-and-drops.sh
	@echo
	./04-snmp-counters.sh
	@echo
	python3 ./05-socket-diag.py
	@echo
	./07-watch-tcp-state.sh 3 1
	@echo
	@echo "=============================================================="
	@echo " 全部可运行脚本执行完毕。"
	@echo " 06-tracepoint-templates.md 需要 root + tracefs + perf/bpftrace，"
	@echo " 本机不可运行，仅作为模板提供。"
	@echo "=============================================================="

# ---------------------------------------------------------------------------
# 受控验证：idiag_states 的位序号 == TCP 状态编号
# ---------------------------------------------------------------------------
verify-states:
	python3 ./05-socket-diag.py --verify-states

# ---------------------------------------------------------------------------
# 清理
# ---------------------------------------------------------------------------
clean:
	@echo "清理 Python 字节码缓存 ..."
	@rm -rf __pycache__ *.pyc
	@echo "完成。"

help:
	@echo "labs/09-observability 可用目标："
	@echo "  make all            语法检查（bash -n + py_compile），退出码 0"
	@echo "  make run            依次运行本机可跑的脚本（01,02,03,04,05,07）"
	@echo "  make verify-states  验证 NETLINK_SOCK_DIAG 的 idiag_states 位约定"
	@echo "  make clean          清理 __pycache__ / *.pyc"
	@echo "  make help           显示本帮助"
