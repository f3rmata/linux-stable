/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_POLICY_H
#define _LINUX_HERMIT_POLICY_H
#include <linux/bits.h>
#include <linux/types.h>

struct mm_struct;
struct vm_area_struct;
struct vm_fault;
struct folio;
struct list_head;
struct dentry;
struct mem_cgroup;

struct hermit_reclaim_hint {
	u64 owner, generation;
	unsigned long start, end, address;
	u64 age;
	unsigned int heat;
	bool cold;
	bool mglru;	/* selected by MGLRU, not a negative PTE observation */
};

/* debugfs policy_features: independent ablation switches for pebs_mode=1. */
#define HERMIT_POLICY_SWAPIN	BIT(0)	/* 4 KiB predicted SG swap-in */
#define HERMIT_POLICY_PEBS	BIT(1)	/* PEBS footprint votes extend a batch */
#define HERMIT_POLICY_SORT	BIT(2)	/* reclaim candidate ranking */
#define HERMIT_POLICY_OBSERVE	BIT(3)	/* young-bit cold observation */
#define HERMIT_POLICY_FLUSH	BIT(4)	/* TLB flush at observation start */
#define HERMIT_POLICY_BATCH	BIT(5)	/* SG writeout of cold dirty pages */
#define HERMIT_POLICY_RESERVE	BIT(6)	/* replace apt_reclaim headroom/workers */
#define HERMIT_POLICY_LARGE	BIT(7)	/* long sequential streams use mTHP path */
#define HERMIT_POLICY_MGLRU	BIT(8)	/* reuse MGLRU selection for SG writeout */
#define HERMIT_POLICY_MEDIUM	BIT(9)	/* fit-driven 16-64 KiB swap-in orders */
#define HERMIT_POLICY_DEFAULT	(HERMIT_POLICY_SWAPIN | HERMIT_POLICY_PEBS | \
				 HERMIT_POLICY_SORT | HERMIT_POLICY_OBSERVE | \
				 HERMIT_POLICY_FLUSH | HERMIT_POLICY_BATCH | \
				 HERMIT_POLICY_LARGE | HERMIT_POLICY_MGLRU)

#ifdef CONFIG_HERMIT
bool hermit_policy_feature(unsigned int feature);
bool hermit_policy_reserve_mode(void);
/*
 * Order suggestion for the Hermit swap-in path: returns the highest folio
 * order the caller may allocate, or -1 to keep the 4 KiB SG batches. @orders
 * is the caller's legal order mask; it is only read. @medium reports a
 * fit-driven HERMIT_POLICY_MEDIUM suggestion (a LARGE one is not counted).
 */
int hermit_policy_swapin_order(struct vm_fault *vmf, unsigned long orders,
		bool *medium);
void hermit_policy_medium_used(unsigned int order);
void hermit_policy_medium_fallback(bool alloc);
void hermit_policy_unmap(struct vm_area_struct *vma, unsigned long start,
		unsigned long end);
bool hermit_policy_active(struct mm_struct *mm);
void hermit_policy_mm_exit(struct mm_struct *mm);
void hermit_policy_sample(struct mm_struct *mm, unsigned long addr, u64 time);
void hermit_policy_observe(struct folio *folio, struct vm_area_struct *vma,
		unsigned long addr, bool young, bool valid);
void hermit_policy_installed(struct vm_fault *vmf, unsigned long pfn,
		unsigned long start, unsigned int nr);
unsigned int hermit_policy_predict(struct vm_fault *vmf);
/*
 * Cost instrumentation (debugfs swapin_cost_enable) only. @entry_val is a
 * swp_entry_t.val. Prefetched pages are remembered for one POLICY_TTL so a
 * later demand fault that finds the page in swapcache can be counted as a
 * prefetch hit instead of a backend read. Both calls are cheap no-ops when the
 * caller has already tested hermit_swapin_cost_enabled().
 */
void hermit_policy_prefetched(struct mm_struct *mm, unsigned long entry_val);
bool hermit_policy_prefetch_hit(struct mm_struct *mm, unsigned long entry_val);
void hermit_policy_hint(struct folio *folio, struct hermit_reclaim_hint *hint);
void hermit_policy_defer_flush(struct vm_area_struct *vma, unsigned long addr);
void hermit_policy_eviction(struct folio *folio, struct hermit_reclaim_hint *hint,
		bool lru_gen_reclaim);
void hermit_policy_sort(struct list_head *list, bool lru_gen_reclaim);
void hermit_policy_lru_changed(void);
unsigned long hermit_policy_reserve(struct mem_cgroup *memcg);
void hermit_policy_debugfs_init(struct dentry *root);
#else
static inline void hermit_policy_observe(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr, bool young, bool valid) {}
static inline void hermit_policy_unmap(struct vm_area_struct *vma,
		unsigned long start, unsigned long end) {}
#endif
#endif
