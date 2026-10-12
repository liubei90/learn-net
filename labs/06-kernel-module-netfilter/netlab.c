// SPDX-License-Identifier: GPL-2.0
/*
 * netlab.c —— 一个"只观测、不改包"的 netfilter 观测型内核模块
 *
 * 教学目的：
 *   1. 亲手在 NFPROTO_IPV4 的 NF_INET_LOCAL_IN / NF_INET_LOCAL_OUT 两个 hook 点
 *      注册自己的 nf_hook_ops，看清 netfilter 的注册/调用机制；
 *   2. 演示网络数据面（data path）上的统计应当怎么写：per-CPU 无锁计数；
 *   3. 演示 skb 线性/非线性数据的正确解析方式（skb_header_pointer），
 *      以及分片包必须"先判分片、再摸 L4 头"这条硬规则；
 *   4. 用 seq_file + procfs 把内核态数据导出给用户态，并支持 echo reset 清零。
 *
 * 安全约定（非常重要）：
 *   - hook 回调永远返回 NF_ACCEPT，绝不 DROP / STOLEN / QUEUE，
 *     绝不修改 skb 内容、绝不改 mark —— 本模块是"旁观者"，不是防火墙。
 *   - hook 回调运行在网络软中断 / 进程上下文里，绝对不可以睡眠
 *     （不能 kmalloc(GFP_KERNEL)、不能 mutex_lock、不能 copy_to_user、不能 msleep）。
 *
 * 编译：make            （产出 netlab.ko）
 * 加载：sudo insmod netlab.ko
 * 观测：cat /proc/netlab_stats ; dmesg | tail
 * 卸载：sudo rmmod netlab
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/spinlock.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/math64.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <linux/debugfs.h>
#include <linux/fs.h>

#include <net/net_namespace.h>		/* init_net			*/
#include <net/ip.h>			/* ip_is_fragment()		*/

#include <linux/netfilter.h>		/* struct nf_hook_ops, NF_ACCEPT */
#include <linux/netfilter_ipv4.h>	/* NF_IP_PRI_*			*/

#define NETLAB_NAME		"netlab"
#define NETLAB_PROC_NAME	"netlab_stats"	/* => /proc/netlab_stats	*/
#define NETLAB_PROTO_MAX	256		/* IP 协议号取值空间 0..255	*/
#define NETLAB_NDEV		8		/* 每 CPU 每方向最多跟踪的设备数	*/
#define NETLAB_RING_MAX		256		/* 环形缓冲硬上限		*/
#define NETLAB_DEVNAME_LEN	IFNAMSIZ	/* 16				*/

/* ------------------------------------------------------------------ */
/* 1. 模块参数（/sys/module/netlab/parameters/ 下可见）                  */
/* ------------------------------------------------------------------ */

/*
 * sample_limit：环形缓冲保留多少条"最近记录"。
 *   统计总数走 per-CPU 计数（每包都记），而"最近记录"是抽样展示用的，
 *   因此可以独立调整大小，避免把内存浪费在没人看的明细上。
 */
static unsigned int sample_limit = 16;
module_param(sample_limit, uint, 0644);
MODULE_PARM_DESC(sample_limit,
		 "number of most-recent packet records kept in the ring buffer (1..256)");

/* enabled：总开关。置 0 时 hook 立即返回 NF_ACCEPT，用于 A/B 对比开销。 */
static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "master switch: when 0 the hooks return NF_ACCEPT immediately");

/* ------------------------------------------------------------------ */
/* 2. per-CPU 统计结构                                                  */
/* ------------------------------------------------------------------ */

