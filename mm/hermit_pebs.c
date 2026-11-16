// SPDX-License-Identifier: GPL-2.0
/*
 * Hermit PEBS access-footprint sampling.
 *
 * Per-mm, fault-triggered event-time windows train cross-region fault-PC
 * footprints for the 4 KiB swap-in predictor. Folio allocation and transport
 * splitting remain separate decisions. Tracking works without CONFIG_MEMCG.
 */
#include <linux/kernel.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/hash.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/perf_event.h>
#include <linux/syscalls.h>
#include <linux/hermit_pebs.h>
#include <linux/hermit_policy.h>
#include <linux/hermit_backend.h>
#include <linux/rbtree.h>
#include <linux/ktime.h>

#include "../kernel/events/internal.h"
#include "hermit_pebs_policy.h"

/* ---------- tunables (debugfs) ---------- */
u32 hermit_pebs_enabled;
u32 hermit_pebs_all;
/* Zero selects the existing adaptive period table. Configure while stopped. */
u32 hermit_pebs_load_period;
u32 hermit_pebs_store_period;
u32 hermit_pebs_adaptive = 1;
u32 hermit_pebs_mode = HERMIT_PEBS_MODE_STATIC;
u32 hermit_pebs_fixed_cost_ns = 7000;
u32 hermit_pebs_region_max = HERMIT_PEBS_MAX_REGIONS;
static u32 hermit_pebs_pc_max = 4096;
static u32 hermit_pebs_window_ms = 10;
static u32 hermit_pebs_history_ms = 1000;
/* Epoch invalidation is lazy; no global mm walk or reference pinning. */
u64 hermit_pebs_epoch;
/* Subpage heat: a sample counter per base page, halved every period. */
u32 hermit_pebs_heat_ms = 1000;

/* ---------- sampling period lists (from MEMTIS) ---------- */
#define HERMIT_PCOUNT 30
static const unsigned int hermit_period_list[HERMIT_PCOUNT] = {
	199,  293,  401,  499,	599,   701,   797,   907,   997,   1201,
	1399, 1601, 1801, 1999, 2503,  3001,  3499,  4001,  4507,  4999,
	6007, 7001, 7993, 9001, 10007, 12007, 13999, 16001, 17989, 19997,
};
#define HERMIT_PINSTCOUNT 5
static const unsigned int hermit_inst_period_list[HERMIT_PINSTCOUNT] = {
	100003, 300007, 600011, 1000003, 1500003,
};

struct hermit_pebs_sample {
	struct perf_event_header header;
	__u64 ip;
	__u32 pid, tid;
	__u64 time;
	__u64 addr;
};

static struct task_struct *hermit_pebsd;
static struct perf_event **hermit_pebs_events[HERMIT_PEBS_NR_EVENTS];
static DEFINE_MUTEX(hermit_pebs_mutex);
static unsigned int hermit_period_idx;
static unsigned int hermit_inst_period_idx;

/* global counters */
static atomic_long_t hermit_nr_sampled;
static atomic_long_t hermit_window_samples;
static atomic_long_t hermit_nr_throttled;
static atomic_long_t hermit_nr_lost;

/* ---------- per-mm windows and cross-region fault-PC table ---------- */

struct hermit_pending_window {
	struct rb_node node;
	struct hermit_footprint_window data;
	bool pending;
};

struct hermit_region {
	struct hlist_node hnode;
	struct list_head lru;
	unsigned long vaddr;
	/* One open window and its predecessor's collection grace period. */
	struct hermit_pending_window window[2];
	/* Per-subpage load+store sample counts for the split policy. Allocated
	 * on the first sample once HERMIT_POLICY_SPLIT is on; NULL means the
	 * region has no heat record (unknown, never cold).
	 */
	u8 *heat;
	u64 heat_ns;
};

struct hermit_pc_entry {
	struct hlist_node hnode;
	struct list_head lru;
	unsigned long pc;
	u16 offset;
	u64 epoch;
	struct hermit_pc_history history;
};

struct hermit_pebs_state {
	spinlock_t lock;
	struct hlist_head buckets[HERMIT_PEBS_BUCKETS];
	struct hlist_head pc_buckets[HERMIT_PEBS_BUCKETS];
	struct list_head lru, pc_lru;
	struct rb_root_cached pending;
	unsigned int nr_regions, nr_pcs;
	u64 since_ns;
};

enum hermit_policy_counter {
	HPC_COMPLETED, HPC_EXPIRED, HPC_LATE, HPC_ALLOC_FAIL, HPC_EVICT,
	/* Sample pipeline: tracked mm -> recorded in an open window. */
	HPC_SAMPLE_TARGET, HPC_SAMPLE_WINDOW,
	/* 4 KiB predictor: why PEBS did or did not extend a batch. */
	HPC_PRED_CALLS, HPC_PRED_NO_ENTRY, HPC_PRED_FEW_REGIONS,
	HPC_PRED_FEW_SAMPLES, HPC_PRED_NO_VOTES, HPC_PRED_EXTENDED, HPC_NR,
};
static atomic_long_t hermit_policy_counters[HPC_NR];
static atomic_long_t hermit_actual_orders[HERMIT_PEBS_MAX_ORDER + 1];
static const char * const hermit_policy_names[HPC_NR] = {
	"windows_completed", "history_expired", "samples_late_or_untracked",
	"metadata_alloc_failed", "table_evictions",
	"samples_target_mm", "samples_in_window",
	"predict_calls", "predict_no_entry", "predict_few_regions",
	"predict_few_samples", "predict_no_votes", "predict_extended",
};

