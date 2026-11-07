/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_RECLAIM_H
#define _LINUX_HERMIT_RECLAIM_H

struct hmt_reclaim_watermarks {
	unsigned long low;
	unsigned long high;
};

/*
 * A zero stop setting preserves the existing single-watermark behaviour.
 * Explicit hysteresis is bounded by the cgroup's local limit and never asks
 * for its entire budget to be free. Invalid ordering collapses to one level.
 * Ancestor/NUMA pressure and prefetch reservations are not represented here.
 */
static inline struct hmt_reclaim_watermarks
hmt_reclaim_watermarks(unsigned long start, unsigned long stop,
		       unsigned long limit)
{
	struct hmt_reclaim_watermarks marks = { start, start };

	if (!start || !stop)
		return marks;
	if (limit <= 1) {
		marks.low = 0;
		marks.high = 0;
		return marks;
	}
	if (start >= limit)
		start = limit - 1;
	if (stop >= limit)
		stop = limit - 1;
	marks.low = start;
	marks.high = stop > start ? stop : start;
	return marks;
}

#endif /* _LINUX_HERMIT_RECLAIM_H */
