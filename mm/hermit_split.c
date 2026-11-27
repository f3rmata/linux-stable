// SPDX-License-Identifier: GPL-2.0
/*
 * Hermit PEBS-guided split of resident large anonymous folios.
 *
 * Memcached and similar caches keep a large share of their resident bytes in
 * mTHP folios whose cold subranges nobody touches. The PEBS sampler already
 * counts every L3-miss load and every store per 2 MiB virtual region
 * (mm/hermit_pebs.c). This file turns those per-subpage counts into a reclaim
 * decision: when a large anonymous folio that MGLRU selected for eviction has
 * an aligned hot subrange and a cold aligned remainder, split it down to the
 * order of that hot subrange so reclaim can drop the cold parts at a small
 * granularity instead of writing out the whole extent or keeping it whole.
 *
 * The decision is conservative and bounded:
 *  - no PEBS samples means unknown, never cold;
 *  - the folio must be fully mapped in exactly one mm, without MMU notifiers,
 *    and the cold subpages must have their accessed bit clear;
 *  - at most hermit_split_rate folios per second are split;
 *  - splitting only happens while the memcg is near its limit or while its
 *    refault rate is low enough that the hot subrange is not thrashing.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/huge_mm.h>
#include <linux/pgtable.h>
#include <linux/slab.h>
#include <linux/bitmap.h>
#include <linux/jiffies.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/memcontrol.h>
#include <linux/page_counter.h>
#include <linux/mmu_notifier.h>
#include <linux/hermit_pebs.h>
#include <linux/hermit_policy.h>
#include <linux/hermit_split.h>

/* debugfs tunables; the split only runs when HERMIT_POLICY_SPLIT is set. */
static u32 hermit_split_rate = 32; /* folios per second */
static u32 hermit_split_hot_cov = 80; /* % of samples the hot set covers */
static u32 hermit_split_hot_pct =
	75; /* % of a block's pages that must be hot */
static u32 hermit_split_min_cold = 25; /* % of the folio that must be cold */
static u32 hermit_split_min_samples = 4; /* samples needed in the folio */
static u32 hermit_split_refault_max = 256; /* max refault score for a split */

#define HERMIT_SPLIT_MAX_ORDER 9 /* 2 MiB, the PEBS region size */
#define HERMIT_SPLIT_MAX_SUBPAGES (1U << HERMIT_SPLIT_MAX_ORDER)

/* Skip reasons: one counter each, printed as split_skipped_reason_<name>. */
enum hermit_split_skip {
	HSP_SHARED,
	HSP_KSM,
	HSP_WRITEBACK,
	HSP_SWAPCACHE,
	HSP_PINNED,
	HSP_NOTIFIER,
	HSP_UNKNOWN,
	HSP_ALL_HOT,
	HSP_COLD_YOUNG,
	HSP_NO_ORDER,
	HSP_BUDGET,
	HSP_PRESSURE,
	HSP_MEMORY,
	HSP_INACTIVE,
	HSP_NOHEAT,
	HSP_FEWSAMPLES,
	HSP_CONTENDED,
	HSP_NR,
};
static atomic_long_t split_candidates, split_attempts, split_pages;
static atomic_long_t split_failures;
static atomic_long_t split_done[HERMIT_SPLIT_MAX_ORDER + 1];
static atomic_long_t split_skipped[HSP_NR];
static const char *const split_skip_names[HSP_NR] = {
	"shared",     "ksm",	   "writeback", "swapcache",  "pinned",
	"notifier",   "unknown",   "all_hot",	"cold_young", "no_order",
	"budget",     "pressure",  "memory",	"inactive",   "noheat",
	"fewsamples", "contended",
};

/* hermit_split:begin pure */

