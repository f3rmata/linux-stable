/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_PEBS_H
#define _LINUX_HERMIT_PEBS_H

#include <linux/types.h>
#include <asm/page.h>

struct mm_struct;
struct vm_fault;
struct perf_event;
struct dentry;

/*
 * PEBS events (Intel, Intel SDM Vol.3B):
 *  0x1d3  MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM (Cascade Lake encoding)
 *  0x82d0 MEM_INST_RETIRED.ALL_STORES (DLA)
 */
#define HERMIT_PEBS_L3_MISS 0x1d3
#define HERMIT_PEBS_ALL_STORES 0x82d0

#define HERMIT_PEBS_NR_EVENTS 2
#define HERMIT_PEBS_RING_ORDER 5 /* 2^5 = 32 data pages */
#define HERMIT_PEBS_RING_PAGES (1U << HERMIT_PEBS_RING_ORDER)

/* Region accounting: one 2 MiB virtual region per entry. */
#define HERMIT_REGION_SHIFT 21
#define HERMIT_REGION_NR_SUBPAGES (1UL << (HERMIT_REGION_SHIFT - PAGE_SHIFT))
#define HERMIT_REGION_WORDS (HERMIT_REGION_NR_SUBPAGES / 64)

#define HERMIT_PEBS_MAX_ORDER 9 /* 2 MiB */

enum hermit_pebs_mode {
	HERMIT_PEBS_MODE_STATIC = 0, /* keep remote_order_mask behaviour */
	HERMIT_PEBS_MODE_POLICY = 1, /* 4 KiB prediction and reclaim policy */
};

#define HERMIT_PEBS_BUCKETS 256
#define HERMIT_PEBS_MAX_REGIONS 32768

/* Snapshot interface reserved for future cold-page/swap-out policies. */
struct hermit_pebs_observation {
	u64 start_ns, end_ns;
	u64 load[HERMIT_REGION_WORDS], store[HERMIT_REGION_WORDS];
};

/* Tunables (debugfs), defaults from Hermit measurements. */
extern u64 hermit_pebs_epoch;
extern u32 hermit_pebs_enabled; /* master switch */
extern u32 hermit_pebs_all; /* track every mm, no cgroup required */
extern u32 hermit_pebs_mode; /* enum hermit_pebs_mode */
/* Cold observation interval is at least twice this value (default 7000 ns). */
extern u32 hermit_pebs_fixed_cost_ns;
extern u32 hermit_pebs_region_max;
/* Subpage heat half-life: every this many ms the per-subpage counters halve. */
extern u32 hermit_pebs_heat_ms;

/* Attach a ring to a disabled kernel event; enable only after success. */
int hermit_perf_event_init(struct perf_event *event, unsigned int nr_pages);
int hermit_perf_event_set_period(struct perf_event *event, u64 period);
void hermit_perf_event_release(struct perf_event *event);

/* mm/hermit_pebs.c */
int hermit_pebs_start_sampling(pid_t pid, int mode);
void hermit_pebs_stop_sampling(void);
void hermit_pebs_debugfs_init(struct dentry *root);

/* Per-mm history lifetime: __mmdrop() calls this after mm_count hits 0. */
void hermit_pebs_mm_exit(struct mm_struct *mm);
void hermit_pebs_mm_enable(struct mm_struct *mm, bool enable);

/* Predict a contiguous base-page prefix; insufficient evidence returns one. */
unsigned int hermit_pebs_predict_pages(struct vm_fault *vmf);
void hermit_pebs_fault(struct mm_struct *mm, unsigned long addr,
		       unsigned long pc, u64 time);
void hermit_pebs_account_swapin(struct mm_struct *mm, unsigned int order);
bool hermit_pebs_snapshot(struct mm_struct *mm, unsigned long addr,
			  struct hermit_pebs_observation *out);
/*
 * Read the decayed per-subpage sample counts of the 2 MiB region that holds
 * @addr, for the @nr_pages subpages starting at @addr. Returns false when the
 * region has no heat record at all, which the caller must treat as unknown
 * rather than as cold. May be called from the reclaim path; takes no sleeping
 * lock and performs at most one bounded decay pass over the region.
 */
bool hermit_pebs_heat_read(struct mm_struct *mm, unsigned long addr,
			   unsigned int nr_pages, u8 *out);
bool hermit_pebs_heat_skew(struct mm_struct *mm, unsigned long addr,
			   unsigned int nr_pages, unsigned int hot_cov,
			   unsigned int *total_out, unsigned int *k_out);

#endif /* _LINUX_HERMIT_PEBS_H */