/*
 * 为什么用 per-CPU 而不是全局 atomic64？
 *   网络路径上，同一个全局计数器会被所有 CPU（更准确地说所有软中断）疯狂地
 *   atomic_inc，缓存行在 CPU 之间来回弹跳（cache-line bouncing），
 *   cache 一致性流量本身就会吃掉相当一部分转发性能。
 *   per-CPU 变量让每个 CPU 只写自己那份副本（写的是本 CPU 的 cache line），
 *   读的时候再求和，把"写竞争"彻底变成"读聚合"。
 *   Linux 网络栈自己的 /proc/net/softnet_stat、每设备统计都是这个套路。
 *
 * 为什么这里可以不用锁？
 *   x86-64 上 this_cpu_inc() 编译成单条 `incq %gs:<offset>` 指令，
 *   同 CPU 上的抢占/中断不会把这条指令撕开；而"别的 CPU 只写它自己的副本"，
 *   所以不存在跨 CPU 竞争。注意：不是所有 per-CPU RMW 都是单指令
 *   （this_cpu_add_return 之类需要 preempt_disable() 配合）。
 */
struct netlab_dev_stat {
	char	name[NETLAB_DEVNAME_LEN];
	u64	pkts;
	u64	bytes;
};

struct netlab_cpu_stats {
	u64			pkts;				/* 本 CPU 总包数	*/
	u64			bytes;				/* 本 CPU 总字节数	*/
	u64			hook_pkts[NF_INET_NUMHOOKS];	/* 按 hook 点		*/
	u64			proto[NETLAB_PROTO_MAX];	/* 按 IP 协议号	*/
	struct netlab_dev_stat	in_dev[NETLAB_NDEV];		/* 入向设备	*/
	struct netlab_dev_stat	out_dev[NETLAB_NDEV];		/* 出向设备	*/
};

/* DEFINE_PER_CPU 已经隐含 __percpu 注解，不需要（也不能）手写。 */
static DEFINE_PER_CPU(struct netlab_cpu_stats, netlab_pcpu);

/* ------------------------------------------------------------------ */
/* 3. 最近记录环形缓冲（跨 CPU 共享 => 需要锁）                          */
/* ------------------------------------------------------------------ */

struct netlab_record {
	u64	ts_ns;				/* ktime_get_real_ns()	*/
	u32	len;				/* skb->len		*/
	__be32	saddr;
	__be32	daddr;
	__be16	sport;
	__be16	dport;
	u8	hook;
	u8	proto;
	u8	frag;				/* 1 = 分片（未解析 L4） */
	char	indev[NETLAB_DEVNAME_LEN];
	char	outdev[NETLAB_DEVNAME_LEN];
};

static struct netlab_record	netlab_ring[NETLAB_RING_MAX];
static unsigned int		netlab_ring_head;	/* 下一个写入下标	*/
static u64			netlab_ring_total;	/* 历史累计记录条数	*/
static DEFINE_SPINLOCK(netlab_ring_lock);

/*
 * 全局聚合结果（只在 procfs/debugfs 读取时构建，不在数据面上使用）。
 * 结构约 2.6KB，所以由调用者在堆上分配，不放在内核栈上。
 */
struct netlab_totals {
	u64 pkts;
	u64 bytes;
	u64 hook_pkts[NF_INET_NUMHOOKS];
	u64 proto[NETLAB_PROTO_MAX];
	struct netlab_dev_stat in_dev[NETLAB_NDEV];
	struct netlab_dev_stat out_dev[NETLAB_NDEV];
};

static const char *const netlab_hook_names[NF_INET_NUMHOOKS] = {
	[NF_INET_PRE_ROUTING]	= "PRE_ROUTING",
	[NF_INET_LOCAL_IN]	= "LOCAL_IN",
	[NF_INET_FORWARD]	= "FORWARD",
	[NF_INET_LOCAL_OUT]	= "LOCAL_OUT",
	[NF_INET_POST_ROUTING]	= "POST_ROUTING",
};

static const char *netlab_proto_name(u8 proto)
{
	switch (proto) {
	case IPPROTO_ICMP:	return "ICMP";
	case IPPROTO_TCP:	return "TCP";
	case IPPROTO_UDP:	return "UDP";
	case IPPROTO_ICMPV6:	return "ICMPv6";
	case IPPROTO_GRE:	return "GRE";
	case IPPROTO_ESP:	return "ESP";
	default:		return "OTHER";
	}
}

