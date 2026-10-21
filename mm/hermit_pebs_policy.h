/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_HERMIT_PEBS_POLICY_H
#define _MM_HERMIT_PEBS_POLICY_H

#include <linux/hermit_pebs.h>
#include <linux/math64.h>
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
	int last_order;
};

struct hermit_cost_params {
	u32 read_ns, wr_ns, bw_mibps, hysteresis;
	u32 min_regions, min_samples;
	u64 ttl_ns;
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

	f->end_ns = w->end_ns;
	f->region = w->region;
	f->loads = w->loads;
	memcpy(f->load, w->load, sizeof(f->load));
	h->next = (h->next + 1) % HERMIT_PC_HISTORY;
}

static inline unsigned int hermit_footprint_count(const u64 *bits,
						 unsigned int start, unsigned int nr)
{
	unsigned int count = 0;

	while (nr) {
		unsigned int take = min(nr, 64U - start % 64);
		u64 word = bits[start / 64] >> (start % 64);

		if (take < 64)
			word &= (1ULL << take) - 1;
		count += hweight64(word);
		start += take;
		nr -= take;
	}
	return count;
}

/*
 * U is mean observed useful pages, with the demanded page forced to one
 * in every history window. Absence of a sample is not proof of coldness.
 * Cost units are ns/useful-page * 1024. All products fit u64 for u32
 * parameters, <= 512 pages and <= 8 histories. The transport currently
 * uses one WR when order is in its mask, otherwise order-0 WRs.
 */
static inline int hermit_footprint_choose(const struct hermit_pc_history *h,
		unsigned int offset, unsigned long candidates, unsigned long wr_mask,
		u64 now, const struct hermit_cost_params *p, u64 *scores,
		bool *confident)
{
	unsigned int live = 0, distinct = 0, i, j, o;
	unsigned long regions[HERMIT_PC_HISTORY];
	u64 samples = 0, best_cost = U64_MAX, previous = U64_MAX;
	int best = 0;

	*confident = false;
	if (!p->bw_mibps || !p->ttl_ns || offset >= HERMIT_REGION_NR_SUBPAGES ||
	    !p->min_regions || p->min_regions > HERMIT_PC_HISTORY ||
	    !p->min_samples || p->hysteresis > 1000)
		return 0;
	for (i = 0; i < HERMIT_PC_HISTORY; i++) {
		const struct hermit_footprint *f = &h->slot[i];

		if (!f->end_ns || now < f->end_ns || now - f->end_ns >= p->ttl_ns)
			continue;
		live |= BIT(i);
		samples += f->loads;
		for (j = 0; j < distinct && regions[j] != f->region; j++)
			;
		if (j == distinct)
			regions[distinct++] = f->region;
	}
	if (distinct < p->min_regions || samples < p->min_samples)
		return 0;
	*confident = true;
	candidates = (candidates | BIT(0)) & ~BIT(1);
	for (o = 0; o <= HERMIT_PEBS_MAX_ORDER; o++) {
		unsigned int nr = 1U << o, start = offset & ~(nr - 1);
		u64 useful = 0, bytes = (u64)PAGE_SIZE << o, cost, transfer;
		unsigned int wrs = (wr_mask & BIT(o)) ? 1 : nr;

		if (!(candidates & BIT(o)))
			continue;
		for (i = 0; i < HERMIT_PC_HISTORY; i++) {
			const u64 *bits = h->slot[i].load;

			if (!(live & BIT(i)))
				continue;
			useful += hermit_footprint_count(bits, start, nr);
			if (!(bits[offset / 64] & (1ULL << (offset % 64))))
				useful++;
		}
		transfer = DIV_ROUND_UP_ULL(bytes * 1000000000ULL,
					 (u64)p->bw_mibps << 20);
		cost = (p->read_ns + (u64)wrs * p->wr_ns + transfer) *
			1024 * hweight32(live);
		cost = div64_u64(cost, useful);
		if (scores)
			scores[o] = cost;
		if (cost < best_cost) {
			best_cost = cost;
			best = o;
		}
		if ((int)o == h->last_order)
			previous = cost;
	}
	/* Both operands stay below 2^64 even at maximum u32 cost settings. */
	if (previous != U64_MAX && best_cost != previous && best != h->last_order &&
	    best_cost * 1000 > previous * p->hysteresis)
		best = h->last_order;
	return best;
}

#endif
