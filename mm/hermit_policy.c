// SPDX-License-Identifier: GPL-2.0
/* MGLRU swap batching and stream prediction. No speculative capacity is
 * hidden from memcg: the swapcache owns every prefetched 4 KiB charge.
 */
#include <linux/debugfs.h>
#include <linux/hash.h>
#include <linux/hermit_backend.h>
#include <linux/hermit_pebs.h>
#include <linux/hermit_policy.h>
#include <linux/hermit_split.h>
#include <linux/ktime.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <asm/tlbflush.h>
#include <linux/rmap.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/swapops.h>

/* Stream state is set-associative: a key hashes to one set and may occupy any
 * of its ways. With one slot per hash, two threads that collided reset each
 * other's run on every fault and never reached the large-folio threshold.
 */
#define POLICY_STREAM_SET_BITS 3
#define POLICY_STREAM_WAYS 8
#define POLICY_STREAMS ((1U << POLICY_STREAM_SET_BITS) * POLICY_STREAM_WAYS)
#define POLICY_TTL NSEC_PER_SEC
/* Cost instrumentation only: direct-mapped slots for the swap entries that
 * hermit_swapin_pages() read speculatively. 256 slots cover several in-flight
 * batches; a collision only loses one hit count, never a policy decision.
 */
#define POLICY_PREFETCH_BITS 8
#define POLICY_PREFETCH_SLOTS (1U << POLICY_PREFETCH_BITS)

/* debugfs tunables; see HERMIT_POLICY_* for the ablation bits. */
static u32 policy_features = HERMIT_POLICY_DEFAULT;
static u32 policy_swapin_max = HERMIT_SWAPIN_MAX;
/* Phase B: one medium folio instead of a 4 KiB batch. The default ceiling is
 * 64 KiB because upstream swpout fallback rates rise steeply above it, and a
 * segment length must repeat before it is trusted.
 */
static u32 policy_medium_max_order = 4;
static u32 policy_medium_min_order = 4;
static u32 policy_medium_stable = 3;
/* Phase C: the same completed-segment signal gates the fit-sized first batch
 * and the PTE install of the pages a batch read ahead.
 */
static u32 policy_fitstart_stable = 3;

bool hermit_policy_feature(unsigned int feature)
{
	return READ_ONCE(policy_features) & feature;
}

static bool policy_mode(void)
{
	return READ_ONCE(hermit_pebs_enabled) &&
	       READ_ONCE(hermit_pebs_mode) == HERMIT_PEBS_MODE_POLICY;
}

bool hermit_policy_reserve_mode(void)
{
	return policy_mode() && hermit_policy_feature(HERMIT_POLICY_RESERVE);
}

struct policy_stream {
	u64 task_start, generation, epoch, time;
	pid_t tid;
	unsigned long pc, start, end, last;
	u16 sequential; /* consecutive base pages, demand + prefetch */
	/* The last predicted batch [batch_start, batch_end) as actually read.
	 * Later faults on its prefetched pages advance s->last through it, so a
	 * run that breaks before batch_end left part of it unused.
	 */
	unsigned long batch_start, batch_end;
	u16 fit; /* length of the last predicted run, 0 = none yet */
	/* Medium-order confidence: how many completed segments in a row ended at
	 * the same rounded-down power-of-two length. Both fields live in the
	 * padding the struct already had, so no stream slot grows.
	 */
	u8 fit_order; /* ilog2(fit) */
	u8 fit_stable; /* consecutive completed segments of fit_order */
};
struct hermit_policy_state {
	spinlock_t lock;
	struct policy_stream streams[POLICY_STREAMS];
	unsigned int refault;
	u64 id;
	/* Swap entries read speculatively by hermit_swapin_pages(), used only by
	 * the swap-in cost counters (debugfs swapin_cost_enable).
	 */
	struct {
		unsigned long entry; /* swp_entry_t.val */
		u64 time; /* 0 = unused */
	} prefetch[POLICY_PREFETCH_SLOTS];
};
static atomic64_t policy_ids;
static atomic_long_t predicted_pages, refault_pages, large_streams;
/* A live stream (used within POLICY_TTL) replaced by a different key. */
static atomic_long_t stream_evictions;
/* Runs that ended inside a predicted batch; capped_batches = predictions cut by that cap. */
static atomic_long_t batch_overruns, capped_batches;
static atomic_long_t mglru_candidates;
/* Fit-driven medium orders: one suggestion per fault, then exactly one of
 * medium_used[order], medium_fallback_alloc or medium_fallback_extent.
 */