/*
 * Selection of the split order from a decayed subpage histogram.
 *
 * The hot set holds the subpages that cover @rule->hot_cov percent of the
 * folio's samples; it is the tail of the histogram, so its size is the number
 * of subpages whose count reaches the smallest threshold that still covers
 * that share. The split order is the largest aligned sub-order of the folio
 * with at least one block whose pages are @rule->hot_pct percent hot and at
 * least one block that is not, i.e. the largest block that still isolates the
 * hot subrange from a cold remainder. A cold subpage whose PTE is still
 * accessed vetoes the split.
 */
enum hermit_split_verdict {
	HERMIT_SPLIT_PICK = 0, /* @out_order holds the split order */
	HERMIT_SPLIT_UNKNOWN,
	HERMIT_SPLIT_FEWSAMPLES,
	HERMIT_SPLIT_ALL_HOT, /* no cold remainder worth splitting */
	HERMIT_SPLIT_COLD_YOUNG, /* a cold subpage is still accessed */
	HERMIT_SPLIT_NO_ORDER, /* no aligned block isolates the hot part */
};

struct hermit_split_rule {
	u32 hot_cov;
	u32 hot_pct;
	u32 min_cold;
	u32 min_samples;
};

static enum hermit_split_verdict
hermit_split_select(const u8 *heat, unsigned int nr_pages,
		    unsigned int folio_order, const unsigned long *young,
		    const struct hermit_split_rule *rule,
		    unsigned int *out_order)
{
	unsigned int hist[U8_MAX + 1] = {};
	unsigned int i, j, block, blocks, order, size;
	unsigned int total = 0, covered = 0, hot = 0, cold, threshold = 0;

	if (!nr_pages || nr_pages > (1U << folio_order) || folio_order < 2)
		return HERMIT_SPLIT_NO_ORDER;
	for (i = 0; i < nr_pages; i++) {
		hist[heat[i]]++;
		total += heat[i];
	}
	if (total < rule->min_samples)
		return HERMIT_SPLIT_FEWSAMPLES;
	/* Smallest count whose tail still covers hot_cov% of the samples. */
	for (i = U8_MAX; i > 0; i--) {
		covered += hist[i] * i;
		if (covered * 100 >= total * rule->hot_cov) {
			threshold = i;
			break;
		}
	}
	if (!threshold)
		return HERMIT_SPLIT_UNKNOWN;
	for (i = 0; i < nr_pages; i++)
		if (heat[i] >= threshold)
			hot++;
	cold = nr_pages - hot;
	if (!cold || cold * 100 < rule->min_cold * nr_pages)
		return HERMIT_SPLIT_ALL_HOT;
	for (i = 0; i < nr_pages; i++)
		if (heat[i] < threshold && test_bit(i, young))
			return HERMIT_SPLIT_COLD_YOUNG;
	/* Order 1 anonymous folios are not supported by the split code. */
	for (order = folio_order; order-- > 0;) {
		bool any_hot = false, any_cold = false;

		if (order == 1)
			continue;
		size = 1U << order;
		blocks = nr_pages / size;
		for (block = 0; block < blocks; block++) {
			unsigned int in = 0;

			for (j = 0; j < size; j++)
				if (heat[block * size + j] >= threshold)
					in++;
			if (in * 100 >= rule->hot_pct * size)
				any_hot = true;
			else
				any_cold = true;
		}
		if (any_hot && any_cold) {
			*out_order = order;
			return HERMIT_SPLIT_PICK;
		}
	}
	return HERMIT_SPLIT_NO_ORDER;
}
/* hermit_split:end pure */

static void hermit_split_skip(enum hermit_split_skip reason)
{
	atomic_long_inc(&split_skipped[reason]);
}

/* hermit_split:budget:begin */
/* At most hermit_split_rate folios per second, counted in 1 s windows. */
static atomic_t split_window_count;
static unsigned long split_window_jiffies;