static unsigned int netlab_effective_ring_len(void)
{
	/*
	 * 必须夹紧到 [1, NETLAB_RING_MAX]：
	 * sample_limit 可以通过 sysfs 修改（/sys/module/netlab/parameters/），
	 * 往里 echo 0 是完全合法的写入；而本函数的返回值会被当作
	 *     netlab_ring[head % ring_len]
	 * 的除数。一旦返回 0，就会在**软中断上下文**里触发 division by zero，
	 * 直接把内核打挂（Oops）。
	 *
	 * 教训：内核代码里任何来自用户态的参数都必须校验，
	 * 而且要在**使用点**再兜一次下界，不能只依赖模块加载时的默认值。
	 */
	return max_t(unsigned int, 1u,
		     min_t(unsigned int, sample_limit, NETLAB_RING_MAX));
}

/* 在给定的 per-CPU 设备表里按名字记账；表满则新增一条（最多 NETLAB_NDEV 条）。 */
static void netlab_dev_account(struct netlab_dev_stat *tab, const char *name,
			       u32 len)
{
	unsigned int i;

	for (i = 0; i < NETLAB_NDEV; i++) {
		if (tab[i].name[0] == '\0') {
			strscpy(tab[i].name, name, NETLAB_DEVNAME_LEN);
			tab[i].pkts = 1;
			tab[i].bytes = len;
			return;
		}
		if (strcmp(tab[i].name, name) == 0) {
			tab[i].pkts++;
			tab[i].bytes += len;
			return;
		}
	}
	/* 表已满：不记录，绝不在这里分配内存（数据面禁止 GFP_KERNEL）。 */
}

/* ------------------------------------------------------------------ */
/* 4. 数据面：netfilter hook 回调                                       */
/* ------------------------------------------------------------------ */

/*
 * 从 skb 中安全地取出 IP 头。
 * skb_header_pointer() 会处理三种情况：
 *   - 数据在 skb->data 指向的线性区：直接返回指针；
 *   - 数据在 fraglist / page 里（非线性 skb）：把需要的字节拷到调用者提供的
 *     buffer（这里是栈上的 _iph）并返回 buffer；
 *   - 数据不完整（被截断）：返回 NULL。
 * 直接用 ip_hdr(skb) 在非线性 skb 上会读到错误内容；
 * 而"先 pskb_may_pull 再读"会改 skb，不是观测者该做的事。
 */
static const struct iphdr *netlab_get_iph(const struct sk_buff *skb,
					  struct iphdr *buf)
{
	return skb_header_pointer(skb, skb_network_offset(skb),
				  sizeof(*buf), buf);
}

/*
 * 从 IP 头之后取 TCP/UDP 端口。
 * 分片包（MF 置位或 offset != 0）的 L4 头只在第一个分片里，且此时
 * iph->ihl 之后的字节在大多数分片里根本不属于 L4 头 —— 强行解析会：
 *   a) 读到别的分片的数据（统计错误）；
 *   b) 越界读（在 paged data 上可能直接内核 oops）。
 * 所以：调用者必须先用 ip_is_fragment() 判掉分片，本函数再做一次长度兜底。
 */
static void netlab_get_ports(const struct sk_buff *skb, const struct iphdr *iph,
			     __be16 *sport, __be16 *dport)
{
	unsigned int ihl, tot_len;
	int l4off;

	*sport = 0;
	*dport = 0;

	/* ihl 以 4 字节为单位；协议栈正常路径会校验 >= 5，观测者必须自己兜底。 */
	if (unlikely(iph->ihl < 5))
		return;

	ihl = (unsigned int)iph->ihl * 4;
	tot_len = (unsigned int)ntohs(iph->tot_len);
	/*
	 * 按协议分别兜底：TCP 头最小 20 字节、UDP 头固定 8 字节。
	 * 这里只是「快速排除明显不合法的包」的悲观预检；真正的越界防护由下面的
	 * skb_header_pointer() 负责（它在内部做 hlen/offset 判断 + skb_copy_bits 双重兜底）。
	 */
	if (iph->protocol == IPPROTO_TCP) {
		if (unlikely(ihl + sizeof(struct tcphdr) > tot_len))
			return;
	} else if (unlikely(ihl + sizeof(struct udphdr) > tot_len)) {
		return;
	}

	l4off = skb_network_offset(skb) + (int)ihl;

	if (iph->protocol == IPPROTO_TCP) {
		struct tcphdr _tcph, *th;

		th = skb_header_pointer(skb, l4off, sizeof(_tcph), &_tcph);
		if (unlikely(!th))
			return;
		*sport = th->source;
		*dport = th->dest;
	} else if (iph->protocol == IPPROTO_UDP) {
		struct udphdr _udph, *uh;

		uh = skb_header_pointer(skb, l4off, sizeof(_udph), &_udph);
		if (unlikely(!uh))
			return;
		*sport = uh->source;
		*dport = uh->dest;
	}
}