static atomic_long_t medium_suggested, medium_fallback_alloc;
static atomic_long_t medium_below_min;
static atomic_long_t slotrun_groups, slotrun_contiguous, slotrun_fallback;
static atomic_long_t medium_fallback_extent;
static atomic_long_t medium_extent_reasons[SWAPIN_EXTENT_NO_ORDER];
static atomic_long_t medium_used[PMD_ORDER + 1];
/* FITSTART / PTEBATCH: batches whose size came from a stable fit, the pages
 * they cover, and the PTE install result of an SG batch.
 */
static atomic_long_t fitstart_batches, fitstart_pages;
static atomic_long_t ptebatch_installed, ptebatch_skipped;
static atomic_long_t ptebatch_gate_rejects;

bool hermit_policy_active(struct mm_struct *mm)
{
	return policy_mode() && (READ_ONCE(hermit_pebs_all) ||
				 READ_ONCE(mm->hermit_pebs_enabled));
}

static struct hermit_policy_state *policy_state(struct mm_struct *mm)
{
	struct hermit_policy_state *st = READ_ONCE(mm->hermit_policy), *new;

	if (st || !hermit_policy_active(mm))
		return st;
	new = kzalloc(sizeof(*new), GFP_NOWAIT | __GFP_NOWARN);
	if (!new)
		return NULL;
	spin_lock_init(&new->lock);
	new->id = atomic64_inc_return(&policy_ids);
	st = cmpxchg(&mm->hermit_policy, NULL, new);
	if (st)
		kfree(new);
	return st ?: new;
}

static bool policy_stream_match(const struct policy_stream *s,
				struct vm_fault *vmf, u64 gen, u64 epoch)
{
	return s->time && s->tid == task_pid_nr(current) &&
	       s->task_start == current->start_time &&
	       s->pc == vmf->hermit_fault_pc &&
	       s->start == vmf->vma->vm_start && s->end == vmf->vma->vm_end &&
	       s->generation == gen && s->epoch == epoch;
}

/* Replacement order: unused or invalidated ways first, then the stream idle
 * the longest. An expired stream (idle >= POLICY_TTL) is never counted as an
 * eviction because it could no longer extend a run.
 */
static u64 policy_stream_age(const struct policy_stream *s, u64 gen, u64 epoch,
			     u64 now)
{
	if (!s->time || s->generation != gen || s->epoch != epoch)
		return ~0ULL;
	return now > s->time ? now - s->time : 0;
}

/* Find this thread/PC/VMA stream. @create replaces a way on a miss; lookups
 * from the prediction paths pass false so they never disturb other streams.
 */
static struct policy_stream *policy_stream(struct hermit_policy_state *st,
					   struct vm_fault *vmf, bool create)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long key = vmf->hermit_fault_pc ^ (unsigned long)current;
	struct policy_stream *set, *s, *victim = NULL;
	u64 gen = atomic64_read(&vma->vm_mm->hermit_vma_generation);
	u64 epoch = READ_ONCE(hermit_pebs_epoch);
	u64 now = ktime_get_ns(), age, victim_age = 0;
	unsigned int i;

	set = &st->streams[hash_long(key, POLICY_STREAM_SET_BITS) *
			   POLICY_STREAM_WAYS];
	for (i = 0; i < POLICY_STREAM_WAYS; i++) {
		s = &set[i];
		if (policy_stream_match(s, vmf, gen, epoch))
			return s;
		age = policy_stream_age(s, gen, epoch, now);
		if (!victim || age > victim_age) {
			victim = s;
			victim_age = age;
		}
	}
	if (!create)
		return NULL;
	if (victim_age < POLICY_TTL)
		atomic_long_inc(&stream_evictions);
	memset(victim, 0, sizeof(*victim));
	victim->tid = task_pid_nr(current);
	victim->task_start = current->start_time;
	victim->pc = vmf->hermit_fault_pc;
	victim->start = vma->vm_start;
	victim->end = vma->vm_end;
	victim->generation = gen;
	victim->epoch = epoch;
	return victim;
}

