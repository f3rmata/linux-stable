// SPDX-License-Identifier: GPL-2.0
/* Base-page evidence and bounded reclaim ranking. No speculative capacity is
 * hidden from memcg: the swapcache owns every prefetched 4 KiB charge.
 */
#include <linux/debugfs.h>
#include <linux/hash.h>
#include <linux/hermit_backend.h>
#include <linux/hermit_pebs.h>
#include <linux/hermit_policy.h>
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

#define POLICY_BUCKET_BITS 10
#define POLICY_BUCKETS (1U << POLICY_BUCKET_BITS)
#define POLICY_PAGES_LIMIT 262144
/* Stream state is set-associative: a key hashes to one set and may occupy any
 * of its ways. With one slot per hash, two threads that collided reset each
 * other's run on every fault and never reached the large-folio threshold.
 */
#define POLICY_STREAM_SET_BITS 3
#define POLICY_STREAM_WAYS 8
#define POLICY_STREAMS ((1U << POLICY_STREAM_SET_BITS) * POLICY_STREAM_WAYS)
#define POLICY_SCAN 32
#define POLICY_TTL NSEC_PER_SEC
/* Cost instrumentation only: direct-mapped slots for the swap entries that
 * hermit_swapin_pages() read speculatively. 256 slots cover several in-flight
 * batches; a collision only loses one hit count, never a policy decision.
 */
#define POLICY_PREFETCH_BITS 8
#define POLICY_PREFETCH_SLOTS (1U << POLICY_PREFETCH_BITS)

/* debugfs tunables; see HERMIT_POLICY_* for the ablation bits. */
static u32 policy_features = HERMIT_POLICY_DEFAULT;
static u32 policy_cold_us = 1000;	/* minimum empty observation interval */
static u32 policy_cold_epochs = 1;	/* empty intervals required for cold */
static u32 policy_pages_max = 32768;	/* tracked base pages per mm */
static u32 policy_swapin_max = HERMIT_SWAPIN_MAX;
/* Phase B: one medium folio instead of a 4 KiB batch. The default ceiling is
 * 64 KiB because upstream swpout fallback rates rise steeply above it, and a
 * segment length must repeat before it is trusted.
 */
static u32 policy_medium_max_order = 4;
static u32 policy_medium_stable = 3;

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

static unsigned int policy_epochs(void)
{
	return clamp_t(u32, READ_ONCE(policy_cold_epochs), 1, 8);
}

struct policy_page {
	struct hlist_node node;
	struct list_head lru;
	unsigned long address, pfn, start, end;
	u64 generation, epoch, installed, observed, sampled, hot_until, reuse;
	u64 aging_epoch;
	u8 empty, uses;
	bool epoch_hit, valid, ghost;
};
struct policy_stream {
	u64 task_start, generation, epoch, time;
	pid_t tid;
	unsigned long pc, start, end, last;
	u16 sequential;		/* consecutive base pages, demand + prefetch */
	/* The last predicted batch [batch_start, batch_end) as actually read.
	 * Later faults on its prefetched pages advance s->last through it, so a
	 * run that breaks before batch_end left part of it unused.
	 */
	unsigned long batch_start, batch_end;
	u16 fit;		/* length of the last predicted run, 0 = none yet */
	/* Medium-order confidence: how many completed segments in a row ended at
	 * the same rounded-down power-of-two length. Both fields live in the
	 * padding the struct already had, so no stream slot grows.
	 */
	u8 fit_order;		/* ilog2(fit) */
	u8 fit_stable;		/* consecutive completed segments of fit_order */
};
struct hermit_policy_state {
	spinlock_t lock;
	struct hlist_head *pages;	/* POLICY_BUCKETS heads */
	struct list_head lru;
	struct policy_stream streams[POLICY_STREAMS];
	unsigned int nr, refault;
	u64 id;
	/* Swap entries read speculatively by hermit_swapin_pages(), used only by
	 * the swap-in cost counters (debugfs swapin_cost_enable).
	 */
	struct {
		unsigned long entry;	/* swp_entry_t.val */
		u64 time;		/* 0 = unused */
	} prefetch[POLICY_PREFETCH_SLOTS];
};
static atomic64_t policy_ids;
static atomic64_t policy_aging_epoch;
static atomic_long_t observed_pages, cold_pages, invalid_epochs, late_samples;
static atomic_long_t predicted_pages, refault_pages, ranked_pages;
/* Cold-evidence lifecycle: why observations did or did not become cold. */
static atomic_long_t started_epochs, young_epochs, short_epochs, evicted_pages;
static atomic_long_t sorted_lists, large_streams, unmapped_pages;
/* A live stream (used within POLICY_TTL) replaced by a different key. */
static atomic_long_t stream_evictions;
/* Runs that ended inside a predicted batch; capped_batches = predictions cut by that cap. */
static atomic_long_t batch_overruns, capped_batches;
static atomic_long_t mglru_candidates, mglru_sort_bypasses;
/* Fit-driven medium orders: one suggestion per fault, then exactly one of
 * medium_used[order], medium_fallback_alloc or medium_fallback_extent.
 */
