// SPDX-License-Identifier: GPL-2.0
#include <asm/local64.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/hermit_adc.h>
#include <linux/hermit_stats.h>
#include <linux/jump_label.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

struct hermit_time_stat {
	atomic64_t total_ns;
	atomic64_t count;
};
static atomic_long_t hermit_counters[HMT_STAT_NR];
static struct hermit_time_stat hermit_latencies[HMT_LAT_NR];

/*
 * Swap-in cost, per folio order. count/total_ns cover the interval from
 * do_swap_page() entry to the installed PTE, so they include the backend read,
 * the swapcache work and everything the fault waits on. The prefetch counters
 * tie those costs to policy readahead: pages read speculatively by
 * hermit_swapin_pages() and later demand faults that found them in swapcache
 * instead of paying a backend read.
 */
static struct hermit_time_stat hermit_swapin_cost[HERMIT_COST_ORDERS];
static atomic64_t hermit_prefetched_pages;
static atomic64_t hermit_prefetch_hits;
static atomic64_t hermit_prefetch_hit_ns;

DEFINE_STATIC_KEY_FALSE(hermit_cost_key);

void hermit_swapin_cost_add(unsigned int order, u64 ns)
{
	if (order >= HERMIT_COST_ORDERS)
		return;
	atomic64_add(ns, &hermit_swapin_cost[order].total_ns);
	atomic64_inc(&hermit_swapin_cost[order].count);
}

void hermit_swapin_cost_prefetched(unsigned int nr)
{
	atomic64_add(nr, &hermit_prefetched_pages);
}

void hermit_swapin_cost_hit(u64 ns)
{
	atomic64_add(ns, &hermit_prefetch_hit_ns);
	atomic64_inc(&hermit_prefetch_hits);
}

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
	for (i = 0; i < HERMIT_COST_ORDERS; i++) {
		atomic64_set(&hermit_swapin_cost[i].total_ns, 0);
		atomic64_set(&hermit_swapin_cost[i].count, 0);
	}
	atomic64_set(&hermit_prefetched_pages, 0);
	atomic64_set(&hermit_prefetch_hits, 0);
	atomic64_set(&hermit_prefetch_hit_ns, 0);
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

SYSCALL_DEFINE3(get_swap_stats, long __user *, ondemand,
		long __user *, prefetch, long __user *, hit_on_cache)
{
	int ret = 0;

	/*
	 * No per-call hermit_stats_report(): dumping 15 pr_info lines on every
	 * invocation lets an unprivileged caller flood the kernel log.
	 */
	if (put_user(hermit_stat_read(HMT_STAT_ONDEMAND_SWAPIN), ondemand))
		ret = -EFAULT;
	if (put_user(hermit_stat_read(HMT_STAT_PREFETCH_SWAPIN), prefetch))
		ret = -EFAULT;
	if (put_user(hermit_stat_read(HMT_STAT_HIT_ON_SWAPCACHE), hit_on_cache))
		ret = -EFAULT;
	return ret;
}

/*
 * Read-only cost table. Field names are stable: tools parse "name value" and
 * diff two snapshots, so new rows are appended, never renamed or reordered.
 */
static int hermit_cost_stats_show(struct seq_file *m, void *unused)
{
	unsigned int order;

	seq_printf(m, "enabled %u\n", hermit_swapin_cost_enabled());
	for (order = 0; order < HERMIT_COST_ORDERS; order++) {
		seq_printf(m, "order%u_faults %lld\n", order,
			   atomic64_read(&hermit_swapin_cost[order].count));
		seq_printf(m, "order%u_ns %lld\n", order,
			   atomic64_read(&hermit_swapin_cost[order].total_ns));
	}
	seq_printf(m, "prefetched_4k %lld\n",
		   atomic64_read(&hermit_prefetched_pages));
	seq_printf(m, "prefetch_hit_faults %lld\n",
		   atomic64_read(&hermit_prefetch_hits));
	seq_printf(m, "prefetch_hit_ns %lld\n",
		   atomic64_read(&hermit_prefetch_hit_ns));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hermit_cost_stats);

/* Fixed-width parser, so a short write cannot clobber the stored value. */
static int hermit_cost_enable_get(void *data, u64 *value)
{
	*value = hermit_swapin_cost_enabled();
	return 0;
}