static unsigned int netlab_hook(void *priv, struct sk_buff *skb,
				const struct nf_hook_state *state)
{
	struct netlab_cpu_stats *st;
	const struct iphdr *iph;
	struct iphdr _iph;
	struct netlab_record *rec;
	unsigned int ring_len;
	unsigned long flags;
	u8 proto;
	__be16 sport, dport;
	bool frag;
	const char *inname, *outname;
	u32 len;

	/*
	 * priv 就是 nf_hook_ops.priv，本模块没用它；显式 (void) 防止
	 * 未来打开 -Wunused-parameter 时报警。
	 */
	(void)priv;

	/* 总开关关闭时立刻返回：hook 依然被调用，但几乎没有开销。 */
	if (unlikely(!READ_ONCE(enabled)))
		return NF_ACCEPT;

	if (unlikely(!skb))
		return NF_ACCEPT;

	len = skb->len;
	inname = (state->in && state->in->name[0]) ? state->in->name : "-";
	outname = (state->out && state->out->name[0]) ? state->out->name : "-";

	/*
	 * 快路径：只做 per-CPU 计数。
	 * this_cpu_inc/add 不需要 preempt_disable()：x86-64 上是单条带 %gs 段的
	 * RMW 指令，且每个 CPU 只碰自己的副本，不存在跨 CPU 竞争。
	 */
	st = this_cpu_ptr(&netlab_pcpu);
	st->pkts++;
	st->bytes += len;
	if (state->hook < NF_INET_NUMHOOKS)
		st->hook_pkts[state->hook]++;

	/*
	 * skb_network_offset() 对裸 IPv4 通常为 0，但对 VLAN/QinQ 封装过的包
	 * 可能是 14/18 字节 —— 用网络偏移而不是硬编码 0 才正确。
	 */
	iph = netlab_get_iph(skb, &_iph);
	if (unlikely(!iph)) {
		/* 头不完整 / 被截断：只留一条"未知"记录，不做 L4 解析。 */
		proto = 0xff;
		frag = false;
		sport = 0;
		dport = 0;
	} else {
		proto = iph->protocol;
		/*
		 * 硬规则：先判分片，再碰 L4 头。
		 * ip_is_fragment() 检查 frag_off 里的 MF(More Fragments) 与
		 * offset 字段，是内核里判断"这是不是一个分片"的标准做法。
		 */
		frag = ip_is_fragment(iph);
		if (frag) {
			sport = 0;
			dport = 0;
		} else {
			netlab_get_ports(skb, iph, &sport, &dport);
		}
	}

	st->proto[proto]++;
	netlab_dev_account(st->in_dev, inname, len);
	netlab_dev_account(st->out_dev, outname, len);

	/*
	 * 慢路径：把这一条写进"最近记录"环形缓冲。
	 * 环形缓冲是跨 CPU 共享的，必须加锁。用 irqsave 而不是普通 spin_lock：
	 * LOCAL_OUT 的 hook 可能在进程上下文（可被软中断打断）执行，
	 * 而 LOCAL_IN 的 hook 在软中断上下文执行 —— 两者会争同一把锁，
	 * 必须屏蔽本 CPU 中断，否则会死锁。
	 *
	 * 这段临界区非常短（拷贝一条固定大小结构），而且真实系统里
	 * 明细记录通常还会再加"1/N 采样"，不会对每包都做。
	 */
	spin_lock_irqsave(&netlab_ring_lock, flags);
	ring_len = netlab_effective_ring_len();
	rec = &netlab_ring[netlab_ring_head % ring_len];
	rec->ts_ns = ktime_get_real_ns();
	rec->len = len;
	rec->hook = state->hook;
	rec->proto = proto;
	rec->frag = frag ? 1 : 0;
	rec->saddr = iph ? iph->saddr : 0;
	rec->daddr = iph ? iph->daddr : 0;
	rec->sport = sport;
	rec->dport = dport;
	strscpy(rec->indev, inname, NETLAB_DEVNAME_LEN);
	strscpy(rec->outdev, outname, NETLAB_DEVNAME_LEN);
	netlab_ring_head++;
	netlab_ring_total++;
	spin_unlock_irqrestore(&netlab_ring_lock, flags);

	/*
	 * 观测者永远放行。
	 * 想清楚再改：返回 NF_DROP 就是防火墙，返回 NF_STOLEN 就是自己接管包，
	 * 那都是"网络功能"，需要一整套配套（连接跟踪、NAT、fail-open 策略）。
	 */
	return NF_ACCEPT;
}

