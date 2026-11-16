/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_STATS_H
#define _LINUX_HERMIT_STATS_H

#include <linux/jump_label.h>
#include <linux/types.h>

struct dentry;

enum hermit_counter {
	HMT_STAT_ONDEMAND_SWAPIN,
	HMT_STAT_PREFETCH_SWAPIN,
	HMT_STAT_HIT_ON_SWAPCACHE,
	HMT_STAT_SWAPOUT,
	HMT_STAT_RECLAIM,
	HMT_STAT_BACKEND_STORE,
	HMT_STAT_BACKEND_LOAD,
	HMT_STAT_BACKEND_ERROR,
	HMT_STAT_NR,
};

enum hermit_latency {
	HMT_LAT_SWAP_MAJOR,
	HMT_LAT_SWAP_MINOR,
	HMT_LAT_SWAPOUT,
	HMT_LAT_NON_SWAP,
	HMT_LAT_BACKEND_READ,
	HMT_LAT_BACKEND_WRITE,
	HMT_LAT_POLL_LOAD,
	HMT_LAT_NR,
};

void hermit_stat_inc(enum hermit_counter counter);
void hermit_stat_add(enum hermit_counter counter, long value);
long hermit_stat_read(enum hermit_counter counter);
void hermit_latency_add(enum hermit_latency latency, u64 ns);
void hermit_stats_reset(void);
void hermit_stats_report(void);

/*
 * Per-order swap-in cost accounting. Debugfs swapin_cost_enable gates it: the
 * fault path pays one patched jump label when the toggle is off and no
 * ktime_get_*() call at all. It stays off by default because these counters
 * are diagnostic, not part of a policy decision.
 */
#define HERMIT_COST_ORDERS 10

DECLARE_STATIC_KEY_FALSE(hermit_cost_key);

static inline bool hermit_swapin_cost_enabled(void)
{
	return static_branch_unlikely(&hermit_cost_key);
}

void hermit_swapin_cost_add(unsigned int order, u64 ns);
void hermit_swapin_cost_prefetched(unsigned int nr);
void hermit_swapin_cost_hit(u64 ns);
void hermit_stats_debugfs_init(struct dentry *root);

#endif