static atomic_long_t medium_suggested, medium_fallback_alloc;
static atomic_long_t medium_fallback_extent;
static atomic_long_t medium_used[PMD_ORDER + 1];

/* A round trip through MGLRU must not complete an old empty PTE interval. */
void hermit_policy_lru_changed(void)
{
	atomic64_inc(&policy_aging_epoch);
}

bool hermit_policy_active(struct mm_struct *mm)
{
	return policy_mode() &&
		(READ_ONCE(hermit_pebs_all) || READ_ONCE(mm->hermit_pebs_enabled));
}

/* Minimum length of an observation interval and of the hot-protection window.
 * The old 14 us default was an RDMA latency, not an access interval.
 */
static u64 policy_interval(void)
{
	return max_t(u64, (u64)READ_ONCE(policy_cold_us) * NSEC_PER_USEC,
		     2ULL * READ_ONCE(hermit_pebs_fixed_cost_ns));
}

static struct hermit_policy_state *policy_state(struct mm_struct *mm)
{
	struct hermit_policy_state *st = READ_ONCE(mm->hermit_policy), *new;

	if (st || !hermit_policy_active(mm))
		return st;
	new = kzalloc(sizeof(*new), GFP_NOWAIT | __GFP_NOWARN);
	if (!new)
		return NULL;
	new->pages = kcalloc(POLICY_BUCKETS, sizeof(*new->pages),
			     GFP_NOWAIT | __GFP_NOWARN);
	if (!new->pages) {
		kfree(new);
		return NULL;
	}
	spin_lock_init(&new->lock);
	INIT_LIST_HEAD(&new->lru);
	new->id = atomic64_inc_return(&policy_ids);
	st = cmpxchg(&mm->hermit_policy, NULL, new);
	if (st) {
		kfree(new->pages);
		kfree(new);
	} else
		st = new;
	return st;
}

static struct policy_page *policy_lookup(struct hermit_policy_state *st,
		unsigned long addr)
{
	struct policy_page *p;

	hlist_for_each_entry(p, &st->pages[hash_long(addr >> PAGE_SHIFT,
						     POLICY_BUCKET_BITS)], node)
		if (p->address == addr)
			return p;
	return NULL;
}

static void policy_free(struct hermit_policy_state *st, struct policy_page *p)
{
	hlist_del(&p->node);
	list_del(&p->lru);
	st->nr--;
	kfree(p);
}

static struct policy_page *policy_page(struct hermit_policy_state *st,
		unsigned long addr, bool create)
{
	struct policy_page *p;
	unsigned int hash;

	addr &= PAGE_MASK;
	hash = hash_long(addr >> PAGE_SHIFT, POLICY_BUCKET_BITS);
	p = policy_lookup(st, addr);
	if (p) {
		list_move(&p->lru, &st->lru);
		return p;
	}
	if (!create)
		return NULL;
	if (st->nr >= clamp_t(u32, READ_ONCE(policy_pages_max), 1, POLICY_PAGES_LIMIT)) {
		p = list_last_entry(&st->lru, struct policy_page, lru);
		hlist_del(&p->node);
		list_del(&p->lru);
		atomic_long_inc(&evicted_pages);
	} else {
		p = kzalloc(sizeof(*p), GFP_NOWAIT | __GFP_NOWARN);
		if (!p)
			return NULL;
		st->nr++;
	}
	memset(p, 0, sizeof(*p));
	p->address = addr;
	p->pfn = ULONG_MAX;
	hlist_add_head(&p->node, &st->pages[hash]);
	list_add(&p->lru, &st->lru);
	return p;
}