static bool hermit_pebs_tracks(struct mm_struct *mm)
{
	return READ_ONCE(hermit_pebs_enabled) &&
		(READ_ONCE(hermit_pebs_all) || READ_ONCE(mm->hermit_pebs_enabled));
}

static u64 hermit_state_epoch(struct hermit_pebs_state *st)
{
	return max(st->since_ns, READ_ONCE(hermit_pebs_epoch));
}

static struct hermit_region *hermit_region_lookup(struct hermit_pebs_state *st,
						  unsigned long vaddr)
{
	struct hermit_region *r;

	hlist_for_each_entry(r, &st->buckets[hash_long(vaddr >> HERMIT_REGION_SHIFT, 8)], hnode)
		if (r->vaddr == vaddr)
			return r;
	return NULL;
}

/* Callers hold mm_users, including sampler task lookup and the fault path. */
static struct hermit_pebs_state *hermit_pebs_state_get(struct mm_struct *mm)
{
	struct hermit_pebs_state *st = READ_ONCE(mm->hermit_pebs);
	struct hermit_pebs_state *new;

	/* VMA edits, zaps and fork no longer reset the whole history: PEBS
	 * windows age out by TTL and per-page policy state is dropped per range.
	 */
	if (st)
		return st;
	/* May be called with a PTE lock held. No reclaim or sleeping. */
	new = kzalloc(sizeof(*new), GFP_NOWAIT | __GFP_NOWARN);
	if (!new) {
		atomic_long_inc(&hermit_policy_counters[HPC_ALLOC_FAIL]);
		return NULL;
	}
	spin_lock_init(&new->lock);
	INIT_LIST_HEAD(&new->lru);
	INIT_LIST_HEAD(&new->pc_lru);
	new->pending = RB_ROOT_CACHED;
	st = cmpxchg(&mm->hermit_pebs, NULL, new);
	if (st) {
		kfree(new);
		return st;
	}
	return new;
}

static struct hermit_pc_entry *hermit_pc_get(struct hermit_pebs_state *st,
		unsigned long pc, unsigned int offset, bool create)
{
	unsigned int hash = hash_long(pc ^ (offset << 4), 8);
	struct hermit_pc_entry *p;

	hlist_for_each_entry(p, &st->pc_buckets[hash], hnode) {
		if (p->pc != pc || p->offset != offset)
			continue;
		if (p->epoch != hermit_state_epoch(st)) {
			memset(&p->history, 0, sizeof(p->history));
			p->epoch = hermit_state_epoch(st);
		}
		list_move(&p->lru, &st->pc_lru);
		return p;
	}
	if (!create)
		return NULL;
	if (st->nr_pcs >= max_t(u32, 1, READ_ONCE(hermit_pebs_pc_max))) {
		p = list_last_entry(&st->pc_lru, struct hermit_pc_entry, lru);
		hlist_del(&p->hnode);
		list_del(&p->lru);
		atomic_long_inc(&hermit_policy_counters[HPC_EVICT]);
	} else {
		p = kzalloc(sizeof(*p), GFP_NOWAIT | __GFP_NOWARN);
		if (!p) {
			atomic_long_inc(&hermit_policy_counters[HPC_ALLOC_FAIL]);
			return NULL;
		}
		st->nr_pcs++;
	}
	memset(p, 0, sizeof(*p));
	p->pc = pc;
	p->offset = offset;
	p->epoch = hermit_state_epoch(st);
	hlist_add_head(&p->hnode, &st->pc_buckets[hash]);
	list_add(&p->lru, &st->pc_lru);
	return p;
}

static void hermit_window_remove(struct hermit_pebs_state *st,
				 struct hermit_pending_window *w)
{
	if (w->pending) {
		rb_erase_cached(&w->node, &st->pending);
		w->pending = false;
	}
}

static void hermit_window_queue(struct hermit_pebs_state *st,
				struct hermit_pending_window *w)
{
	struct rb_node **link = &st->pending.rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	while (*link) {
		struct hermit_pending_window *other;

		parent = *link;
		other = rb_entry(parent, struct hermit_pending_window, node);
		if (w->data.close_ns < other->data.close_ns) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&w->node, parent, link);
	rb_insert_color_cached(&w->node, &st->pending, leftmost);
	w->pending = true;
}

/* Lazy completion before a lookup; never rewrite an already published window.
 * Bound fault-path work. A backlog can only postpone learning, not enlarge an
 * untrained read. Further faults/samples drain the remaining completed windows.
 */
static void hermit_windows_finish(struct hermit_pebs_state *st, u64 now)
{
	struct rb_node *node;
	unsigned int budget = 64;

	while (budget-- && (node = rb_first_cached(&st->pending))) {
		struct hermit_pending_window *w;
		struct hermit_pc_entry *p;

		w = rb_entry(node, struct hermit_pending_window, node);
		if (w->data.close_ns > now)
			break;
		hermit_window_remove(st, w);
		if (w->data.epoch != hermit_state_epoch(st) ||
		    now - w->data.end_ns >=
			    (u64)READ_ONCE(hermit_pebs_history_ms) * NSEC_PER_MSEC) {
			atomic_long_inc(&hermit_policy_counters[HPC_EXPIRED]);
			continue;
		}
		p = hermit_pc_get(st, w->data.pc, w->data.offset, true);
		if (p)
			hermit_history_add(&p->history, &w->data);
		atomic_long_inc(&hermit_policy_counters[HPC_COMPLETED]);
	}
}

