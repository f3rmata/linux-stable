// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/hermit_stats.h>
#include <linux/printk.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

struct hermit_time_stat {
	atomic64_t total_ns;
	atomic64_t count;
};
static atomic_long_t hermit_counters[HMT_STAT_NR];
static struct hermit_time_stat hermit_latencies[HMT_LAT_NR];
static const char * const hermit_counter_names[HMT_STAT_NR] = {
	"Demand", "Prefetch", "HitOnCache", "TotalSwapOut",
	"TotalReclaim", "HermitSwapOut", "HermitSwapIn", "BackendErrors",
};
static const char * const hermit_latency_names[HMT_LAT_NR] = {
	"major swap duration", "minor swap duration", "swap-out   duration",
	"non-swap   duration", "RDMA read  latency", "RDMA write latency",
	"Poll wait",
};

void hermit_stat_inc(enum hermit_counter counter)
{
	atomic_long_inc(&hermit_counters[counter]);
}
EXPORT_SYMBOL_GPL(hermit_stat_inc);

void hermit_stat_add(enum hermit_counter counter, long value)
{
	atomic_long_add(value, &hermit_counters[counter]);
}
EXPORT_SYMBOL_GPL(hermit_stat_add);

long hermit_stat_read(enum hermit_counter counter)
{
	return atomic_long_read(&hermit_counters[counter]);
}
EXPORT_SYMBOL_GPL(hermit_stat_read);

void hermit_latency_add(enum hermit_latency latency, u64 ns)
{
	atomic64_add(ns, &hermit_latencies[latency].total_ns);
	atomic64_inc(&hermit_latencies[latency].count);
}
EXPORT_SYMBOL_GPL(hermit_latency_add);

void hermit_stats_reset(void)
{
	int i;

	for (i = 0; i < HMT_STAT_NR; i++)
		atomic_long_set(&hermit_counters[i], 0);
	for (i = 0; i < HMT_LAT_NR; i++) {
		atomic64_set(&hermit_latencies[i].total_ns, 0);
		atomic64_set(&hermit_latencies[i].count, 0);
	}
}

void hermit_stats_report(void)
{
	int i;

	for (i = 0; i < HMT_STAT_NR; i++)
		pr_info("%s: %ld\n", hermit_counter_names[i],
			hermit_stat_read(i));
	for (i = 0; i < HMT_LAT_NR; i++) {
		s64 count = atomic64_read(&hermit_latencies[i].count);
		s64 total = atomic64_read(&hermit_latencies[i].total_ns);

		pr_info("%s: %lldns, #: %lld\n", hermit_latency_names[i],
			count ? total / count : 0, count);
	}
}

SYSCALL_DEFINE0(reset_swap_stats)
{
	hermit_stats_reset();
	return 0;
}

SYSCALL_DEFINE3(get_swap_stats, int __user *, ondemand,
		int __user *, prefetch, int __user *, hit_on_cache)
{
	int ret = 0;

	hermit_stats_report();
	if (put_user((int)hermit_stat_read(HMT_STAT_ONDEMAND_SWAPIN), ondemand))
		ret = -EFAULT;
	if (put_user((int)hermit_stat_read(HMT_STAT_PREFETCH_SWAPIN), prefetch))
		ret = -EFAULT;
	if (put_user((int)hermit_stat_read(HMT_STAT_HIT_ON_SWAPCACHE), hit_on_cache))
		ret = -EFAULT;
	return ret;
}