/* The run ended. Doubling sizes the next batch from the run so far, so a run
 * that stops mid-batch leaves the rest of that batch read for nothing; with
 * fixed-length accesses (one 128 KiB record per region) that happens on every
 * run. Remember the length of a run that used prediction, and let the next run
 * of this stream read up to that length but not past it. A run that grows
 * beyond the learned length is no longer capped and records the longer length
 * when it ends. Runs that never predicted carry no evidence and keep the value.
 */
static void policy_stream_break(struct policy_stream *s)
{
	unsigned int order;

	if (s->batch_end) {
		if (s->last >= s->batch_start &&
		    s->last < s->batch_end - PAGE_SIZE)
			atomic_long_inc(&batch_overruns);
		s->fit = min_t(unsigned int, 1024, s->sequential + 1);
		/* Confidence that this is a fixed-length access: count the completed
		 * segments that ended at the same rounded-down power of two. A new
		 * length starts its own run at one, so a mixed workload never looks
		 * stable.
		 */
		order = ilog2(s->fit);
		if (order == s->fit_order) {
			if (s->fit_stable < 255)
				s->fit_stable++;
		} else {
			s->fit_order = order;
			s->fit_stable = 1;
		}
	}
	s->batch_start = s->batch_end = 0;
}

/* @start/@nr is the PTE range installed by this fault. A large folio maps
 * all of it at once, so the stream advances to its end; a base page installs
 * only itself and later swapcache hits report the prefetched pages.
 */
void hermit_policy_installed(struct vm_fault *vmf, unsigned long start,
			     unsigned int nr)
{
	struct hermit_policy_state *st;
	struct policy_stream *s;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address & PAGE_MASK;
	unsigned long end = start + (unsigned long)nr * PAGE_SIZE;
	u64 now = ktime_get_ns();
	bool refault = vmf->hermit_refault;

	if (!hermit_policy_active(vma->vm_mm))
		return;
	st = policy_state(vma->vm_mm);
	if (!st)
		return;
	spin_lock(&st->lock);
	if (refault)
		atomic_long_inc(&refault_pages);
	st->refault = (st->refault * 7 + (refault ? 1024 : 0)) / 8;
	s = policy_stream(st, vmf, true);
	if (s->time && now - s->time < POLICY_TTL &&
	    start <= s->last + PAGE_SIZE && addr >= s->last + PAGE_SIZE &&
	    addr < end) {
		s->sequential =
			min_t(unsigned int, 1024,
			      s->sequential + ((end - (s->last + PAGE_SIZE)) >>
					       PAGE_SHIFT));
	} else {
		policy_stream_break(s);
		/* The demand page of a new run is not counted, so a folio that also
		 * mapped the rest of the segment must add those pages itself; the
		 * equivalent 4 KiB batch counts them through later swapcache faults.
		 */
		s->sequential = nr > 1 ? min_t(unsigned int, 1024, nr - 1) : 0;
	}
	s->last = end - PAGE_SIZE;
	s->time = now;
	/* hermit_swapin_pages() read this many pages from the demand page on;
	 * later faults on them advance s->last through the batch.
	 */
	if (vmf->hermit_batch_pages > 1 && nr == 1) {
		s->batch_start = addr;
		s->batch_end = addr + (unsigned long)vmf->hermit_batch_pages *
					      PAGE_SIZE;
	} else if (nr > 1) {
		/* A large folio maps @nr pages at once, which must look like a batch
		 * of @nr base pages to the run-break logic above: the folio is the
		 * whole read, so its range is what the next run is measured against.
		 */
		s->batch_start = start;
		s->batch_end = end;
	}
#ifdef CONFIG_MEMCG
	if (current->mm) {
		struct mem_cgroup *memcg = get_mem_cgroup_from_mm(current->mm);

		if (memcg) {
			atomic_set(&memcg->hermit_refault, st->refault);
			if (s->sequential >= 2 && st->refault < 256)
				atomic64_set(&memcg->hermit_stream_until,
					     now + 10 * NSEC_PER_MSEC);
			mem_cgroup_put(memcg);
		}
	}
#endif
	spin_unlock(&st->lock);
}

