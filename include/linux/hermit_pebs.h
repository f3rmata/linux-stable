/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_PEBS_H
#define _LINUX_HERMIT_PEBS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <asm/page.h>

struct mm_struct;
struct mem_cgroup;
struct perf_event;
struct dentry;
struct kernfs_open_file;
struct seq_file;

/*
 * PEBS events (Intel, Intel SDM Vol.3B):
 *  0x1d3  MEM_LOAD_RETIRED.L3_MISS    (PEBS-LL: DLA + latency + data source)
 *  0x82d0 MEM_INST_RETIRED.ALL_STORES (DLA)
 */
#define HERMIT_PEBS_L3_MISS	0x1d3
#define HERMIT_PEBS_ALL_STORES	0x82d0

#define HERMIT_PEBS_NR_EVENTS	2
#define HERMIT_PEBS_RING_ORDER	5		/* 2^5 = 32 data pages */
#define HERMIT_PEBS_RING_PAGES	(1U << HERMIT_PEBS_RING_ORDER)

/* Region accounting: one 2 MiB virtual region per entry. */
#define HERMIT_REGION_SHIFT		21
#define HERMIT_REGION_NR_SUBPAGES	(1UL << (HERMIT_REGION_SHIFT - PAGE_SHIFT))
#define HERMIT_REGION_WORDS		(HERMIT_REGION_NR_SUBPAGES / 64)

#define HERMIT_PEBS_MAX_ORDER	9	/* 2 MiB */

enum hermit_pebs_mode {
	HERMIT_PEBS_MODE_STATIC = 0,	/* keep remote_order_mask behaviour */
	HERMIT_PEBS_MODE_POLICY = 1,	/* per-folio transfer order policy */
};

/* Two cooling-clock periods without a touch: the region bitmap is reset. */
#define HERMIT_PEBS_STALE_CLOCKS 2

struct hermit_region {
	struct hlist_node hnode;
	struct list_head lru;
	struct mm_struct *mm;
	unsigned long vaddr;		/* region aligned */
	u32 cooling_clock;		/* memcg cooling_clock at last touch */
	u64 touched[HERMIT_REGION_WORDS];
};

#define HERMIT_PEBS_BUCKETS	256
#define HERMIT_PEBS_MAX_REGIONS	32768

struct hermit_pebs_state {
	spinlock_t lock;
	struct hlist_head buckets[HERMIT_PEBS_BUCKETS];
	struct list_head lru;
	unsigned int nr_regions;
	u32 cooling_clock;
	unsigned long nr_samples;
};

/* Tunables (debugfs), defaults from Hermit measurements. */
extern u32 hermit_pebs_enabled;		/* master switch */
extern u32 hermit_pebs_mode;		/* enum hermit_pebs_mode */
extern u32 hermit_pebs_force_order;	/* 0 = off, else forced transfer order */
extern u32 hermit_pebs_fixed_cost_ns;	/* per-WR fixed cost, default 7000 */
extern u32 hermit_pebs_bw_mibps;	/* link bandwidth MiB/s, default 12288 */
extern u32 hermit_pebs_min_f;		/* min f in permille, default 50 */
extern u32 hermit_pebs_hysteresis;	/* permille, default 900 */
extern u32 hermit_pebs_cooling_period;	/* samples per cooling tick */
extern u32 hermit_pebs_region_max;

/* kernel/events/core.c: attach a ring buffer to a kernel event. */
int hermit_perf_event_init(struct perf_event *event, unsigned int nr_pages);
int hermit_perf_event_set_period(struct perf_event *event, u64 period);
void hermit_perf_event_release(struct perf_event *event);

/* mm/hermit_pebs.c */
int hermit_pebs_start_sampling(pid_t pid, int mode);
void hermit_pebs_stop_sampling(void);
void hermit_pebs_debugfs_init(struct dentry *root);
void hermit_pebs_memcg_init(struct mem_cgroup *memcg);
void hermit_pebs_memcg_exit(struct mem_cgroup *memcg);
ssize_t hermit_pebs_memcg_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off);
int hermit_pebs_memcg_show(struct seq_file *m, void *v);

/*
 * Pick the transfer order for a folio covering
 * [addr, addr + 2^folio_order * PAGE_SIZE).
 * Returns the order (0..folio_order) or -1 when no sampled data is
 * available / policy disabled; the caller then falls back to the
 * static remote_order_mask behaviour.
 */
int hermit_order_policy(struct mem_cgroup *memcg, struct mm_struct *mm,
			unsigned long addr, unsigned int folio_order,
			unsigned int cur_order, unsigned long allowed_mask);

#endif /* _LINUX_HERMIT_PEBS_H */
