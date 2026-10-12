/*
 * bpf_insns.h —— 手写 eBPF 指令需要的编码宏
 *
 * 为什么不用 <bpf/bpf_helpers.h>（libbpf 提供的那些 BPF_MOV64_IMM 宏）？
 *   因为这个实验的目的就是让你看清「eBPF 程序就是一段指令数组」。
 *   用 libbpf 的宏会把它藏起来：你写的是 C，编译出来是指令，中间发生了什么看不见。
 *   这里的所有编码都直接来自内核 UAPI：
 *     /usr/include/linux/bpf_common.h   （指令类别、操作码、源操作数）
 *     /usr/include/linux/bpf.h          （BPF_ALU64 / BPF_JMP32 / BPF_MOV / BPF_EXIT …）
 *   你可以在自己机器上打开这两个文件逐条核对。
 */
#ifndef BPF_INSNS_H
#define BPF_INSNS_H

#include <linux/bpf.h>
#include <linux/bpf_common.h>

/* ── 10 个 64 位通用寄存器 ──────────────────────────────────────────
 *   r0      返回值 / helper 调用的返回
 *   r1..r5  helper 调用的参数
 *   r6..r9  被调用者保存（跨 helper 调用仍然保留）
 *   r10     只读栈帧指针（512 字节栈）
 */
#define BPF_REG_0 0
#define BPF_REG_1 1
#define BPF_REG_2 2
#define BPF_REG_3 3
#define BPF_REG_4 4
#define BPF_REG_5 5
#define BPF_REG_6 6
#define BPF_REG_7 7
#define BPF_REG_8 8
#define BPF_REG_9 9
#define BPF_REG_10 10

/* ── 算术/搬移（BPF_ALU64 = 64 位运算） ─────────────────────────────
 * code = 类别 | 操作 | 源操作数
 *   BPF_ALU64(0x07) | BPF_MOV(0xb0) | BPF_K(0x00) = 0xb7  →  r_dst = imm
 *   BPF_ALU64(0x07) | BPF_ADD(0x00) | BPF_K(0x00) = 0x07  →  r_dst += imm
 *   BPF_ALU64(0x07) | BPF_MOV(0xb0) | BPF_X(0x08) = 0xbf  →  r_dst = r_src
 */
#define BPF_MOV64_IMM(DST, IMM)                                                \
    ((struct bpf_insn){.code = BPF_ALU64 | BPF_MOV | BPF_K,                    \
                       .dst_reg = (DST),                                       \
                       .imm = (IMM)})

#define BPF_MOV64_REG(DST, SRC)                                                \
    ((struct bpf_insn){.code = BPF_ALU64 | BPF_MOV | BPF_X,                    \
                       .dst_reg = (DST),                                       \
                       .src_reg = (SRC)})

#define BPF_ALU64_IMM(OP, DST, IMM)                                            \
    ((struct bpf_insn){.code = BPF_ALU64 | (OP) | BPF_K,                       \
                       .dst_reg = (DST),                                       \
                       .imm = (IMM)})

#define BPF_ALU64_REG(OP, DST, SRC)                                            \
    ((struct bpf_insn){.code = BPF_ALU64 | (OP) | BPF_X,                       \
                       .dst_reg = (DST),                                       \
                       .src_reg = (SRC)})

#define BPF_ALU32_IMM(OP, DST, IMM)                                            \
    ((struct bpf_insn){.code = BPF_ALU | (OP) | BPF_K,                         \
                       .dst_reg = (DST),                                       \
                       .imm = (IMM)})

/* ── 内存访问 ───────────────────────────────────────────────────────
 *   BPF_ST (0x02) | BPF_MEM(0x60) | BPF_W(0x00) = 0x62 → *(u32*)(dst+off) = imm
 *   BPF_STX(0x03) | BPF_MEM(0x60) | BPF_W(0x00) = 0x63 → *(u32*)(dst+off) = src
 *   BPF_LDX(0x01) | BPF_MEM(0x60) | BPF_W(0x00) = 0x61 → dst = *(u32*)(src+off)
 */
#define BPF_ST_MEM(SIZE, DST, OFF, IMM)                                        \
    ((struct bpf_insn){.code = BPF_ST | BPF_MEM | (SIZE),                      \
                       .dst_reg = (DST),                                       \
                       .off = (OFF),                                           \
                       .imm = (IMM)})