static unsigned int policy_swapin_limit(void)
{
	return clamp_t(u32, READ_ONCE(policy_swapin_max), 1, HERMIT_SWAPIN_MAX);
}

/* Length of the current forward run of this thread/PC/VMA, 0 if broken. */
/* Length of the current forward run, 0 if broken. @remaining, if not NULL,
 * receives the pages left from this fault to the end of the last predicted
 * run's length, or 0 when that length gives no bound (none learned yet, or
 * this run already reached it and keeps growing).
 */
static unsigned int policy_run(struct hermit_policy_state *st,
			       struct vm_fault *vmf, u64 now,
			       unsigned int *remaining)
{
	struct policy_stream *s = policy_stream(st, vmf, false);
	unsigned int run;

	if (remaining)
		*remaining = 0;
	if (!s || now - s->time >= POLICY_TTL || st->refault >= 256 ||
	    (vmf->address & PAGE_MASK) != s->last + PAGE_SIZE)
		return 0;
	run = s->sequential;
	/* The demand page is page run + 1 of this run (0-based). */
	if (remaining && s->fit > run + 1)
		*remaining = s->fit - (run + 1);
	return run;
}

unsigned int hermit_policy_predict(struct vm_fault *vmf)
{
	struct hermit_policy_state *st = policy_state(vmf->vma->vm_mm);
	struct policy_stream *s;
	unsigned int nr = 1, run, pebs, remaining, fit_nr = 0, pos;
	u64 now = ktime_get_ns();

	if (!st || !vmf->hermit_fault_pc)
		return nr;
	spin_lock(&st->lock);
	if (st->refault >= 256) {
		spin_unlock(&st->lock);
		return 1;
	}
	/* Forward runs in the same thread/PC/VMA double the batch: a fully
	 * consumed batch of N pages extends the run by N, so the next miss
	 * reads 2N. No cold-start padding, no inference from WRITE sizes.
	 */
	run = policy_run(st, vmf, now, &remaining);
	/* A run of zero is a segment start: the first fault of a new segment,
	 * or the page after a single-page install. Waiting for the doubling to
	 * reach a repeating length costs one full segment ramp per segment, so
	 * a stream whose completed segments already agree on a length starts at
	 * that length instead of at one page.
	 */
	if (!run && hermit_policy_feature(HERMIT_POLICY_FITSTART)) {
		s = policy_stream(st, vmf, false);
		if (s && now - s->time < POLICY_TTL && s->fit >= 2 &&
		    s->fit_stable >= clamp_t(u32,
					     READ_ONCE(policy_fitstart_stable),
					     1, 8)) {
			pos = (vmf->address & PAGE_MASK) ==
					      s->last + PAGE_SIZE ?
				      s->sequential + 1 :
				      0;
			/* Never past the learned length: fit - pos pages are left. */
			if ((unsigned long)pos < s->fit)
				fit_nr = min_t(unsigned int, s->fit - pos,
					       policy_swapin_limit());
		}
	}
	spin_unlock(&st->lock);
	if (run >= 2) {
		nr = min_t(unsigned int, policy_swapin_limit(),
			   roundup_pow_of_two(run + 1));
		/* Doubling alone overshoots a fixed-length access by up to the
		 * last batch; stop at the length this stream's last predicted run
		 * reached. Batches are SG lists, so any page count is legal.
		 */
		if (remaining && nr > remaining) {
			nr = remaining;
			atomic_long_inc(&capped_batches);
		}
	}
	if (hermit_policy_feature(HERMIT_POLICY_PEBS)) {
		pebs = hermit_pebs_predict_pages(vmf);
		nr = max(nr, min(pebs, policy_swapin_limit()));
	}
	/* The fit length is the whole point of a FITSTART batch, so it replaces
	 * the doubling and the PEBS vote instead of extending them.
	 */
	if (fit_nr > 1) {
		nr = fit_nr;
		atomic_long_inc(&fitstart_batches);
		atomic_long_add(nr, &fitstart_pages);
	}
	atomic_long_add(nr - 1, &predicted_pages);
	return nr;
}