static void policy_reset(struct policy_page *p, struct vm_area_struct *vma,
		unsigned long pfn, u64 now)
{
	p->pfn = pfn;
	p->start = vma->vm_start;
	p->end = vma->vm_end;
	p->generation = atomic64_read(&vma->vm_mm->hermit_vma_generation);
	p->epoch = READ_ONCE(hermit_pebs_epoch);
	p->aging_epoch = atomic64_read(&policy_aging_epoch);
	p->installed = p->observed = now;
	p->sampled = p->hot_until = p->reuse = 0;
	p->empty = p->uses = 0;
	p->valid = p->epoch_hit = p->ghost = false;
}

/* Called at the point the PTE young bit is tested AND cleared. Thus another
 * aging consumer cannot turn a used interval into an empty observation.
 * Untracked/large/shared mappings and incomplete observations stay unknown.
 */
void hermit_policy_observe(struct folio *folio, struct vm_area_struct *vma,
		unsigned long addr, bool young, bool valid)
{
	struct hermit_policy_state *st;
	struct policy_page *p;
	u64 now, interval;
	bool flush = false;

	/* MGLRU owns accessed-bit aging, including neighboring PTEs. It does
	 * not provide the flushed, complete intervals this observer requires.
	 */
	if (lru_gen_enabled() || !hermit_policy_feature(HERMIT_POLICY_OBSERVE) ||
	    !hermit_policy_active(vma->vm_mm) || !folio_test_anon(folio))
		return;
	now = ktime_get_ns();
	interval = policy_interval();
	valid &= !folio_test_large(folio) && folio_mapcount(folio) == 1 &&
		 !mm_has_notifiers(vma->vm_mm);
	/* Only exclusive base pages can become cold evidence. Filter before
	 * touching the table: every PTE of a large folio is aged separately,
	 * and creating an entry per PTE would evict valid base-page history.
	 * An existing entry for this address is reset, never newly created.
	 */
	if (!valid) {
		st = READ_ONCE(vma->vm_mm->hermit_policy);
		if (!st)
			return;
		spin_lock(&st->lock);
		p = policy_lookup(st, addr & PAGE_MASK);
		if (p) {
			p->valid = false;
			p->empty = 0;
			p->observed = now;
		}
		spin_unlock(&st->lock);
		atomic_long_inc(&invalid_epochs);
		return;
	}
	st = policy_state(vma->vm_mm);
	if (!st)
		return;
	spin_lock(&st->lock);
	p = policy_page(st, addr, true);
	if (!p)
		goto out;
	if (p->pfn != folio_pfn(folio) || p->start != vma->vm_start ||
	    p->end != vma->vm_end || p->ghost ||
	    p->epoch != READ_ONCE(hermit_pebs_epoch) ||
	    p->generation != atomic64_read(&vma->vm_mm->hermit_vma_generation))
		policy_reset(p, vma, folio_pfn(folio), now);
	if (p->aging_epoch != atomic64_read(&policy_aging_epoch)) {
		p->aging_epoch = atomic64_read(&policy_aging_epoch);
		p->valid = false;
		p->empty = 0;
	}
	p->epoch_hit |= young || p->sampled >= p->observed;
	if (young) {
		p->hot_until = now + interval;
		p->empty = 0;
	}
	if (!p->valid) {
		/* First observation only opens an interval. */
		p->observed = now;
		p->epoch_hit = false;
		p->valid = flush = true;
		atomic_long_inc(&started_epochs);
		goto out;
	}
	if (now - p->observed < interval) {
		atomic_long_inc(&short_epochs);
		goto out;
	}
	if (p->epoch_hit) {
		p->empty = 0;
		atomic_long_inc(&young_epochs);
	} else {
		p->empty = min_t(unsigned int, policy_epochs(), p->empty + 1);
	}
	p->epoch_hit = false;
	p->observed = now;
	flush = true;
	atomic_long_inc(&observed_pages);
	if (p->empty >= policy_epochs())
		atomic_long_inc(&cold_pages);
out:
	spin_unlock(&st->lock);
	/* x86 young-bit aging omits the TLB flush, so a cached accessed bit
	 * could hide accesses in the next interval. Queue the flush in the
	 * reclaimer's unmap batch instead of one IPI per page; reclaim flushes
	 * that batch before the interval (policy_cold_us) can complete.
	 */
	if (flush && hermit_policy_feature(HERMIT_POLICY_FLUSH))
		hermit_policy_defer_flush(vma, addr);
}