/* hermit_heat:begin */
/*
 * Decayed per-subpage sample counters: one saturating u8 per base page of a
 * 2 MiB region, halved once per period so that only the last few seconds of
 * accesses matter. @stamp is the last decay instant; a long idle gap collapses
 * into one shift of at most 8 bits.
 */
static void hermit_heat_decay(u8 *heat, u64 *stamp, u64 now, u64 period)
{
	unsigned int i, shift;

	if (!period)
		return;
	if (now <= *stamp) {
		if (now < *stamp)
			*stamp = now;
		return;
	}
	shift = div_u64(now - *stamp, period);
	if (!shift)
		return;
	*stamp += (u64)shift * period;
	if (shift >= 8)
		memset(heat, 0, HERMIT_REGION_NR_SUBPAGES);
	else
		for (i = 0; i < HERMIT_REGION_NR_SUBPAGES; i++)
			heat[i] >>= shift;
}

static void hermit_heat_add(u8 *heat, u64 *stamp, unsigned int bit, u64 now,
			    u64 period)
{
	hermit_heat_decay(heat, stamp, now, period);
	if (bit < HERMIT_REGION_NR_SUBPAGES && heat[bit] != U8_MAX)
		heat[bit]++;
}
/* hermit_heat:end */

static u64 hermit_heat_period(void)
{
	return (u64)READ_ONCE(hermit_pebs_heat_ms) * NSEC_PER_MSEC;
}

/* Allocate the heat record lazily; GFP_NOWAIT, so the sample path never sleeps. */
static u8 *hermit_region_heat(struct hermit_region *r, u64 now)
{
	if (!r->heat) {
		r->heat = kzalloc(HERMIT_REGION_NR_SUBPAGES,
				  GFP_NOWAIT | __GFP_NOWARN);
		if (!r->heat) {
			atomic_long_inc(&hermit_policy_counters[HPC_ALLOC_FAIL]);
			return NULL;
		}
		r->heat_ns = now;
	}
	return r->heat;
}

static struct hermit_region *hermit_region_get(struct hermit_pebs_state *st,
					      unsigned long addr)
{
	unsigned long vaddr = addr & ~((1UL << HERMIT_REGION_SHIFT) - 1);
	struct hermit_region *r = hermit_region_lookup(st, vaddr);
	u8 *heat;
	unsigned int i;

	if (r) {
		list_move(&r->lru, &st->lru);
		return r;
	}
	if (st->nr_regions >= max_t(u32, 1, READ_ONCE(hermit_pebs_region_max))) {
		r = list_last_entry(&st->lru, struct hermit_region, lru);
		for (i = 0; i < ARRAY_SIZE(r->window); i++)
			hermit_window_remove(st, &r->window[i]);
		hlist_del(&r->hnode);
		list_del(&r->lru);
		atomic_long_inc(&hermit_policy_counters[HPC_EVICT]);
		heat = r->heat;
	} else {
		r = kzalloc(sizeof(*r), GFP_NOWAIT | __GFP_NOWARN);
		if (!r) {
			atomic_long_inc(&hermit_policy_counters[HPC_ALLOC_FAIL]);
			return NULL;
		}
		st->nr_regions++;
		heat = NULL;
	}
	memset(r, 0, sizeof(*r));
	/* Reuse the evicted region's heat record instead of reallocating it. */
	r->heat = heat;
	if (heat)
		memset(heat, 0, HERMIT_REGION_NR_SUBPAGES);
	r->vaddr = vaddr;
	hlist_add_head(&r->hnode, &st->buckets[hash_long(vaddr >> HERMIT_REGION_SHIFT, 8)]);
	list_add(&r->lru, &st->lru);
	return r;
}

/* Called only after the fault installs its PTEs. time is fault entry time. */
void hermit_pebs_fault(struct mm_struct *mm, unsigned long addr,
		       unsigned long pc, u64 time)
{
	struct hermit_pebs_state *st;
	struct hermit_region *r;
	struct hermit_pending_window *free = NULL;
	u64 now, duration;
	unsigned int i, bit = hermit_page_offset(addr);

	if (!pc || !hermit_pebs_tracks(mm))
		return;
	duration = (u64)READ_ONCE(hermit_pebs_window_ms) * NSEC_PER_MSEC;
	if (!duration)
		return;
	now = ktime_get_ns();
	st = hermit_pebs_state_get(mm);
	if (!st)
		return;
	spin_lock(&st->lock);
	hermit_windows_finish(st, now);
	/* Do not train on an operation that straddled enable/start or took
	 * longer than the collection grace interval.
	 */
	if (time < hermit_state_epoch(st) || now - time >= 2 * duration)
		goto out;
	r = hermit_region_get(st, addr);
	if (!r)
		goto out;
	for (i = 0; i < ARRAY_SIZE(r->window); i++) {
		struct hermit_pending_window *w = &r->window[i];

		if (w->data.epoch != hermit_state_epoch(st)) {
			hermit_window_remove(st, w);
			memset(&w->data, 0, sizeof(w->data));
		}
		if (hermit_window_contains(&w->data, time)) {
			if (w->pending && now < w->data.close_ns)
				w->data.load[bit / 64] |= 1ULL << (bit % 64);
			goto out;
		}
		/* Out-of-order fault completion must not create overlapping windows. */
		if (time < w->data.end_ns)
			goto out;
		if (!w->pending)
			free = w;
	}
	if (free) {
		hermit_window_start(&free->data, addr, pc, time, duration,
				    hermit_state_epoch(st));
		hermit_window_queue(st, free);
	}
out:
	spin_unlock(&st->lock);
}