static bool hermit_split_budget(void)
{
	unsigned int rate = READ_ONCE(hermit_split_rate);
	unsigned long old;

	if (!rate)
		return false;
	old = READ_ONCE(split_window_jiffies);
	if (time_after(jiffies, old + HZ) &&
	    cmpxchg(&split_window_jiffies, old, jiffies) == old)
		atomic_set(&split_window_count, 0);
	return (unsigned int)atomic_inc_return(&split_window_count) <= rate;
}
/* hermit_split:budget:end */

/* hermit_split:pressure:begin */
/*
 * A split is only useful, and cheap enough, when the memcg is close to its
 * limit (memory is needed now) or when its pages are not refaulting (the hot
 * subrange is not thrashing). The refault score is the per-memcg value the
 * swap-in policy already maintains.
 */
static bool hermit_split_under_pressure(struct mem_cgroup *memcg)
{
#ifdef CONFIG_MEMCG
	unsigned long max, usage;

	if (memcg) {
		max = READ_ONCE(memcg->memory.max);
		if (max != PAGE_COUNTER_MAX) {
			usage = page_counter_read(&memcg->memory);
			if (usage >= max - max / 4)
				return true;
		}
		return (unsigned int)atomic_read(&memcg->hermit_refault) <
		       READ_ONCE(hermit_split_refault_max);
	}
#endif
	return true;
}
/* hermit_split:pressure:end */

struct hermit_split_probe {
	struct mm_struct *mm;
	unsigned long addr;
	unsigned int nr_pages;
	unsigned int mappings;
	enum hermit_split_skip reason;
	bool failed;
	DECLARE_BITMAP(young, HERMIT_SPLIT_MAX_SUBPAGES);
	u8 *heat;
};

static bool hermit_split_probe_one(struct folio *folio,
				   struct vm_area_struct *vma,
				   unsigned long address, void *arg)
{
	struct hermit_split_probe *probe = arg;
	struct mm_struct *mm = vma->vm_mm;

	probe->mappings++;
	if (probe->mappings > 1) {
		probe->reason = HSP_SHARED;
		probe->failed = true;
		return false;
	}
	if (!mm || !hermit_policy_active(mm)) {
		probe->reason = HSP_INACTIVE;
		probe->failed = true;
		return false;
	}
	if (mm_has_notifiers(mm)) {
		probe->reason = HSP_NOTIFIER;
		probe->failed = true;
		return false;
	}
	probe->mm = mm;
	probe->addr = address & PAGE_MASK;
	if (!hermit_pebs_heat_read(mm, probe->addr, probe->nr_pages,
				   probe->heat)) {
		probe->reason = HSP_NOHEAT;
		probe->failed = true;
		return false;
	}
	{
		DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, probe->addr, 0);

		while (page_vma_mapped_walk(&pvmw)) {
			if (pvmw.pte) {
				unsigned long offset;
				pte_t pte = ptep_get(pvmw.pte);

				if (!pte_present(pte) || !pte_young(pte))
					continue;
				offset = (pvmw.address - probe->addr) >>
					 PAGE_SHIFT;
				if (offset < probe->nr_pages)
					__set_bit(offset, probe->young);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			} else if (pmd_young(pmdp_get(pvmw.pmd))) {
				/* One bit for the whole PMD mapping. */
				bitmap_fill(probe->young, probe->nr_pages);
#endif
			}
		}
	}
	return true;
}

bool hermit_split_try(struct folio *folio, struct list_head *list,
		      struct mem_cgroup *memcg)
{
	unsigned int order = folio_order(folio),
		     nr_pages = folio_nr_pages(folio);
	unsigned int target = 0;
	struct hermit_split_rule rule = {
		.hot_cov = READ_ONCE(hermit_split_hot_cov),
		.hot_pct = READ_ONCE(hermit_split_hot_pct),
		.min_cold = READ_ONCE(hermit_split_min_cold),
		.min_samples = READ_ONCE(hermit_split_min_samples),
	};
	struct hermit_split_probe probe;
	struct rmap_walk_control rwc = {
		.rmap_one = hermit_split_probe_one,
		.arg = &probe,
		.try_lock = true,
	};
	enum hermit_split_verdict verdict;
	int ret;