/*
 * hook 的排布：
 *   NF_IP_PRI_FIRST 是 INT_MIN，+1 让我们排在所有已知 hook 之前
 *   （iptables/nftables 的 NAT_DST 在 -100、NAT_SRC 在 100，conntrack 在 -200，
 *    filter 在 0）。放在最前面只读最原始的 skb，不会被前面的人改过；
 *   但反过来说，这时 DNAT 还没发生，看到的是"原始目的地址"。
 *   这是观测点选择上的经典取舍。
 */
static struct nf_hook_ops netlab_ops[] = {
	{
		.hook		= netlab_hook,
		.pf		= NFPROTO_IPV4,
		.hooknum	= NF_INET_LOCAL_IN,
		.priority	= NF_IP_PRI_FIRST + 1,
	},
	{
		.hook		= netlab_hook,
		.pf		= NFPROTO_IPV4,
		.hooknum	= NF_INET_LOCAL_OUT,
		.priority	= NF_IP_PRI_FIRST + 1,
	},
};

/* ------------------------------------------------------------------ */
/* 5. 聚合与清零                                                        */
/* ------------------------------------------------------------------ */

/* 在聚合表里按名字找槽位；找不到就占用第一个空槽（满了返回 NETLAB_NDEV）。 */
static unsigned int netlab_totals_slot(struct netlab_dev_stat *tab,
				       const char *name)
{
	unsigned int k;

	for (k = 0; k < NETLAB_NDEV; k++) {
		if (tab[k].name[0] == '\0') {
			strscpy(tab[k].name, name, NETLAB_DEVNAME_LEN);
			return k;
		}
		if (strcmp(tab[k].name, name) == 0)
			return k;
	}
	return NETLAB_NDEV;
}

static void netlab_merge_devs(struct netlab_dev_stat *dst,
			      const struct netlab_dev_stat *src)
{
	unsigned int d;

	for (d = 0; d < NETLAB_NDEV; d++) {
		unsigned int k;

		if (!src[d].name[0])
			continue;
		k = netlab_totals_slot(dst, src[d].name);
		if (k >= NETLAB_NDEV)
			break;
		dst[k].pkts  += src[d].pkts;
		dst[k].bytes += src[d].bytes;
	}
}

/*
 * 汇总所有 CPU 的 per-CPU 计数。
 * 读的时候逐 CPU 拷贝：其它 CPU 仍可能在并发地自增，所以得到的是
 * "略有过期"的快照 —— 对统计来说完全够用（/proc/net/dev 同理）。
 */