unsigned int hermit_policy_medium_min_order(void)
{
	u32 max =
		clamp_t(u32, READ_ONCE(policy_medium_max_order), 2, PMD_ORDER);

	return clamp_t(u32, READ_ONCE(policy_medium_min_order), 2, max);
}

/*
 * Fit-driven medium order: the last completed segments all had the same
 * rounded-down power-of-two length, so that length can be read as one folio
 * without reading past the segment. The folio is allocated at
 * ALIGN_DOWN(fault, 1 << order), so only an order that starts exactly at this
 * position is legal: pages already mapped in front of it are no longer swap
 * entries. The tunable ceiling keeps the default at 64 KiB, where upstream
 * swpout fallback is still cheap.
 */
static int policy_medium_order(struct policy_stream *s, unsigned long addr,
			       unsigned long orders)
{
	unsigned int pos, left, order, max;

	if (s->fit < 4 ||
	    s->fit_stable < clamp_t(u32, READ_ONCE(policy_medium_stable), 1, 8))
		return -1;
	/* The demand page of a contiguous run is page sequential + 1; a fault
	 * that does not continue the run starts a new segment at page 0.
	 */
	pos = addr == s->last + PAGE_SIZE ? s->sequential + 1 : 0;
	if (pos + 1 > s->fit) /* the run already reached the learned length */
		return -1;
	left = s->fit - pos;
	max = clamp_t(u32, READ_ONCE(policy_medium_max_order), 2, PMD_ORDER);
	order = min_t(unsigned int, ilog2(s->fit), max);
	while (order >= 2 && (!(orders & BIT(order)) || (1U << order) > left ||
			      (pos & ((1U << order) - 1))))
		order--;
	if (order >= 2 && order < hermit_policy_medium_min_order()) {
		atomic_long_inc(&medium_below_min);
		return -1;
	}
	return order >= 2 ? (int)order : -1;
}

/*
 * Suggested folio order for this fault, -1 to keep the 4 KiB SG batches.
 * LARGE keeps its phase-A rule (a long run that cannot be trimmed to the SG
 * budget is cheaper as one contiguous mTHP read) and lets alloc_swap_folio()
 * validate the extent; MEDIUM adds a fit-driven order that the caller validates
 * before allocating anything.
 */
int hermit_policy_swapin_order(struct vm_fault *vmf, unsigned long orders,
			       bool *medium)
{
	struct hermit_policy_state *st;
	struct policy_stream *s;
	unsigned int run, remaining, limit;
	u64 now;
	int order = -1;

	if (medium)
		*medium = false;
	if (!vmf->hermit_fault_pc || !(orders & ~(BIT(0) | BIT(1))))
		return -1;
	if (!hermit_policy_feature(HERMIT_POLICY_LARGE) &&
	    !hermit_policy_feature(HERMIT_POLICY_MEDIUM))
		return -1;
	st = READ_ONCE(vmf->vma->vm_mm->hermit_policy);
	if (!st)
		return -1;
	now = ktime_get_ns();
	limit = 2 * policy_swapin_limit();
	spin_lock(&st->lock);
	run = policy_run(st, vmf, now, &remaining);
	if (hermit_policy_feature(HERMIT_POLICY_LARGE) && run >= limit &&
	    (!remaining || remaining >= limit)) {
		/* No extra restriction: take the largest legal folio. */
		order = min_t(unsigned int, ilog2(orders), PMD_ORDER);
		atomic_long_inc(&large_streams);
	} else if (hermit_policy_feature(HERMIT_POLICY_MEDIUM)) {
		s = policy_stream(st, vmf, false);
		if (s && now - s->time < POLICY_TTL && st->refault < 256) {
			order = policy_medium_order(s, vmf->address & PAGE_MASK,
						    orders);
			if (order >= 0) {
				if (medium)
					*medium = true;
				atomic_long_inc(&medium_suggested);
			}
		}
	}
	spin_unlock(&st->lock);
	return order;
}