	if (!hermit_policy_feature(HERMIT_POLICY_SPLIT))
		return false;
	/* Only a large, swap-backed anon folio inside one PEBS region can be a
	 * candidate. Lazyfree folios are freed without IO and keep their order.
	 */
	if (!folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
	    !folio_test_large(folio) || order < 2 ||
	    order > HERMIT_SPLIT_MAX_ORDER)
		return false;
	if (folio_test_ksm(folio)) {
		hermit_split_skip(HSP_KSM);
		return false;
	}
	if (folio_test_writeback(folio)) {
		hermit_split_skip(HSP_WRITEBACK);
		return false;
	}
	if (folio_test_swapcache(folio)) {
		hermit_split_skip(HSP_SWAPCACHE);
		return false;
	}
	/* Fully mapped in one mm: mapcount == nr_pages and a single rmap hit. */
	if (folio_mapcount(folio) != nr_pages) {
		hermit_split_skip(HSP_SHARED);
		return false;
	}
	if (folio_maybe_dma_pinned(folio) || !can_split_folio(folio, 1, NULL)) {
		hermit_split_skip(HSP_PINNED);
		return false;
	}
	if (!hermit_split_budget()) {
		hermit_split_skip(HSP_BUDGET);
		return false;
	}
	if (!hermit_split_under_pressure(memcg)) {
		hermit_split_skip(HSP_PRESSURE);
		return false;
	}
	atomic_long_inc(&split_candidates);

	memset(&probe, 0, sizeof(probe));
	probe.nr_pages = nr_pages;
	probe.reason = HSP_UNKNOWN;
	probe.heat = kmalloc(nr_pages, GFP_NOWAIT | __GFP_NOWARN);
	if (!probe.heat) {
		hermit_split_skip(HSP_MEMORY);
		return false;
	}
	rmap_walk(folio, &rwc);
	if (probe.failed || rwc.contended || probe.mappings != 1) {
		hermit_split_skip(probe.failed	? probe.reason :
				  rwc.contended ? HSP_CONTENDED :
						  HSP_SHARED);
		kfree(probe.heat);
		return false;
	}
	verdict = hermit_split_select(probe.heat, nr_pages, order, probe.young,
				      &rule, &target);
	kfree(probe.heat);
	switch (verdict) {
	case HERMIT_SPLIT_FEWSAMPLES:
		hermit_split_skip(HSP_FEWSAMPLES);
		return false;
	case HERMIT_SPLIT_UNKNOWN:
		hermit_split_skip(HSP_UNKNOWN);
		return false;
	case HERMIT_SPLIT_ALL_HOT:
		hermit_split_skip(HSP_ALL_HOT);
		return false;
	case HERMIT_SPLIT_COLD_YOUNG:
		hermit_split_skip(HSP_COLD_YOUNG);
		return false;
	case HERMIT_SPLIT_NO_ORDER:
		hermit_split_skip(HSP_NO_ORDER);
		return false;
	case HERMIT_SPLIT_PICK:
		break;
	}

	atomic_long_inc(&split_attempts);
	ret = split_folio_to_list_to_order(folio, list, target);
	if (ret) {
		atomic_long_inc(&split_failures);
		return true;
	}
	atomic_long_inc(&split_done[target]);
	atomic_long_add(nr_pages, &split_pages);
	return true;
}

static void hermit_split_show_skips(struct seq_file *m, unsigned int first,
				    unsigned int end)
{
	unsigned int i;

	for (i = first; i < end; i++)
		seq_printf(m, "split_skipped_reason_%s %ld\n",
			   split_skip_names[i],
			   atomic_long_read(&split_skipped[i]));
}