static void hermit_region_sample(struct mm_struct *mm, unsigned long addr,
				u64 time, bool store)
{
	struct hermit_pebs_state *st = READ_ONCE(mm->hermit_pebs);
	struct hermit_region *r;
	u64 now = ktime_get_ns();
	unsigned int i;
	bool recorded = false;
	bool heat_on;

	if (!st)
		return;
	spin_lock(&st->lock);
	hermit_windows_finish(st, now);
	/* Only the split policy consumes heat, so SPLIT off keeps the table
	 * exactly as the 4 KiB predictor left it.
	 */
	heat_on = hermit_policy_feature(HERMIT_POLICY_SPLIT);
	r = hermit_region_lookup(st, addr & ~((1UL << HERMIT_REGION_SHIFT) - 1));
	if (!r && heat_on)
		r = hermit_region_get(st, addr);
	if (r) {
		u8 *heat = heat_on ? hermit_region_heat(r, now) : NULL;

		/* Load and store misses both count as an access to the page. */
		if (heat)
			hermit_heat_add(heat, &r->heat_ns, hermit_page_offset(addr),
					now, hermit_heat_period());
		for (i = 0; i < ARRAY_SIZE(r->window); i++) {
			struct hermit_pending_window *w = &r->window[i];

			if (w->pending && w->data.epoch == hermit_state_epoch(st) &&
			    hermit_window_sample(&w->data, addr, time, now, store)) {
				recorded = true;
				break;
			}
		}
	}
	atomic_long_inc(&hermit_policy_counters[recorded ? HPC_SAMPLE_WINDOW : HPC_LATE]);
	spin_unlock(&st->lock);
}

bool hermit_pebs_snapshot(struct mm_struct *mm, unsigned long addr,
			  struct hermit_pebs_observation *out)
{
	struct hermit_pebs_state *st = READ_ONCE(mm->hermit_pebs);
	struct hermit_region *r;
	u64 now = ktime_get_ns();
	unsigned int i;
	bool found = false;

	memset(out, 0, sizeof(*out));
	if (!st || !hermit_pebs_tracks(mm))
		return false;
	spin_lock(&st->lock);
	r = hermit_region_lookup(st, addr & ~((1UL << HERMIT_REGION_SHIFT) - 1));
	if (r) {
		for (i = 0; i < ARRAY_SIZE(r->window); i++) {
			struct hermit_footprint_window *w = &r->window[i].data;

			if (w->epoch != hermit_state_epoch(st) || !w->end_ns ||
			    now >= w->close_ns || (found && w->start_ns <= out->start_ns))
				continue;
			out->start_ns = w->start_ns;
			out->end_ns = w->end_ns;
			memcpy(out->load, w->load, sizeof(out->load));
			memcpy(out->store, w->store, sizeof(out->store));
			found = true;
		}
	}
	spin_unlock(&st->lock);
	return found;
}

bool hermit_pebs_heat_read(struct mm_struct *mm, unsigned long addr,
			   unsigned int nr_pages, u8 *out)
{
	struct hermit_pebs_state *st = READ_ONCE(mm->hermit_pebs);
	struct hermit_region *r;
	unsigned int offset = hermit_page_offset(addr);
	u64 now;
	bool found = false;

	if (!st || !nr_pages || offset + nr_pages > HERMIT_REGION_NR_SUBPAGES ||
	    offset + nr_pages < offset)
		return false;
	now = ktime_get_ns();
	spin_lock(&st->lock);
	r = hermit_region_lookup(st, addr & ~((1UL << HERMIT_REGION_SHIFT) - 1));
	if (r && r->heat) {
		hermit_heat_decay(r->heat, &r->heat_ns, now, hermit_heat_period());
		memcpy(out, r->heat + offset, nr_pages);
		found = true;
	}
	spin_unlock(&st->lock);
	return found;
}

/* ---------- sampling ---------- */

static bool hermit_valid_va(u64 addr)
{
	return addr != 0 && !(addr >> (PGDIR_SHIFT + 9));
}