/* Called from alloc_swap_folio() only when a MEDIUM order was requested. */
void hermit_policy_medium_used(unsigned int order)
{
	if (order >= 2 && order <= PMD_ORDER)
		atomic_long_inc(&medium_used[order]);
	else
		atomic_long_inc(&medium_fallback_alloc);
}

void hermit_policy_medium_extent(enum swapin_extent reason)
{
	atomic_long_inc(&medium_fallback_extent);
	/* An empty VMA order mask has no extent to check. */
	if (reason != SWAPIN_EXTENT_NO_ORDER)
		atomic_long_inc(&medium_extent_reasons[reason]);
}

/*
 * May the demand fault install PTEs for the @pages pages its SG batch read?
 * The batch has to sit inside a segment length this stream repeated often
 * enough to be trusted: pages past it were speculative readahead, and mapping
 * them would hide a wrong prediction from the next run instead of letting the
 * next run fault and correct it. The position is the same one
 * policy_medium_order() uses: page sequential + 1 for a contiguous run, 0 for
 * a fault that starts a new segment.
 */
static bool hermit_policy_pte_install_check(struct vm_fault *vmf,
					    unsigned int pages)
{
	struct hermit_policy_state *st =
		READ_ONCE(vmf->vma->vm_mm->hermit_policy);
	struct policy_stream *s;
	unsigned int pos;
	bool ok = false;
	u64 now;

	if (pages < 2)
		return false;
	if (!hermit_policy_feature(HERMIT_POLICY_PTEBATCH) || !st ||
	    !vmf->hermit_fault_pc)
		return false;
	now = ktime_get_ns();
	spin_lock(&st->lock);
	if (st->refault < 256) {
		s = policy_stream(st, vmf, false);
		if (s && now - s->time < POLICY_TTL &&
		    s->fit_stable >= clamp_t(u32,
					     READ_ONCE(policy_fitstart_stable),
					     1, 8)) {
			pos = (vmf->address & PAGE_MASK) ==
					      s->last + PAGE_SIZE ?
				      s->sequential + 1 :
				      0;
			ok = (unsigned long)pos + pages <= s->fit;
		}
	}
	spin_unlock(&st->lock);
	return ok;
}

bool hermit_policy_pte_install_ready(struct vm_fault *vmf, unsigned int pages)
{
	return hermit_policy_pte_install_check(vmf, pages);
}

bool hermit_policy_pte_install(struct vm_fault *vmf, unsigned int pages)
{
	bool ok;

	if (pages < 2)
		return false;
	ok = hermit_policy_pte_install_check(vmf, pages);
	if (!ok)
		atomic_long_inc(&ptebatch_gate_rejects);
	return ok;
}

/* Pages of an SG batch whose PTE was installed by the demand fault. */
void hermit_policy_pte_installed(unsigned int pages)
{
	if (pages)
		atomic_long_add(pages, &ptebatch_installed);
}

/* Prefetched pages the batch did not map, e.g. after the first mismatch. */
void hermit_policy_pte_skipped(unsigned int pages)
{
	if (pages)
		atomic_long_add(pages, &ptebatch_skipped);
}

/* Record one page read speculatively by hermit_swapin_pages(). The caller
 * tests hermit_swapin_cost_enabled() first, so an uninstrumented run pays
 * nothing for this bookkeeping.
 */
void hermit_policy_prefetched(struct mm_struct *mm, unsigned long entry_val)
{
	struct hermit_policy_state *st = READ_ONCE(mm->hermit_policy);
	typeof(st->prefetch[0]) *slot;
	unsigned long flags;

	if (!st)
		return;
	spin_lock_irqsave(&st->lock, flags);
	slot = &st->prefetch[hash_long(entry_val, POLICY_PREFETCH_BITS)];
	slot->entry = entry_val;
	slot->time = ktime_get_ns();
	spin_unlock_irqrestore(&st->lock, flags);
}

/* A demand fault found its page in swapcache: was it one of ours? A hit is
 * consumed so a later refault of the same entry is not counted twice.
 */
