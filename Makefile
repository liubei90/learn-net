# learn-net · 顶层构建入口
#
# 约定：每个 labs/NN-xxx/ 自带一个 Makefile，必须实现 all 与 clean，
#       并且 **all 必须退出码 0**——需要 root 才能运行的实验要在 all 里
#       打印 SKIP 说明后正常退出，绝不中断顶层构建。
#
# 常用目标：
#   make            编译全部实验
#   make check      编译 + 语法检查（不运行需要 root 的东西）
#   make test       跑可离线运行的单元测试
#   make run LAB=07-http-parser   跑指定实验的 run.sh
#   make env        环境自检（本机能跑哪些实验）
#   make list       列出全部实验与权限要求
#   make clean      清理全部产物

SHELL := /bin/bash
# 注意：不能用 $(notdir $(wildcard labs/*/))——路径以 '/' 结尾时 notdir 返回空串。
LABS  := $(sort $(notdir $(wildcard labs/*)))
DOCS  := $(sort $(wildcard docs/*.md))

ifeq ($(strip $(LABS)),)
$(error 没有找到任何 labs/* 目录，请在 learn-net 根目录下执行 make)
endif

.PHONY: all check test env list clean help docs

all:
	@fail=0; \
	for d in $(LABS); do \
	  printf '\n\033[1;34m━━━ 构建 labs/%s ━━━\033[0m\n' "$$d"; \
	  if [ -f "labs/$$d/Makefile" ]; then \
	    $(MAKE) -C "labs/$$d" all || { echo "  ✗ labs/$$d 构建失败"; fail=1; }; \
	  else \
	    echo "  (无 Makefile，跳过)"; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo -e "\n\033[31m存在构建失败\033[0m"; exit 1; fi; \
	echo -e "\n\033[32m全部实验构建完成\033[0m"

# 质量门禁：每个 lab 都从干净状态重建，并断言「零 warning / 零 error」
# （教程承诺所有代码在 -Wall -Wextra 下零警告，这个目标就是用来守住这条承诺的）
check:
	@fail=0; \
	for d in $(LABS); do \
	  printf '\n\033[1;34m━━━ check labs/%s ━━━\033[0m\n' "$$d"; \
	  if [ ! -f "labs/$$d/Makefile" ]; then echo "  (无 Makefile，跳过)"; continue; fi; \
	  out=$$($(MAKE) -C "labs/$$d" clean all 2>&1); rc=$$?; \
	  w=$$(printf '%s\n' "$$out" | grep -cE 'warning:|error:'); \
	  if [ "$$rc" -ne 0 ]; then \
	    echo "  ✗ 构建失败 (rc=$$rc)，输出末尾："; printf '%s\n' "$$out" | tail -15; fail=1; \
	  elif [ "$$w" -ne 0 ]; then \
	    echo "  ✗ 发现 $$w 条 warning/error："; printf '%s\n' "$$out" | grep -E 'warning:|error:'; fail=1; \
	  else \
	    echo "  ✅ 零警告、构建通过"; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo -e "\n\033[31mcheck 失败\033[0m"; exit 1; fi; \
	echo -e "\n\033[32mcheck 通过：全部 lab 从零重建且零警告\033[0m"

# 只跑纯用户态、可离线的单元测试
test:
	@for d in $(LABS); do \
	  if $(MAKE) -C "labs/$$d" -n test >/dev/null 2>&1; then \
	    printf '\n\033[1;34m━━━ test labs/%s ━━━\033[0m\n' "$$d"; \
	    $(MAKE) -C "labs/$$d" test || exit 1; \
	  fi; \
	done
	@echo -e "\n\033[32m全部单元测试通过\033[0m"

run:
	@if [ -z "$(LAB)" ]; then echo "用法: make run LAB=07-http-parser"; exit 2; fi
	@if [ ! -d "labs/$(LAB)" ]; then echo "没有 labs/$(LAB)"; exit 2; fi
	@if [ -x "labs/$(LAB)/run.sh" ]; then \
	  cd "labs/$(LAB)" && ./run.sh; \
	else \
	  echo "labs/$(LAB) 没有可执行的 run.sh"; exit 2; \
	fi

env:
	@bash scripts/env-check.sh

list:
	@echo "文档（docs/）："; \
	for f in $(DOCS); do printf '  %s\n' "$$f"; done; \
	echo; echo "实验（labs/）："; \
	for d in $(LABS); do \
	  desc="$$(grep -m1 '^# ' "labs/$$d/README.md" 2>/dev/null | sed 's/^# //' || true)"; \
	  printf '  %-34s %s\n' "$$d" "$$desc"; \
	done

docs:
	@echo "文档清单："; for f in $(DOCS); do printf '  %s (%s 行)\n' "$$f" "$$(wc -l < $$f)"; done
	@echo; echo "总行数: $$(cat $(DOCS) | wc -l)"

clean:
	@for d in $(LABS); do \
	  if [ -f "labs/$$d/Makefile" ]; then $(MAKE) -C "labs/$$d" clean >/dev/null 2>&1 || true; fi; \
	done
	@echo "已清理全部实验产物"

help:
	@sed -n '2,20p' Makefile | sed 's/^# \{0,1\}//'