static void hermit_pebs_record(struct hermit_pebs_sample *s, bool store)
{
	struct task_struct *task;
	struct mm_struct *mm;
	bool enabled;

	if (!hermit_valid_va(s->addr))
		return;

	atomic_long_inc(&hermit_nr_sampled);
	atomic_long_inc(&hermit_window_samples);

	rcu_read_lock();
	task = find_task_by_vpid(s->tid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return;

	/* A delayed ring record must not attach to a recycled TID. */
	if (s->time < task->start_time) {
		put_task_struct(task);
		return;
	}
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return;

	enabled = READ_ONCE(mm->hermit_pebs_enabled) ||
		  READ_ONCE(hermit_pebs_all);
	if (enabled) {
		atomic_long_inc(&hermit_policy_counters[HPC_SAMPLE_TARGET]);
		hermit_region_sample(mm, s->addr, s->time, store);
		hermit_policy_sample(mm, s->addr, s->time);
	}
	mmput(mm);
}

/* Records may straddle both allocation pages and the end of the ring. */
static void hermit_ring_copy(struct perf_buffer *rb, u64 pos, void *dst,
			     size_t len)
{
	unsigned int shift = PAGE_SHIFT + page_order(rb);
	size_t page_size = 1UL << shift;

	while (len) {
		size_t off = pos & (page_size - 1);
		size_t n = min(len, page_size - off);
		unsigned int pg = (pos >> shift) & (rb->nr_pages - 1);

		memcpy(dst, rb->data_pages[pg] + off, n);
		dst += n;
		pos += n;
		len -= n;
	}
}

static int hermit_pebs_read_events(void)
{
	int cpu, e;
	unsigned long count = 0;

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		if (!hermit_pebs_events[e])
			continue;
		for_each_possible_cpu(cpu) {
			struct perf_event *event = hermit_pebs_events[e][cpu];
			struct perf_buffer *rb;
			struct perf_event_mmap_page *up;
			u64 head, tail;
			unsigned int n = 0;

			if (!event || !event->rb)
				continue;
			rb = event->rb;
			up = rb->user_page;
			head = smp_load_acquire(&up->data_head);
			tail = READ_ONCE(up->data_tail);
			if (head - tail > perf_data_size(rb)) {
				atomic_long_inc(&hermit_nr_lost);
				tail = head;
			}
			while (head != tail && n++ < 512) {
				struct perf_event_header ph;

				if (head - tail < sizeof(ph))
					break;
				hermit_ring_copy(rb, tail, &ph, sizeof(ph));
				if (ph.size < sizeof(ph) ||
				    ph.size > perf_data_size(rb)) {
					atomic_long_inc(&hermit_nr_lost);
					tail = head;
					break;
				}
				if (ph.size > head - tail)
					break;
				if (ph.type == PERF_RECORD_SAMPLE &&
				    ph.size >=
					    sizeof(struct hermit_pebs_sample)) {
					struct hermit_pebs_sample sample;

					hermit_ring_copy(rb, tail, &sample,
							 sizeof(sample));
					hermit_pebs_record(&sample, e == 1);
					count++;
				} else if (ph.type == PERF_RECORD_THROTTLE ||
					   ph.type == PERF_RECORD_UNTHROTTLE) {
					atomic_long_inc(&hermit_nr_throttled);
				} else if (ph.type == PERF_RECORD_LOST) {
					atomic_long_inc(&hermit_nr_lost);
				}
				tail += ph.size;
			}
			/* Finish copying before allowing the producer to reuse pages. */
			smp_store_release(&up->data_tail, tail);
		}
	}
	return count;
}

static void hermit_pebs_update_periods(void)
{
	int cpu, e;
	u64 period, inst_period;

	period = READ_ONCE(hermit_pebs_load_period) ?:
			 hermit_period_list[hermit_period_idx];
	inst_period = READ_ONCE(hermit_pebs_store_period) ?:
			      hermit_inst_period_list[hermit_inst_period_idx];

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		if (!hermit_pebs_events[e])
			continue;
		for_each_online_cpu(cpu) {
			struct perf_event *event = hermit_pebs_events[e][cpu];

			if (!event)
				continue;
			hermit_perf_event_set_period(
				event, e == 0 ? period : inst_period);
		}
	}
}

static void hermit_pebs_adapt(unsigned long window_samples)
{
	if (!READ_ONCE(hermit_pebs_adaptive))
		return;
	if (window_samples > 50000 && hermit_period_idx < HERMIT_PCOUNT - 1) {
		hermit_period_idx++;
		hermit_inst_period_idx = min_t(unsigned int,
					       hermit_inst_period_idx + 1,
					       HERMIT_PINSTCOUNT - 1);
		hermit_pebs_update_periods();
	} else if (window_samples < 1000 && hermit_period_idx > 0) {
		hermit_period_idx--;
		if (hermit_inst_period_idx > 0)
			hermit_inst_period_idx--;
		hermit_pebs_update_periods();
	}
}

static int hermit_pebsd_main(void *arg)
{
	unsigned long last = jiffies;
	const unsigned long window = msecs_to_jiffies(2000);

	while (!kthread_should_stop()) {
		hermit_pebs_read_events();
		schedule_timeout_interruptible(usecs_to_jiffies(2000));
		if (time_after(jiffies, last + window)) {
			unsigned long samples =
				atomic_long_xchg(&hermit_window_samples, 0);

			last = jiffies;
			hermit_pebs_adapt(samples);
		}
	}
	return 0;
}

static void hermit_pebs_close_events(void)
{
	int cpu, e;

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		if (!hermit_pebs_events[e])
			continue;
		for_each_possible_cpu(cpu) {
			if (hermit_pebs_events[e][cpu]) {
				perf_event_disable(hermit_pebs_events[e][cpu]);
				hermit_perf_event_release(
					hermit_pebs_events[e][cpu]);
				hermit_pebs_events[e][cpu] = NULL;
			}
		}
		kfree(hermit_pebs_events[e]);
		hermit_pebs_events[e] = NULL;
	}
}

