/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_STATS_H
#define _LINUX_HERMIT_STATS_H

#include <linux/types.h>

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

#endif