static int hermit_cost_enable_set(void *data, u64 value)
{
	if (value > 1)
		return -EINVAL;
	if (value)
		static_branch_enable(&hermit_cost_key);
	else
		static_branch_disable(&hermit_cost_key);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(hermit_cost_enable_fops, hermit_cost_enable_get,
			 hermit_cost_enable_set, "%llu\n");

DEFINE_STATIC_KEY_FALSE(hermit_adc_key);

/* Per-CPU updates avoid a shared cacheline on the fault and reclaim paths. */
struct hermit_adc_cpu {
	local64_t ns[ADC_STAGE_NR];
	local64_t count[ADC_STAGE_NR];
	local64_t ticks[ADC_STAGE_NR];
	local64_t tick_count[ADC_STAGE_NR];
};
static DEFINE_PER_CPU(struct hermit_adc_cpu, hermit_adc_cpu);

void hermit_adc_record(enum hermit_adc_stage stage, u64 ns, u64 ticks)
{
	struct hermit_adc_cpu *cpu = get_cpu_ptr(&hermit_adc_cpu);

	local64_add(ns, &cpu->ns[stage]);
	local64_inc(&cpu->count[stage]);
	if (ticks) {
		local64_add(ticks, &cpu->ticks[stage]);
		local64_inc(&cpu->tick_count[stage]);
	}
	put_cpu_ptr(&hermit_adc_cpu);
}

static const char * const adc_names[] = {
	"user_addr_fault", "do_swap_page", "lookup_swapcache", "alloc_page",
	"cgroup_account", "dedup_swapin", "policy_prefetch", "lock_folio",
	"lock_get_pte", "set_pte", "batch_set_pte", "reclaim_folio_list",
	"check_references", "alloc_swap_slot", "try_to_unmap", "tlb_flush_dirty",
	"backend_read_sync", "backend_read_submit", "backend_poll_load", "backend_write",
	"batch_read_submit", "batch_read_poll", "batch_write_submit", "batch_write_poll",
	"read_cache_async", "add_to_swapcache", "sg_pte_scan", "sg_finish",
	"policy_plan", "swap_major", "swap_minor",
};
static_assert(ARRAY_SIZE(adc_names) == ADC_STAGE_NR);

static int hermit_adc_show(struct seq_file *m, void *unused)
{
	unsigned int stage, cpu;

	seq_puts(m, "stage count total_ns tsc_count total_tsc_ticks\n");
	for (stage = 0; stage < ADC_STAGE_NR; stage++) {
		u64 count = 0, ns = 0, tick_count = 0, ticks = 0;

		for_each_possible_cpu(cpu) {
			struct hermit_adc_cpu *c = per_cpu_ptr(&hermit_adc_cpu, cpu);

			count += local64_read(&c->count[stage]);
			ns += local64_read(&c->ns[stage]);
			ticks += local64_read(&c->ticks[stage]);
			tick_count += local64_read(&c->tick_count[stage]);
		}
		seq_printf(m, "%s %llu %llu %llu %llu\n", adc_names[stage],
			   count, ns, tick_count, ticks);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hermit_adc);

static int hermit_adc_enable_get(void *data, u64 *value)
{
	*value = static_branch_unlikely(&hermit_adc_key);
	return 0;
}

static int hermit_adc_enable_set(void *data, u64 value)
{
	if (value > 1)
		return -EINVAL;
	if (value)
		static_branch_enable(&hermit_adc_key);
	else
		static_branch_disable(&hermit_adc_key);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(hermit_adc_enable_fops, hermit_adc_enable_get,
			 hermit_adc_enable_set, "%llu\n");

/* Owner-only access, as for the sibling Hermit control files. The symbolic
 * bits are used because the tree lint rejects octal literals.
 */
void hermit_stats_debugfs_init(struct dentry *root)
{
	debugfs_create_file("adc_stats", S_IRUSR, root, NULL, &hermit_adc_fops);
	debugfs_create_file("adc_enable", S_IRUSR | S_IWUSR, root, NULL,
			    &hermit_adc_enable_fops);
	debugfs_create_file("swapin_cost_stats", S_IRUSR, root, NULL,
			    &hermit_cost_stats_fops);
	debugfs_create_file("swapin_cost_enable", S_IRUSR | S_IWUSR, root, NULL,
			    &hermit_cost_enable_fops);
}