#define BPF_STX_MEM(SIZE, DST, SRC, OFF)                                       \
    ((struct bpf_insn){.code = BPF_STX | BPF_MEM | (SIZE),                     \
                       .dst_reg = (DST),                                       \
                       .src_reg = (SRC),                                       \
                       .off = (OFF)})

#define BPF_LDX_MEM(SIZE, DST, SRC, OFF)                                       \
    ((struct bpf_insn){.code = BPF_LDX | BPF_MEM | (SIZE),                     \
                       .dst_reg = (DST),                                       \
                       .src_reg = (SRC),                                       \
                       .off = (OFF)})

/* ── 跳转 ───────────────────────────────────────────────────────────
 *   BPF_JMP(0x05) | BPF_JEQ(0x10) | BPF_K(0x00) = 0x15 → if dst == imm goto PC+off+1
 *   off 是「相对下一条指令」的偏移，这是最容易数错的地方。
 */
#define BPF_JMP_IMM(OP, DST, IMM, OFF)                                         \
    ((struct bpf_insn){.code = BPF_JMP | (OP) | BPF_K,                         \
                       .dst_reg = (DST),                                       \
                       .off = (OFF),                                           \
                       .imm = (IMM)})

#define BPF_JMP_REG(OP, DST, SRC, OFF)                                         \
    ((struct bpf_insn){.code = BPF_JMP | (OP) | BPF_X,                         \
                       .dst_reg = (DST),                                       \
                       .src_reg = (SRC),                                       \
                       .off = (OFF)})

#define BPF_JMP_A(OFF)                                                         \
    ((struct bpf_insn){.code = BPF_JMP | BPF_JA, .off = (OFF)})

/* ── helper 调用与返回 ──────────────────────────────────────────────
 *   BPF_JMP(0x05) | BPF_CALL(0x80) = 0x85，imm = helper 编号
 *   BPF_JMP(0x05) | BPF_EXIT(0x90) = 0x95
 */
#define BPF_EMIT_CALL(FUNC)                                                    \
    ((struct bpf_insn){.code = BPF_JMP | BPF_CALL, .imm = (FUNC)})

#define BPF_EXIT_INSN()                                                        \
    ((struct bpf_insn){.code = BPF_JMP | BPF_EXIT})

/* ── 把 map fd 载入寄存器（伪指令，占两个槽位） ─────────────────────
 *   code = BPF_LD(0x00) | BPF_DW(0x18) | BPF_IMM(0x00) = 0x18
 *   src_reg = BPF_PSEUDO_MAP_FD(1) 告诉加载器「imm 是 map fd，请替换成 map 指针」
 *   64 位立即数需要两条指令（低 32 位在第一条，高 32 位在第二条）
 */
#define BPF_LD_MAP_FD(DST, FD)                                                 \
    ((struct bpf_insn){.code = BPF_LD | BPF_DW | BPF_IMM,                      \
                       .dst_reg = (DST),                                       \
                       .src_reg = BPF_PSEUDO_MAP_FD,                           \
                       .imm = (FD)}),                                          \
    ((struct bpf_insn){.code = 0, .imm = 0})

/* ── 载入包数据（经典 cBPF 遗留指令，eBPF 里仍可用于 socket filter） ──
 *   BPF_LD(0x00) | BPF_ABS(0x20) | BPF_W(0x00) = 0x20 → r0 = *(u32*)(packet + imm)
 *   注意：对 TCP 套接字来说，packet 起点是 **IP 头**（L2 已被剥掉）；
 *   对 AF_PACKET 套接字才是以太网头。这个差异是很多过滤器写错的根源。
 */
#define BPF_LD_ABS(SIZE, OFF)                                                  \
    ((struct bpf_insn){.code = BPF_LD | BPF_ABS | (SIZE), .imm = (OFF)})

#define BPF_LD_IND(SIZE, SRC, OFF)                                             \
    ((struct bpf_insn){.code = BPF_LD | BPF_IND | (SIZE),                      \
                       .src_reg = (SRC),                                       \
                       .imm = (OFF)})

#define BPF_LD_LEN()                                                           \
    ((struct bpf_insn){.code = BPF_LD | BPF_LEN})

#endif /* BPF_INSNS_H */