void hermit_split_extra_stats_show(struct seq_file *m)
{
	hermit_split_show_skips(m, HSP_INACTIVE, HSP_NR);
}

void hermit_split_stats_show(struct seq_file *m)
{
	unsigned int i;

	seq_printf(m, "split_candidates %ld\n",
		   atomic_long_read(&split_candidates));
	seq_printf(m, "split_attempts %ld\n",
		   atomic_long_read(&split_attempts));
	seq_printf(m, "split_pages %ld\n", atomic_long_read(&split_pages));
	seq_printf(m, "split_failures %ld\n",
		   atomic_long_read(&split_failures));
	for (i = 0; i <= HERMIT_SPLIT_MAX_ORDER; i++)
		seq_printf(m, "split_done_order%u %ld\n", i,
			   atomic_long_read(&split_done[i]));
	hermit_split_show_skips(m, 0, HSP_INACTIVE);
}

/*
 * HERMIT_POLICY_SPLIT_OBS: observe-only skew telemetry on resident and
 * reclaim-scanned large folios without performing any split.
 */
struct hermit_obs_bucket {
	atomic_long_t total;
	atomic_long_t noheat;
	atomic_long_t fewsamples;
	atomic_long_t skew_lt12; /* k80 / nr_pages < 12.5% */
	atomic_long_t skew_12_25; /* 12.5% <= k80 / nr_pages < 25% */
	atomic_long_t skew_25_50; /* 25% <= k80 / nr_pages < 50% */
	atomic_long_t skew_50_75; /* 50% <= k80 / nr_pages < 75% */
	atomic_long_t skew_ge75; /* k80 / nr_pages >= 75% */
	atomic_long_t orders[HERMIT_SPLIT_MAX_ORDER + 1];
};

static struct hermit_obs_bucket split_obs[HERMIT_OBS_STAGES];
static const char *const split_obs_names[HERMIT_OBS_STAGES] = {
	"aged",
	"kept",
	"reclaim",
};

void hermit_split_observe_addr(struct mm_struct *mm, unsigned long addr,
			       unsigned int order,
			       enum hermit_split_obs_stage stage)
{
	unsigned int nr_pages, total = 0, k = 0;
	unsigned int min_samples, hot_cov;
	struct hermit_obs_bucket *b;

	if (!hermit_policy_feature(HERMIT_POLICY_SPLIT_OBS))
		return;
	if (!mm || stage >= HERMIT_OBS_STAGES || order < 2 ||
	    order > HERMIT_SPLIT_MAX_ORDER || !hermit_policy_active(mm))
		return;

	nr_pages = 1U << order;
	addr = ALIGN_DOWN(addr, (unsigned long)nr_pages * PAGE_SIZE);
	b = &split_obs[stage];
	atomic_long_inc(&b->total);
	atomic_long_inc(&b->orders[order]);

	hot_cov = READ_ONCE(hermit_split_hot_cov);
	min_samples = READ_ONCE(hermit_split_min_samples);
	if (!hermit_pebs_heat_skew(mm, addr, nr_pages, hot_cov, &total, &k)) {
		atomic_long_inc(&b->noheat);
		return;
	}
	if (total < min_samples) {
		atomic_long_inc(&b->fewsamples);
		return;
	}
	if (k * 8 < nr_pages)
		atomic_long_inc(&b->skew_lt12);
	else if (k * 4 < nr_pages)
		atomic_long_inc(&b->skew_12_25);
	else if (k * 2 < nr_pages)
		atomic_long_inc(&b->skew_25_50);
	else if (k * 4 < nr_pages * 3)
		atomic_long_inc(&b->skew_50_75);
	else
		atomic_long_inc(&b->skew_ge75);
}

struct hermit_obs_rmap {
	struct mm_struct *mm;
	unsigned long addr;
	unsigned int mappings;
};