static int hermit_pebs_open_events(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_RAW,
		.size = sizeof(attr),
		.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
			       PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR,
		.use_clockid = 1,
		.clockid = CLOCK_MONOTONIC,
		.exclude_kernel = 1,
		.exclude_hv = 1,
		.exclude_callchain_kernel = 1,
		.exclude_callchain_user = 1,
		.precise_ip = 1,
		.disabled = 1,
	};
	int cpu, e, err = 0;

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		hermit_pebs_events[e] = kcalloc(
			nr_cpu_ids, sizeof(struct perf_event *), GFP_KERNEL);
		if (!hermit_pebs_events[e]) {
			err = -ENOMEM;
			goto out;
		}
	}

	attr.config = HERMIT_PEBS_L3_MISS;
	attr.sample_period = READ_ONCE(hermit_pebs_load_period) ?:
				     hermit_period_list[0];
	for_each_online_cpu(cpu) {
		struct perf_event *event;

		event = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL,
							 NULL);
		if (IS_ERR(event)) {
			err = PTR_ERR(event);
			goto out;
		}
		err = hermit_perf_event_init(event, HERMIT_PEBS_RING_PAGES);
		if (err) {
			perf_event_release_kernel(event);
			goto out;
		}
		hermit_pebs_events[0][cpu] = event;
	}

	attr.config = HERMIT_PEBS_ALL_STORES;
	attr.sample_period = READ_ONCE(hermit_pebs_store_period) ?:
				     hermit_inst_period_list[0];
	for_each_online_cpu(cpu) {
		struct perf_event *event;

		event = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL,
							 NULL);
		if (IS_ERR(event)) {
			err = PTR_ERR(event);
			goto out;
		}
		err = hermit_perf_event_init(event, HERMIT_PEBS_RING_PAGES);
		if (err) {
			perf_event_release_kernel(event);
			goto out;
		}
		hermit_pebs_events[1][cpu] = event;
	}
	/* Start only after every event has a fully initialized output ring. */
	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++)
		for_each_online_cpu(cpu)
			perf_event_enable(hermit_pebs_events[e][cpu]);
out:
	if (err)
		hermit_pebs_close_events();
	return err;
}

static struct mm_struct *hermit_pebs_get_mm(pid_t pid)
{
	struct task_struct *task;
	struct mm_struct *mm;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return NULL;
	mm = get_task_mm(task);
	put_task_struct(task);
	return mm;
}

int hermit_pebs_start_sampling(pid_t pid, int mode)
{
	struct task_struct *t;
	struct mm_struct *target_mm = NULL;
	int ret;

	mutex_lock(&hermit_pebs_mutex);

	/* Sampling is always global (per-CPU events); pid only selects the
	 * address space whose history should be recorded. This keeps the
	 * policy independent of cgroup membership: pebs_all opts every mm
	 * in, and passing a pid opts that mm in. When sampling is already
	 * running, the same call is just an enable operation.
	 */
	if (pid != -1) {
		target_mm = hermit_pebs_get_mm(pid);
		if (!target_mm) {
			ret = -ESRCH;
			goto out_unlock;
		}
	}
	if (hermit_pebsd) {
		if (mode != -1)
			WRITE_ONCE(hermit_pebs_mode, mode);
		WRITE_ONCE(hermit_pebs_enabled, 1);
		if (target_mm) {
			hermit_pebs_mm_enable(target_mm, true);
			mmput(target_mm);
		}
		mutex_unlock(&hermit_pebs_mutex);
		return 0;
	}

	WRITE_ONCE(hermit_pebs_epoch, ktime_get_ns());
	hermit_period_idx = 0;
	hermit_inst_period_idx = 0;
	atomic_long_set(&hermit_window_samples, 0);
	cpus_read_lock();
	ret = hermit_pebs_open_events();
	cpus_read_unlock();
	if (ret)
		goto out_mmput;
	t = kthread_run(hermit_pebsd_main, NULL, "hermit_pebsd");
	if (IS_ERR(t)) {
		hermit_pebs_close_events();
		ret = PTR_ERR(t);
		goto out_mmput;
	}
	hermit_pebsd = t;
	if (mode != -1)
		WRITE_ONCE(hermit_pebs_mode, mode);
	WRITE_ONCE(hermit_pebs_enabled, 1);
	if (target_mm) {
		hermit_pebs_mm_enable(target_mm, true);
		mmput(target_mm);
		target_mm = NULL;
	}
	mutex_unlock(&hermit_pebs_mutex);
	return 0;

out_mmput:
	if (target_mm)
		mmput(target_mm);
out_unlock:
	mutex_unlock(&hermit_pebs_mutex);
	return ret;
}

void hermit_pebs_stop_sampling(void)
{
	mutex_lock(&hermit_pebs_mutex);
	if (hermit_pebsd) {
		kthread_stop(hermit_pebsd);
		hermit_pebsd = NULL;
	}
	hermit_pebs_close_events();
	WRITE_ONCE(hermit_pebs_enabled, 0);
	WRITE_ONCE(hermit_pebs_epoch, ktime_get_ns());
	mutex_unlock(&hermit_pebs_mutex);
}

/* ---------- base-page swap-in prediction ---------- */