void hermit_policy_sample(struct mm_struct *mm, unsigned long addr, u64 time)
{
	struct hermit_policy_state *st = READ_ONCE(mm->hermit_policy);
	struct policy_page *p;
	u64 now;

	/* Samples only feed the negative-observation evidence (reuse, hot_until,
	 * epoch_hit). Under MGLRU nothing reads it, so skip the lock entirely.
	 */
	if (!st || lru_gen_enabled() || !hermit_policy_active(mm))
		return;
	now = ktime_get_ns();
	if (time > now)
		return;
	spin_lock(&st->lock);
	p = policy_page(st, addr, false);
	if (!p || p->ghost || p->epoch != READ_ONCE(hermit_pebs_epoch) || p->generation != atomic64_read(&mm->hermit_vma_generation) ||
	    time < p->installed || time < p->observed || now - time > 10 * NSEC_PER_MSEC) {
		atomic_long_inc(&late_samples);
		goto out;
	}
	if (p->sampled && time > p->sampled)
		p->reuse = p->reuse ? (p->reuse * 3 + time - p->sampled) / 4 : time - p->sampled;
	p->sampled = max(p->sampled, time);
	p->hot_until = max(p->hot_until, time + policy_interval());
	p->empty = 0;
	p->epoch_hit = true;
	p->uses = min_t(unsigned int, 255, p->uses + 1);
out:
	spin_unlock(&st->lock);
}

static bool policy_stream_match(const struct policy_stream *s,
		struct vm_fault *vmf, u64 gen, u64 epoch)
{
	return s->time && s->tid == task_pid_nr(current) &&
	       s->task_start == current->start_time &&
	       s->pc == vmf->hermit_fault_pc && s->start == vmf->vma->vm_start &&
	       s->end == vmf->vma->vm_end && s->generation == gen && s->epoch == epoch;
}

/* Replacement order: unused or invalidated ways first, then the stream idle
 * the longest. An expired stream (idle >= POLICY_TTL) is never counted as an
 * eviction because it could no longer extend a run.
 */