static void netlab_aggregate(struct netlab_totals *tot)
{
	struct netlab_cpu_stats *st;
	unsigned int cpu, i;

	memset(tot, 0, sizeof(*tot));

	for_each_possible_cpu(cpu) {
		st = per_cpu_ptr(&netlab_pcpu, cpu);
		tot->pkts  += READ_ONCE(st->pkts);
		tot->bytes += READ_ONCE(st->bytes);
		for (i = 0; i < NF_INET_NUMHOOKS; i++)
			tot->hook_pkts[i] += READ_ONCE(st->hook_pkts[i]);
		for (i = 0; i < NETLAB_PROTO_MAX; i++)
			if (st->proto[i])
				tot->proto[i] += READ_ONCE(st->proto[i]);

		netlab_merge_devs(tot->in_dev, st->in_dev);
		netlab_merge_devs(tot->out_dev, st->out_dev);
	}
}

/*
 * 清零：per-CPU 计数与环形缓冲。
 * 严格来说这不是原子的：清零的同时别的 CPU 可能正在自增，那一次自增会丢。
 * 这与 /proc/net/dev 的语义一致（统计是"尽力而为"的快照，不是账本），
 * 生产代码不会为了统计精确性去给整个数据面加锁。
 * 环形缓冲则是在锁内清零，保证读者看不到半截状态。
 */
static void netlab_reset(void)
{
	struct netlab_cpu_stats *st;
	unsigned int cpu;
	unsigned long flags;

	for_each_possible_cpu(cpu) {
		st = per_cpu_ptr(&netlab_pcpu, cpu);
		memset(st, 0, sizeof(*st));
	}

	spin_lock_irqsave(&netlab_ring_lock, flags);
	memset(netlab_ring, 0, sizeof(netlab_ring));
	netlab_ring_head = 0;
	netlab_ring_total = 0;
	spin_unlock_irqrestore(&netlab_ring_lock, flags);
}

/* ------------------------------------------------------------------ */
/* 6. procfs 导出：/proc/netlab_stats                                   */
/* ------------------------------------------------------------------ */

static int netlab_proc_show(struct seq_file *m, void *v)
{
	struct netlab_totals *tot;
	unsigned int ring_len, i, cnt;
	unsigned long flags;
	u64 head, total;

	(void)v;

	/*
	 * show 回调在进程上下文里被 seq_read() 调用，可以睡眠，
	 * 所以这里用 kzalloc(GFP_KERNEL) 而不是往内核栈上放 2.6KB。
	 */
	tot = kzalloc(sizeof(*tot), GFP_KERNEL);
	if (!tot)
		return -ENOMEM;
	netlab_aggregate(tot);

	seq_printf(m, "%s: enabled=%u sample_limit=%u ring_max=%u\n",
		   NETLAB_NAME, READ_ONCE(enabled) ? 1 : 0,
		   netlab_effective_ring_len(), NETLAB_RING_MAX);

	seq_printf(m, "totals: pkts=%llu bytes=%llu\n", tot->pkts, tot->bytes);

	seq_puts(m, "by_hook:");
	for (i = 0; i < NF_INET_NUMHOOKS; i++)
		seq_printf(m, " %s=%llu", netlab_hook_names[i],
			   tot->hook_pkts[i]);
	seq_puts(m, "\n");

	seq_puts(m, "by_proto:");
	for (i = 0; i < NETLAB_PROTO_MAX; i++) {
		if (!tot->proto[i])
			continue;
		if (i == 0xff)
			seq_printf(m, " UNKNOWN=%llu", tot->proto[i]);
		else
			seq_printf(m, " %s(%u)=%llu", netlab_proto_name((u8)i),
				   i, tot->proto[i]);
	}
	seq_puts(m, "\n");

	seq_puts(m, "by_indev:");
	for (i = 0; i < NETLAB_NDEV; i++)
		if (tot->in_dev[i].name[0])
			seq_printf(m, " %s=%llu/%lluB", tot->in_dev[i].name,
				   tot->in_dev[i].pkts, tot->in_dev[i].bytes);
	seq_puts(m, "\n");

	seq_puts(m, "by_outdev:");
	for (i = 0; i < NETLAB_NDEV; i++)
		if (tot->out_dev[i].name[0])
			seq_printf(m, " %s=%llu/%lluB", tot->out_dev[i].name,
				   tot->out_dev[i].pkts, tot->out_dev[i].bytes);
	seq_puts(m, "\n");

	kfree(tot);

	spin_lock_irqsave(&netlab_ring_lock, flags);
	ring_len = netlab_effective_ring_len();
	head = netlab_ring_head;
	total = netlab_ring_total;

	seq_printf(m, "ring: len=%u head=%llu total_recorded=%llu\n",
		   ring_len, head, total);
	seq_puts(m, "recent (oldest first):\n");

	cnt = (head < ring_len) ? (unsigned int)head : ring_len;
	for (i = 0; i < cnt; i++) {
		/* head 已经自增过，因此最老的一条在 (head - cnt) 处。 */
		u64 idx = head - cnt + i;
		const struct netlab_record *r = &netlab_ring[idx % ring_len];
		u64 secs = div_u64(r->ts_ns, NSEC_PER_SEC);
		u64 nsec = r->ts_ns - secs * NSEC_PER_SEC;

		seq_printf(m,
			   "  #%llu t=%llu.%09llu hook=%-12s len=%u proto=%s",
			   idx, secs, nsec,
			   r->hook < NF_INET_NUMHOOKS ?
				netlab_hook_names[r->hook] : "?",
			   r->len, netlab_proto_name(r->proto));

		if (r->frag)
			seq_puts(m, " frag=1");
		if (r->proto == IPPROTO_TCP || r->proto == IPPROTO_UDP)
			seq_printf(m, " %pI4:%u -> %pI4:%u",
				   &r->saddr, ntohs(r->sport),
				   &r->daddr, ntohs(r->dport));
		else if (r->proto != 0xff)
			seq_printf(m, " %pI4 -> %pI4", &r->saddr, &r->daddr);

		seq_printf(m, " in=%s out=%s\n", r->indev, r->outdev);
	}
	spin_unlock_irqrestore(&netlab_ring_lock, flags);

	seq_puts(m, "hint: echo reset > /proc/netlab_stats   # clear all counters\n");
	return 0;
}