/* Predict individual 4 KiB pages from repeated load footprints. An extent
 * length is never evidence. Return only the contiguous, independently supported
 * prefix; transport may still split it for remote layout or SGE constraints.
 */
/* Each extra page needs a 75% vote across distinct regions; sparse
 * evidence only shortens the suggested batch.
 */
static u32 hermit_pebs_predict_regions = 2;
static u32 hermit_pebs_predict_samples = 8;

unsigned int hermit_pebs_predict_pages(struct vm_fault *vmf)
{
	struct hermit_pebs_state *st;
	struct hermit_pc_entry *p;
	u64 ttl = (u64)READ_ONCE(hermit_pebs_history_ms) * NSEC_PER_MSEC;
	unsigned int i, j, regions = 0, loads = 0, votes[HERMIT_SWAPIN_MAX] = {};
	unsigned int offset = hermit_page_offset(vmf->address), nr = 1;
	unsigned int reason = HPC_PRED_NO_ENTRY;
	unsigned long seen = 0;
	u64 now = ktime_get_ns();

	if (!hermit_policy_active(vmf->vma->vm_mm) || !vmf->hermit_fault_pc)
		return 1;
	st = hermit_pebs_state_get(vmf->vma->vm_mm);
	if (!st)
		return 1;
	atomic_long_inc(&hermit_policy_counters[HPC_PRED_CALLS]);
	spin_lock(&st->lock);
	hermit_windows_finish(st, now);
	p = hermit_pc_get(st, vmf->hermit_fault_pc, offset, false);
	if (!p)
		goto out;
	for (i = 0; i < HERMIT_PC_HISTORY; i++) {
		struct hermit_footprint *f = &p->history.slot[i];

		if (!f->end_ns || now < f->end_ns || now - f->end_ns >= ttl)
			continue;
		/* Repeated windows of one region are one piece of evidence. */
		for (j = 0; j < i; j++)
			if ((seen & BIT(j)) && p->history.slot[j].region == f->region)
				break;
		if (j < i)
			continue;
		seen |= BIT(i);
		regions++;
		loads += f->loads;
		for (j = 1; j < HERMIT_SWAPIN_MAX && offset + j < HERMIT_REGION_NR_SUBPAGES; j++)
			if (f->load[(offset + j) / 64] & BIT_ULL((offset + j) % 64))
				votes[j]++;
	}
	if (regions < max_t(u32, 1, READ_ONCE(hermit_pebs_predict_regions))) {
		reason = HPC_PRED_FEW_REGIONS;
	} else if (loads < READ_ONCE(hermit_pebs_predict_samples)) {
		reason = HPC_PRED_FEW_SAMPLES;
	} else {
		while (nr < HERMIT_SWAPIN_MAX && offset + nr < HERMIT_REGION_NR_SUBPAGES &&
		       votes[nr] * 4 >= regions * 3)
			nr++;
		reason = nr > 1 ? HPC_PRED_EXTENDED : HPC_PRED_NO_VOTES;
	}
out:
	spin_unlock(&st->lock);
	atomic_long_inc(&hermit_policy_counters[reason]);
	return nr;
}

/* Count successfully mapped folios independently of the prediction path. */
void hermit_pebs_account_swapin(struct mm_struct *mm, unsigned int order)
{
	if (order <= HERMIT_PEBS_MAX_ORDER && hermit_pebs_tracks(mm))
		atomic_long_inc(&hermit_actual_orders[order]);
}

/* ---------- syscalls ---------- */

SYSCALL_DEFINE2(hermit_pebs_start, pid_t, pid, int, mode)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (mode != -1 && mode != HERMIT_PEBS_MODE_STATIC &&
	    mode != HERMIT_PEBS_MODE_POLICY)
		return -EINVAL;
	return hermit_pebs_start_sampling(pid, mode);
}

SYSCALL_DEFINE1(hermit_pebs_end, pid_t, pid)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	hermit_pebs_stop_sampling();
	return 0;
}

/* ---------- mm history lifetime ---------- */

void hermit_pebs_mm_exit(struct mm_struct *mm)
{
	struct hermit_pebs_state *st = mm->hermit_pebs;
	struct hermit_region *r, *tmp;
	struct hermit_pc_entry *p, *next;

	if (!st)
		return;
	mm->hermit_pebs = NULL;
	list_for_each_entry_safe(r, tmp, &st->lru, lru) {
		kfree(r->heat);
		kfree(r);
	}
	list_for_each_entry_safe(p, next, &st->pc_lru, lru)
		kfree(p);
	kfree(st);
}

void hermit_pebs_mm_enable(struct mm_struct *mm, bool enable)
{
	struct hermit_pebs_state *st;

	if (!mm)
		return;
	st = READ_ONCE(mm->hermit_pebs);
	if (st) {
		spin_lock(&st->lock);
		st->since_ns = ktime_get_ns();
		spin_unlock(&st->lock);
	}
	/* Enabling is a new session for per-page policy state too. */
	atomic64_inc(&mm->hermit_vma_generation);
	WRITE_ONCE(mm->hermit_pebs_enabled, enable);
}

/* ---------- debugfs ---------- */