bool hermit_policy_prefetch_hit(struct mm_struct *mm, unsigned long entry_val)
{
	struct hermit_policy_state *st = READ_ONCE(mm->hermit_policy);
	typeof(st->prefetch[0]) *slot;
	unsigned long flags;
	u64 now;
	bool hit = false;

	if (!st)
		return false;
	now = ktime_get_ns();
	spin_lock_irqsave(&st->lock, flags);
	slot = &st->prefetch[hash_long(entry_val, POLICY_PREFETCH_BITS)];
	if (slot->time && slot->entry == entry_val &&
	    now - slot->time < POLICY_TTL) {
		slot->time = 0;
		hit = true;
	}
	spin_unlock_irqrestore(&st->lock, flags);
	return hit;
}

struct hint_walk {
	struct hermit_reclaim_hint *hint;
	unsigned int mappings;
};

static bool hint_one(struct folio *folio, struct vm_area_struct *vma,
		     unsigned long address, void *arg)
{
	struct hint_walk *walk = arg;
	struct hermit_reclaim_hint *hint = walk->hint;
	struct mm_struct *mm = vma->vm_mm;
	struct hermit_policy_state *st;

	walk->mappings++;
	if (!hermit_policy_active(mm) || mm_has_notifiers(mm))
		return true;
	st = policy_state(mm);
	if (!st)
		return true;

	/* MGLRU selected the folio. Read the batch key without the policy lock. */
	hint->owner = st->id;
	hint->generation = atomic64_read(&mm->hermit_vma_generation);
	hint->start = vma->vm_start;
	hint->end = vma->vm_end;
	hint->address = address & PAGE_MASK;
	return true;
}

void hermit_policy_eviction(struct folio *folio,
			    struct hermit_reclaim_hint *hint)
{
	struct hint_walk walk = { .hint = hint };
	struct rmap_walk_control rwc = { .rmap_one = hint_one,
					 .arg = &walk,
					 .try_lock = true };

	*hint = (struct hermit_reclaim_hint){};
	if (!policy_mode())
		return;
	if (!folio_test_anon(folio) || folio_test_large(folio) ||
	    folio_mapcount(folio) != 1 || folio_test_ksm(folio))
		return;
	rmap_walk(folio, &rwc);
	if (rwc.contended || walk.mappings != 1)
		hint->owner = 0;
	if (hint->owner)
		atomic_long_inc(&mglru_candidates);
}

unsigned long hermit_policy_reserve(struct mem_cgroup *memcg)
{
#ifdef CONFIG_MEMCG
	unsigned long max = READ_ONCE(memcg->memory.max);

	if (!hermit_policy_feature(HERMIT_POLICY_RESERVE) ||
	    max == PAGE_COUNTER_MAX ||
	    atomic_read(&memcg->hermit_refault) >= 256 ||
	    atomic64_read(&memcg->hermit_stream_until) < ktime_get_ns())
		return 0;
	return min_t(unsigned long, HERMIT_BATCH_MAX * 2, max / 16);
#else
	return 0;
#endif
}

/* Reclaim has sorted these locked, same-device folios by virtual address.
 * Their swap entries are already in PTEs. Inspect the layout before I/O.
 */
void hermit_policy_slotrun(const struct hermit_batch *batch)
{
	unsigned int i, nr = batch->nr;
	unsigned long addr = batch->addresses[0];
	unsigned long first = swp_offset(batch->entries[0]);

	if ((nr != 4 && nr != 8 && nr != 16) ||
	    !IS_ALIGNED(addr, nr * PAGE_SIZE))
		return;
	for (i = 1; i < nr; i++)
		if (batch->addresses[i] != addr + i * PAGE_SIZE)
			return;

	atomic_long_inc(&slotrun_groups);
	if (IS_ALIGNED(first, nr)) {
		for (i = 1; i < nr; i++)
			if (swp_offset(batch->entries[i]) != first + i)
				break;
		if (i == nr) {
			atomic_long_inc(&slotrun_contiguous);
			return;
		}
	}
	atomic_long_inc(&slotrun_fallback);
}

void hermit_policy_mm_exit(struct mm_struct *mm)
{
	struct hermit_policy_state *st = mm->hermit_policy;

	if (!st)
		return;
	mm->hermit_policy = NULL;
	kfree(st);
}