static u64 policy_stream_age(const struct policy_stream *s, u64 gen, u64 epoch, u64 now)
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

	set = &st->streams[hash_long(key, POLICY_STREAM_SET_BITS) * POLICY_STREAM_WAYS];
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
		if (s->last >= s->batch_start && s->last < s->batch_end - PAGE_SIZE)
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
void hermit_policy_installed(struct vm_fault *vmf, unsigned long pfn,
		unsigned long start, unsigned int nr)
{
	struct hermit_policy_state *st;
	struct policy_page *p;
	struct policy_stream *s;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address & PAGE_MASK;
	unsigned long end = start + (unsigned long)nr * PAGE_SIZE;
	u64 now = ktime_get_ns();
	bool refault = false;

	if (!hermit_policy_active(vma->vm_mm))
		return;
	st = policy_state(vma->vm_mm);
	if (!st)
		return;
	spin_lock(&st->lock);
	if (lru_gen_enabled()) {
		/* MGLRU: do_swap_page() read the swap shadow before swap-in, and
		 * protecting the refaulted page is MGLRU's job. No per-page entry.
		 */
		refault = vmf->hermit_refault;
		if (refault)
			atomic_long_inc(&refault_pages);
	} else {
		p = policy_page(st, addr, nr == 1);
		if (p) {
			refault = p->ghost && now - p->observed < POLICY_TTL &&
				p->start == vma->vm_start && p->end == vma->vm_end &&
				p->epoch == READ_ONCE(hermit_pebs_epoch) &&
				p->generation == atomic64_read(&vma->vm_mm->hermit_vma_generation);
			if (nr == 1)
				policy_reset(p, vma, pfn, now);
			else
				policy_free(st, p);	/* large folios are never tracked */
			if (refault && nr == 1) {
				p->hot_until = now + 4 * policy_interval();
				atomic_long_inc(&refault_pages);
			}
		}
	}
	st->refault = (st->refault * 7 + (refault ? 1024 : 0)) / 8;
	s = policy_stream(st, vmf, true);
	if (s->time && now - s->time < POLICY_TTL && start <= s->last + PAGE_SIZE &&
	    addr >= s->last + PAGE_SIZE && addr < end) {
		s->sequential = min_t(unsigned int, 1024, s->sequential +
				      ((end - (s->last + PAGE_SIZE)) >> PAGE_SHIFT));
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
		s->batch_end = addr + (unsigned long)vmf->hermit_batch_pages * PAGE_SIZE;
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
				atomic64_set(&memcg->hermit_stream_until, now + 10 * NSEC_PER_MSEC);
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
		struct vm_fault *vmf, u64 now, unsigned int *remaining)
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
	unsigned int nr = 1, run, pebs, remaining;
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
	atomic_long_add(nr - 1, &predicted_pages);
	return nr;
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
static int policy_medium_order(struct policy_stream *s, unsigned long addr)
{
	unsigned int pos, left, order, max;

	if (s->fit < 4 ||
	    s->fit_stable < clamp_t(u32, READ_ONCE(policy_medium_stable), 1, 8))
		return -1;
	/* The demand page of a contiguous run is page sequential + 1; a fault
	 * that does not continue the run starts a new segment at page 0.
	 */
	pos = addr == s->last + PAGE_SIZE ? s->sequential + 1 : 0;
	if (pos + 1 > s->fit)		/* the run already reached the learned length */
		return -1;
	left = s->fit - pos;
	max = clamp_t(u32, READ_ONCE(policy_medium_max_order), 2, PMD_ORDER);
	order = min_t(unsigned int, ilog2(s->fit), max);
	while (order >= 2 &&
	       ((1U << order) > left || (pos & ((1U << order) - 1))))
		order--;
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
			order = policy_medium_order(s, vmf->address & PAGE_MASK);
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

/* The requested order was not used: @alloc separates an allocation or memcg
 * charge failure from an extent that no legal order covered.
 */
void hermit_policy_medium_fallback(bool alloc)
{
	atomic_long_inc(alloc ? &medium_fallback_alloc : &medium_fallback_extent);
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
	bool evict;
	bool mglru;
};
static bool hint_one(struct folio *folio, struct vm_area_struct *vma,
		unsigned long address, void *arg)
{
	struct hint_walk *walk = arg;
	struct hermit_reclaim_hint *hint = walk->hint;
	struct hermit_policy_state *st = READ_ONCE(vma->vm_mm->hermit_policy);
	struct policy_page *p;
	u64 now = ktime_get_ns();

	walk->mappings++;
	if (!hermit_policy_active(vma->vm_mm))
		return true;
	if (walk->mglru) {
		/* MGLRU selected this folio and its swap shadow will report a
		 * refault, so neither a table entry nor a ghost is needed: the
		 * batch key comes straight from the VMA, without taking the lock.
		 */
		if (mm_has_notifiers(vma->vm_mm))
			return true;
		st = policy_state(vma->vm_mm);
		if (!st)
			return true;
		hint->owner = st->id;
		hint->generation = atomic64_read(&vma->vm_mm->hermit_vma_generation);
		hint->start = vma->vm_start;
		hint->end = vma->vm_end;
		hint->address = address & PAGE_MASK;
		hint->mglru = true;
		return true;
	}
	if (!st)
		return true;
	spin_lock(&st->lock);
	p = policy_page(st, address, false);
	if (p && p->pfn == folio_pfn(folio) && !p->ghost &&
	    p->epoch == READ_ONCE(hermit_pebs_epoch) &&
	    p->generation == atomic64_read(&vma->vm_mm->hermit_vma_generation)) {
		hint->owner = st->id;
		hint->generation = p->generation;
		hint->start = p->start;
		hint->end = p->end;
		hint->address = p->address;
		hint->age = p->sampled ?: p->installed;
		hint->mglru = walk->mglru;
		hint->cold = !walk->mglru && !lru_gen_enabled() &&
			p->aging_epoch == atomic64_read(&policy_aging_epoch) &&
			p->valid && p->empty >= policy_epochs() && now >= p->hot_until &&
			(p->uses < 2 || now - p->sampled >= 2 * p->reuse);
		hint->heat = hint->cold ? 0 :
			(now < p->hot_until ||
			 (p->uses >= 2 && now - p->sampled < 2 * p->reuse)) ? 2 : 1;
	}
	/* Record the ghost while the rmap still supplies a live mm. */
	if (p && walk->evict && p->pfn == folio_pfn(folio)) {
		p->ghost = true;
		p->observed = now;
	}
	spin_unlock(&st->lock);
	return true;
}

static void policy_walk(struct folio *folio, struct hermit_reclaim_hint *hint,
		bool evict, bool lru_gen_reclaim)
{
	struct hint_walk walk = {
		.hint = hint, .evict = evict,
		.mglru = evict && lru_gen_reclaim &&
			 hermit_policy_feature(HERMIT_POLICY_MGLRU),
	};
	struct rmap_walk_control rwc = { .rmap_one = hint_one, .arg = &walk, .try_lock = true };

	memset(hint, 0, sizeof(*hint));
	hint->heat = 1;
	if (!policy_mode())
		return;
	if (!folio_test_anon(folio) || folio_test_large(folio) ||
	    folio_mapcount(folio) != 1 || folio_test_ksm(folio))
		return;
	rmap_walk(folio, &rwc);
	if (rwc.contended || walk.mappings != 1) {
		hint->cold = false;
		hint->mglru = false;
		hint->owner = 0;
		hint->heat = 1;
	}
	if (hint->mglru)
		atomic_long_inc(&mglru_candidates);
}

void hermit_policy_hint(struct folio *folio, struct hermit_reclaim_hint *hint)
{
	policy_walk(folio, hint, false, false);
}

/* The folio is about to be unmapped for swap-out: return its hint and mark
 * the tracked page as a ghost so a later demand fault counts as a refault.
 */
void hermit_policy_eviction(struct folio *folio, struct hermit_reclaim_hint *hint,
		bool lru_gen_reclaim)
{
	policy_walk(folio, hint, true, lru_gen_reclaim);
}

struct policy_rank {
	struct folio *folio;
	u64 age;
	unsigned int score;
};

static bool policy_rank_before(const struct policy_rank *a,
		const struct policy_rank *b)
{
	return a->score < b->score || (a->score == b->score && a->age < b->age);
}

/* Called once per shrink_folio_list() pass. The list is consumed from its
 * tail (lru_to_folio), so rank the POLICY_SCAN folios nearest the tail and
 * leave the best candidate at the tail. Clean cold pages come first, then
 * dirty cold pages, unknown pages and recently used pages. Folios that cannot
 * be locked keep their place. This only changes order, never blocks progress.
 */
void hermit_policy_sort(struct list_head *list, bool lru_gen_reclaim)
{
	struct policy_rank rank[POLICY_SCAN], tmp;
	struct hermit_reclaim_hint hint;
	struct folio *f;
	unsigned int i, j, nr = 0;

	if (!policy_mode() || !hermit_policy_feature(HERMIT_POLICY_SORT))
		return;
	if (hermit_policy_feature(HERMIT_POLICY_MGLRU) &&
	    (lru_gen_reclaim || lru_gen_enabled())) {
		atomic_long_inc(&mglru_sort_bypasses);
		return;
	}
	list_for_each_entry_reverse(f, list, lru) {
		if (nr == POLICY_SCAN)
			break;
		if (!folio_trylock(f))
			continue;
		hermit_policy_hint(f, &hint);
		rank[nr++] = (struct policy_rank) {
			.folio = f, .age = hint.age,
			.score = hint.heat * 2 + folio_test_dirty(f),
		};
		folio_unlock(f);
	}
	if (nr < 2)
		return;
	for (i = 1; i < nr; i++) {
		tmp = rank[i];
		for (j = i; j && policy_rank_before(&tmp, &rank[j - 1]); j--)
			rank[j] = rank[j - 1];
		rank[j] = tmp;
	}
	/* Worst first: each list_move_tail() lands after the previous one. */
	for (i = nr; i--; )
		list_move_tail(&rank[i].folio->lru, list);
	atomic_long_add(nr, &ranked_pages);
	atomic_long_inc(&sorted_lists);
}

/* Drop per-page state for a zapped range (munmap, MADV_DONTNEED, ...).
 * Only this range loses history; other VMAs and PEBS windows are untouched.
 */
void hermit_policy_unmap(struct vm_area_struct *vma, unsigned long start,
		unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	struct hermit_policy_state *st = READ_ONCE(mm->hermit_policy);
	struct policy_page *p, *next;
	unsigned long addr, pages;

	if (!st || !atomic_read(&mm->mm_users))
		return;
	start &= PAGE_MASK;
	pages = (PAGE_ALIGN(end) - start) >> PAGE_SHIFT;
	spin_lock(&st->lock);
	if (pages <= st->nr) {
		for (addr = start; addr < end; addr += PAGE_SIZE) {
			p = policy_lookup(st, addr);
			if (p) {
				policy_free(st, p);
				atomic_long_inc(&unmapped_pages);
			}
		}
	} else {
		list_for_each_entry_safe(p, next, &st->lru, lru)
			if (p->address >= start && p->address < end) {
				policy_free(st, p);
				atomic_long_inc(&unmapped_pages);
			}
	}
	spin_unlock(&st->lock);
}

unsigned long hermit_policy_reserve(struct mem_cgroup *memcg)
{
#ifdef CONFIG_MEMCG
	unsigned long max = READ_ONCE(memcg->memory.max);

	if (!hermit_policy_feature(HERMIT_POLICY_RESERVE) ||
	    max == PAGE_COUNTER_MAX || atomic_read(&memcg->hermit_refault) >= 256 ||
	    atomic64_read(&memcg->hermit_stream_until) < ktime_get_ns())
		return 0;
	return min_t(unsigned long, HERMIT_BATCH_MAX * 2, max / 16);
#else
	return 0;
#endif
}

void hermit_policy_mm_exit(struct mm_struct *mm)
{
	struct hermit_policy_state *st = mm->hermit_policy;
	struct policy_page *p, *next;

	if (!st)
		return;
	mm->hermit_policy = NULL;
	list_for_each_entry_safe(p, next, &st->lru, lru)
		kfree(p);
	kfree(st->pages);
	kfree(st);
}

/* The first seven fields keep their names and order for existing parsers. */
static int policy_stats_show(struct seq_file *m, void *unused)
{
	static const struct { const char *name; atomic_long_t *value; } rows[] = {
		{ "observed_4k", &observed_pages },
		{ "cold_observations", &cold_pages },
		{ "invalid_epochs", &invalid_epochs },
		{ "late_samples", &late_samples },
		{ "predicted_4k", &predicted_pages },
		{ "refault_4k", &refault_pages },
		{ "ranked_candidates", &ranked_pages },
		{ "started_epochs", &started_epochs },
		{ "young_epochs", &young_epochs },
		{ "short_epochs", &short_epochs },
		{ "table_evictions", &evicted_pages },
		{ "unmapped_4k", &unmapped_pages },
		{ "sorted_lists", &sorted_lists },
		{ "large_streams", &large_streams },
		{ "stream_evictions", &stream_evictions },
		{ "batch_overruns", &batch_overruns },
		{ "capped_batches", &capped_batches },
		{ "mglru_candidates", &mglru_candidates },
		{ "mglru_sort_bypasses", &mglru_sort_bypasses },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rows); i++)
		seq_printf(m, "%s %ld\n", rows[i].name, atomic_long_read(rows[i].value));
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
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(policy_stats);
void hermit_policy_debugfs_init(struct dentry *root)
{
	debugfs_create_file("policy_stats", 0400, root, NULL, &policy_stats_fops);
	debugfs_create_x32("policy_features", 0600, root, &policy_features);
	debugfs_create_u32("policy_cold_us", 0600, root, &policy_cold_us);
	debugfs_create_u32("policy_cold_epochs", 0600, root, &policy_cold_epochs);
	debugfs_create_u32("policy_pages_max", 0600, root, &policy_pages_max);
	debugfs_create_u32("policy_swapin_max", 0600, root, &policy_swapin_max);
	debugfs_create_u32("policy_medium_max_order", S_IRUSR | S_IWUSR, root,
			   &policy_medium_max_order);
	debugfs_create_u32("policy_medium_stable", S_IRUSR | S_IWUSR, root,
			   &policy_medium_stable);
}