static int hermit_pebs_stats_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < HPC_NR; i++)
		seq_printf(m, "%s: %ld\n", hermit_policy_names[i],
			   atomic_long_read(&hermit_policy_counters[i]));
	for (i = 0; i <= HERMIT_PEBS_MAX_ORDER; i++)
		seq_printf(m, "actual_order_%d: %ld\n", i,
			   atomic_long_read(&hermit_actual_orders[i]));
	seq_printf(m, "sampled: %ld\n", atomic_long_read(&hermit_nr_sampled));
	seq_printf(m, "throttled: %ld\n",
		   atomic_long_read(&hermit_nr_throttled));
	seq_printf(m, "lost: %ld\n", atomic_long_read(&hermit_nr_lost));
	seq_printf(m, "period_idx: %u inst_period_idx: %u\n", hermit_period_idx,
		   hermit_inst_period_idx);
	seq_printf(m, "adaptive: %u load_period: %u store_period: %u\n",
		   READ_ONCE(hermit_pebs_adaptive),
		   READ_ONCE(hermit_pebs_load_period) ?:
			   hermit_period_list[hermit_period_idx],
		   READ_ONCE(hermit_pebs_store_period) ?:
			   hermit_inst_period_list[hermit_inst_period_idx]);
	return 0;
}

static int hermit_pebs_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, hermit_pebs_stats_show, NULL);
}

static const struct file_operations hermit_pebs_stats_fops = {
	.open = hermit_pebs_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int hermit_ring_test_show(struct seq_file *m, void *unused)
{
	struct perf_buffer *rb;
	struct perf_event *event;
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_CPU_CLOCK,
		.disabled = 1,
		.sample_period = 100003,
		.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
			       PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR,
		.use_clockid = 1,
		.clockid = CLOCK_MONOTONIC,
	};
	u8 result[sizeof(struct hermit_pebs_sample)];
	unsigned int i, j;
	bool pass = true;
	const u64 starts[] = { 0, PAGE_SIZE - 8, 2 * PAGE_SIZE - 8 };

	rb = kzalloc(sizeof(*rb) + 2 * sizeof(void *), GFP_KERNEL);
	if (!rb)
		return -ENOMEM;
	rb->nr_pages = 2;
	for (i = 0; i < 2; i++) {
		rb->data_pages[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!rb->data_pages[i]) {
			while (i--)
				kfree(rb->data_pages[i]);
			kfree(rb);
			return -ENOMEM;
		}
		for (j = 0; j < PAGE_SIZE; j++)
			((u8 *)rb->data_pages[i])[j] = (u8)(j + 31 * i);
	}
	for (i = 0; i < ARRAY_SIZE(starts); i++) {
		hermit_ring_copy(rb, starts[i], result, sizeof(result));
		for (j = 0; j < sizeof(result); j++) {
			u64 pos = (starts[i] + j) % (2 * PAGE_SIZE);
			u8 expected =
				(u8)(pos % PAGE_SIZE + 31 * (pos / PAGE_SIZE));

			if (result[j] != expected)
				pass = false;
		}
	}
	for (i = 0; i < 2; i++)
		kfree(rb->data_pages[i]);
	kfree(rb);
	/* Exercise real perf allocation/detach without requiring a PEBS PMU. */
	for (i = 0; i < 2; i++) {
		int ret;

		event = perf_event_create_kernel_counter(&attr, -1, current,
							 NULL, NULL);
		if (IS_ERR(event))
			return PTR_ERR(event);
		ret = hermit_perf_event_init(event, 2);
		if (ret) {
			perf_event_release_kernel(event);
			return ret;
		}
		/* The production parser requires a complete timestamped sample. */
		if (sizeof(struct perf_event_header) + event->header_size +
				    event->id_header_size !=
			    sizeof(struct hermit_pebs_sample) ||
		    READ_ONCE(event->state) != PERF_EVENT_STATE_OFF)
			pass = false;
		hermit_perf_event_release(event);
	}
	seq_printf(m, "%s\n", pass ? "pass" : "fail");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hermit_ring_test);

void hermit_pebs_debugfs_init(struct dentry *root)
{
	debugfs_create_u32("pebs_window_ms", 0600, root, &hermit_pebs_window_ms);
	debugfs_create_u32("pebs_history_ms", 0600, root, &hermit_pebs_history_ms);
	debugfs_create_u32("pebs_predict_regions", 0600, root, &hermit_pebs_predict_regions);
	debugfs_create_u32("pebs_predict_samples", 0600, root, &hermit_pebs_predict_samples);
	debugfs_create_u32("pebs_pc_max", 0600, root, &hermit_pebs_pc_max);
	debugfs_create_file("pebs_ring_test", S_IRUSR, root, NULL,
			    &hermit_ring_test_fops);
	debugfs_create_u32("pebs_load_period", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_load_period);
	debugfs_create_u32("pebs_store_period", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_store_period);
	debugfs_create_u32("pebs_adaptive", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_adaptive);
	debugfs_create_u32("pebs_enabled", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_enabled);
	debugfs_create_u32("pebs_all", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_all);
	debugfs_create_u32("pebs_mode", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_mode);
	debugfs_create_u32("pebs_fixed_cost_ns", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_fixed_cost_ns);
	debugfs_create_u32("pebs_region_max", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_region_max);
	debugfs_create_u32("pebs_heat_ms", S_IRUSR | S_IWUSR, root,
			   &hermit_pebs_heat_ms);
	debugfs_create_file("pebs_order_stats", S_IRUSR, root, NULL,
			    &hermit_pebs_stats_fops);
}