static int netlab_proc_open(struct inode *inode, struct file *file)
{
	/*
	 * single_open 会分配一个 seq_file 并把它塞进 file->private_data，
	 * 之后 seq_read() 就靠它工作。在 open 里分配（而不是在 show 里）
	 * 是 seq_file 的标准用法：open 允许睡眠，恰好适合做内存分配。
	 */
	(void)inode;
	return single_open(file, netlab_proc_show, NULL);
}

static ssize_t netlab_proc_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	char buf[32];
	size_t n;

	(void)file;
	(void)ppos;

	if (count == 0)
		return 0;

	/*
	 * 必须用 copy_from_user：ubuf 是用户态地址，直接解引用会 oops。
	 * 用固定大小的栈缓冲 + 截断，避免 kmalloc。
	 */
	n = min_t(size_t, count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';

	/* strim 掉首尾空白（echo 会带一个 '\n'）；sysfs_streq 再吃掉行尾换行。 */
	strim(buf);

	if (sysfs_streq(buf, "reset")) {
		netlab_reset();
		pr_info(NETLAB_NAME ": counters reset by pid %d\n", current->pid);
		return count;
	}
	if (sysfs_streq(buf, "0") || sysfs_streq(buf, "disable")) {
		WRITE_ONCE(enabled, false);
		return count;
	}
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "enable")) {
		WRITE_ONCE(enabled, true);
		return count;
	}

	pr_info(NETLAB_NAME ": unknown command '%s' (try: reset | enable | disable)\n",
		buf);
	return -EINVAL;
}

static const struct proc_ops netlab_proc_ops = {
	.proc_open	= netlab_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= netlab_proc_write,
};

static struct proc_dir_entry *netlab_proc_entry;

/* ------------------------------------------------------------------ */
/* 7. 可选加分项：debugfs 别名节点 /sys/kernel/debug/netlab/netlab_stats */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_DEBUG_FS
static struct dentry *netlab_debugfs_dir;

