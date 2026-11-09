/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_HERMIT_PEBS_POLICY_H
#define _MM_HERMIT_PEBS_POLICY_H

#include <linux/hermit_pebs.h>
#include <linux/string.h>

#define HERMIT_PC_HISTORY 8

/* Half-open event-time window; collection may continue until close_ns. */
struct hermit_footprint_window {
	u64 start_ns, end_ns, close_ns, epoch;
	unsigned long pc, region;
	u32 loads;
	u16 offset;
	u64 load[HERMIT_REGION_WORDS], store[HERMIT_REGION_WORDS];
};

struct hermit_footprint {
	u64 end_ns;
	unsigned long region;
	u32 loads;
	u64 load[HERMIT_REGION_WORDS];
};

struct hermit_pc_history {
	struct hermit_footprint slot[HERMIT_PC_HISTORY];
	unsigned int next;
};

static inline unsigned int hermit_page_offset(unsigned long addr)
{
	return (addr >> PAGE_SHIFT) & (HERMIT_REGION_NR_SUBPAGES - 1);
}

static inline bool hermit_window_contains(const struct hermit_footprint_window *w,
					 u64 time)
{
	return time >= w->start_ns && time < w->end_ns;
}

static inline void hermit_window_start(struct hermit_footprint_window *w,
		unsigned long addr, unsigned long pc, u64 time, u64 duration, u64 epoch)
{
	memset(w, 0, sizeof(*w));
	w->start_ns = time;
	w->end_ns = time + duration;
	w->close_ns = w->end_ns + duration;
	w->epoch = epoch;
	w->pc = pc;
	w->region = addr >> HERMIT_REGION_SHIFT;
	w->offset = hermit_page_offset(addr);
	w->load[w->offset / 64] |= 1ULL << (w->offset % 64);
}

static inline bool hermit_window_sample(struct hermit_footprint_window *w,
		unsigned long addr, u64 time, u64 now, bool store)
{
	unsigned int bit = hermit_page_offset(addr);
	u64 *bits = store ? w->store : w->load;

	if (!hermit_window_contains(w, time) || now >= w->close_ns)
		return false;
	bits[bit / 64] |= 1ULL << (bit % 64);
	if (!store && w->loads != U32_MAX)
		w->loads++;
	return true;
}

static inline void hermit_history_add(struct hermit_pc_history *h,
				     const struct hermit_footprint_window *w)
{
	struct hermit_footprint *f = &h->slot[h->next];

	/* A window with no PEBS load is not evidence; it must not push out a
	 * sampled one. Demand bits alone are already known to the fault path.
	 */
	if (!w->loads)
		return;
	f->end_ns = w->end_ns;
	f->region = w->region;
	f->loads = w->loads;
	memcpy(f->load, w->load, sizeof(f->load));
	h->next = (h->next + 1) % HERMIT_PC_HISTORY;
}

#endif /* _MM_HERMIT_PEBS_POLICY_H */