static int policy_stats_show(struct seq_file *m, void *unused)
{
	static const struct {
		const char *name;
		atomic_long_t *value;
	} rows[] = {
		{ "predicted_4k", &predicted_pages },
		{ "refault_4k", &refault_pages },
		{ "large_streams", &large_streams },
		{ "stream_evictions", &stream_evictions },
		{ "batch_overruns", &batch_overruns },
		{ "capped_batches", &capped_batches },
		{ "mglru_candidates", &mglru_candidates },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rows); i++)
		seq_printf(m, "%s %ld\n", rows[i].name,
			   atomic_long_read(rows[i].value));
	/* Phase B, appended: one suggestion per fault, then exactly one of the
	 * per-order / fallback counters (order 0 and 1 are never medium orders).
	 */
	seq_printf(m, "medium_suggested %ld\n",
		   atomic_long_read(&medium_suggested));
	for (i = 2; i <= PMD_ORDER; i++)
		seq_printf(m, "medium_used_order%u %ld\n", i,
			   atomic_long_read(&medium_used[i]));
	seq_printf(m, "medium_fallback_alloc %ld\n",
		   atomic_long_read(&medium_fallback_alloc));
	seq_printf(m, "medium_fallback_extent %ld\n",
		   atomic_long_read(&medium_fallback_extent));
	/* Phase C, appended: batches started at the learned fit length, the pages
	 * they cover (demand page included), and the PTEs those batches mapped.
	 */
	seq_printf(m, "fitstart_batches %ld\n",
		   atomic_long_read(&fitstart_batches));
	seq_printf(m, "fitstart_pages %ld\n",
		   atomic_long_read(&fitstart_pages));
	seq_printf(m, "ptebatch_installed %ld\n",
		   atomic_long_read(&ptebatch_installed));
	seq_printf(m, "ptebatch_skipped %ld\n",
		   atomic_long_read(&ptebatch_skipped));
	/* Phase D, appended: PEBS-guided split of resident large folios. */
	hermit_split_stats_show(m);
	seq_printf(m, "ptebatch_gate_rejects %ld\n",
		   atomic_long_read(&ptebatch_gate_rejects));
	hermit_split_extra_stats_show(m);
	seq_printf(m, "medium_fallback_swpbatch %ld\n",
		   atomic_long_read(
			   &medium_extent_reasons[SWAPIN_EXTENT_SWPBATCH]));
	seq_printf(
		m, "medium_fallback_cache %ld\n",
		atomic_long_read(&medium_extent_reasons[SWAPIN_EXTENT_CACHE]));
	seq_printf(
		m, "medium_fallback_remote %ld\n",
		atomic_long_read(&medium_extent_reasons[SWAPIN_EXTENT_REMOTE]));
	seq_printf(
		m, "medium_fallback_align %ld\n",
		atomic_long_read(&medium_extent_reasons[SWAPIN_EXTENT_ALIGN]));
	seq_printf(m, "medium_below_min %ld\n",
		   atomic_long_read(&medium_below_min));
	seq_printf(m, "slotrun_groups %ld\n",
		   atomic_long_read(&slotrun_groups));
	seq_printf(m, "slotrun_contiguous %ld\n",
		   atomic_long_read(&slotrun_contiguous));
	seq_printf(m, "slotrun_fallback %ld\n",
		   atomic_long_read(&slotrun_fallback));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(policy_stats);
void hermit_policy_debugfs_init(struct dentry *root)
{
	debugfs_create_file("policy_stats", 0400, root, NULL,
			    &policy_stats_fops);
	debugfs_create_x32("policy_features", 0600, root, &policy_features);
	debugfs_create_u32("policy_swapin_max", 0600, root, &policy_swapin_max);
	debugfs_create_u32("policy_medium_max_order", S_IRUSR | S_IWUSR, root,
			   &policy_medium_max_order);
	debugfs_create_u32("policy_medium_min_order", S_IRUSR | S_IWUSR, root,
			   &policy_medium_min_order);
	debugfs_create_u32("policy_medium_stable", S_IRUSR | S_IWUSR, root,
			   &policy_medium_stable);
	debugfs_create_u32("policy_fitstart_stable", S_IRUSR | S_IWUSR, root,
			   &policy_fitstart_stable);
}