static bool hermit_obs_rmap_one(struct folio *folio, struct vm_area_struct *vma,
				unsigned long address, void *arg)
{
	struct hermit_obs_rmap *r = arg;

	r->mappings++;
	if (r->mappings > 1)
		return false;
	r->mm = vma->vm_mm;
	r->addr = address & PAGE_MASK;
	return true;
}

void hermit_split_observe_folio(struct folio *folio,
				enum hermit_split_obs_stage stage)
{
	unsigned int order;
	struct hermit_obs_rmap obs = {};
	struct rmap_walk_control rwc = {
		.rmap_one = hermit_obs_rmap_one,
		.arg = &obs,
		.try_lock = true,
	};

	if (!hermit_policy_feature(HERMIT_POLICY_SPLIT_OBS))
		return;
	if (!folio_test_large(folio) || !folio_test_anon(folio) ||
	    folio_test_ksm(folio))
		return;
	order = folio_order(folio);
	if (order < 2 || order > HERMIT_SPLIT_MAX_ORDER)
		return;
	rmap_walk(folio, &rwc);
	if (obs.mappings == 1 && obs.mm)
		hermit_split_observe_addr(obs.mm, obs.addr, order, stage);
}

static int hermit_split_obs_show(struct seq_file *m, void *unused)
{
	unsigned int s, o;

	for (s = 0; s < HERMIT_OBS_STAGES; s++) {
		struct hermit_obs_bucket *b = &split_obs[s];
		const char *n = split_obs_names[s];

		seq_printf(m, "%s_total %ld\n", n, atomic_long_read(&b->total));
		seq_printf(m, "%s_noheat %ld\n", n,
			   atomic_long_read(&b->noheat));
		seq_printf(m, "%s_fewsamples %ld\n", n,
			   atomic_long_read(&b->fewsamples));
		seq_printf(m, "%s_skew_lt12 %ld\n", n,
			   atomic_long_read(&b->skew_lt12));
		seq_printf(m, "%s_skew_12_25 %ld\n", n,
			   atomic_long_read(&b->skew_12_25));
		seq_printf(m, "%s_skew_25_50 %ld\n", n,
			   atomic_long_read(&b->skew_25_50));
		seq_printf(m, "%s_skew_50_75 %ld\n", n,
			   atomic_long_read(&b->skew_50_75));
		seq_printf(m, "%s_skew_ge75 %ld\n", n,
			   atomic_long_read(&b->skew_ge75));
		for (o = 2; o <= HERMIT_SPLIT_MAX_ORDER; o++)
			seq_printf(m, "%s_order%u %ld\n", n, o,
				   atomic_long_read(&b->orders[o]));
	}
	return 0;
}

static int hermit_split_obs_open(struct inode *inode, struct file *file)
{
	return single_open(file, hermit_split_obs_show, NULL);
}

static const struct file_operations hermit_split_obs_fops = {
	.owner = THIS_MODULE,
	.open = hermit_split_obs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void hermit_split_debugfs_init(struct dentry *root)
{
	debugfs_create_u32("policy_split_rate", S_IRUSR | S_IWUSR, root,
			   &hermit_split_rate);
	debugfs_create_u32("policy_split_hot_cov", S_IRUSR | S_IWUSR, root,
			   &hermit_split_hot_cov);
	debugfs_create_u32("policy_split_hot_pct", S_IRUSR | S_IWUSR, root,
			   &hermit_split_hot_pct);
	debugfs_create_u32("policy_split_min_cold", S_IRUSR | S_IWUSR, root,
			   &hermit_split_min_cold);
	debugfs_create_u32("policy_split_min_samples", S_IRUSR | S_IWUSR, root,
			   &hermit_split_min_samples);
	debugfs_create_u32("policy_split_refault_max", S_IRUSR | S_IWUSR, root,
			   &hermit_split_refault_max);
	debugfs_create_file("split_obs_stats", S_IRUSR, root, NULL,
			    &hermit_split_obs_fops);
}