/*
 * debugfs 版本只提供只读展示，复用 seq_file 的 show 函数：
 * seq_open 之后 show 的签名和 procfs 完全一致。
 */
static int netlab_dbgfs_open(struct inode *inode, struct file *file)
{
	(void)inode;
	return single_open(file, netlab_proc_show, NULL);
}

static const struct file_operations netlab_dbgfs_fops = {
	.owner		= THIS_MODULE,
	.open		= netlab_dbgfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void netlab_debugfs_init(void)
{
	/* debugfs 没挂载时这里返回 NULL/ERR_PTR，模块照常工作，不能因此失败。 */
	netlab_debugfs_dir = debugfs_create_dir(NETLAB_NAME, NULL);
	if (IS_ERR_OR_NULL(netlab_debugfs_dir)) {
		netlab_debugfs_dir = NULL;
		pr_info(NETLAB_NAME ": debugfs unavailable, /proc interface still works\n");
		return;
	}
	debugfs_create_file(NETLAB_PROC_NAME, 0444, netlab_debugfs_dir, NULL,
			    &netlab_dbgfs_fops);
}

static void netlab_debugfs_exit(void)
{
	debugfs_remove_recursive(netlab_debugfs_dir);
	netlab_debugfs_dir = NULL;
}
#else
static void netlab_debugfs_init(void) { }
static void netlab_debugfs_exit(void) { }
#endif /* CONFIG_DEBUG_FS */

/* ------------------------------------------------------------------ */
/* 8. 模块初始化 / 退出                                                 */
/* ------------------------------------------------------------------ */

static int __init netlab_init(void)
{
	int ret;

	/*
	 * 注意 netns 语义：这里只注册到 init_net。
	 * 容器（docker/podman）跑在自己的 network namespace 里，它们的
	 * net->nf.hooks_ipv4[] 是另一套数组，本模块看不到容器内的流量。
	 * 要覆盖所有 netns，需要实现 struct pernet_operations 并在
	 * .init/.exit 回调里对每个 net 调用 nf_register_net_hook(net, ops)。
	 */
	ret = nf_register_net_hooks(&init_net, netlab_ops,
				    ARRAY_SIZE(netlab_ops));
	if (ret) {
		pr_err(NETLAB_NAME ": nf_register_net_hooks failed: %d\n", ret);
		return ret;
	}

	netlab_proc_entry = proc_create(NETLAB_PROC_NAME, 0444, init_net.proc_net,
					&netlab_proc_ops);
	if (!netlab_proc_entry) {
		pr_err(NETLAB_NAME ": proc_create failed\n");
		nf_unregister_net_hooks(&init_net, netlab_ops,
					ARRAY_SIZE(netlab_ops));
		return -ENOMEM;
	}

	netlab_debugfs_init();

	pr_info(NETLAB_NAME ": loaded, %zu hooks on NFPROTO_IPV4 (prio=%d), stats at /proc/%s\n",
		ARRAY_SIZE(netlab_ops), NF_IP_PRI_FIRST + 1, NETLAB_PROC_NAME);
	return 0;
}

static void __exit netlab_exit(void)
{
	netlab_debugfs_exit();

	if (netlab_proc_entry) {
		proc_remove(netlab_proc_entry);
		netlab_proc_entry = NULL;
	}

	/*
	 * 卸载顺序很重要：先摘 procfs/debugfs，再摘 hook。
	 * 反过来也能工作，但先停止向用户态暴露正在被写入的数据更干净。
	 * nf_unregister_net_hook() 内部会 synchronize_rcu()，保证返回时
	 * 不再有任何 CPU 停留在我们的 hook 里 —— 所以之后释放全局数据是安全的。
	 */
	nf_unregister_net_hooks(&init_net, netlab_ops, ARRAY_SIZE(netlab_ops));

	pr_info(NETLAB_NAME ": unloaded\n");
}

module_init(netlab_init);
module_exit(netlab_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("learn-net labs");
MODULE_DESCRIPTION("Observational netfilter hooks on IPv4 LOCAL_IN/LOCAL_OUT (stats only, never drops)");
MODULE_VERSION("1.0");
