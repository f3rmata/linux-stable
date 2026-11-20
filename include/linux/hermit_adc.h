/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_ADC_H
#define _LINUX_HERMIT_ADC_H

#ifdef CONFIG_HERMIT
#include <asm/tsc.h>
#include <linux/jump_label.h>
#include <linux/ktime.h>

/* ADC phase boundaries, using the monotonic clock across sleep and migration. */
enum hermit_adc_stage {
	ADC_USER_ADDR_FAULT, ADC_TOTAL_PF, ADC_LOOKUP_SWAPCACHE, ADC_ALLOC_PAGE,
	ADC_CGROUP_ACCOUNT, ADC_DEDUP_SWAPIN, ADC_PREFETCH, ADC_LOCK_FOLIO,
	ADC_LOCK_GET_PTE, ADC_SETPTE, ADC_BATCH_SETPTE, ADC_PAGE_RECLAIM,
	ADC_PG_CHECK_REF, ADC_ALLOC_SWAP_SLOT, ADC_TRY_TO_UNMAP, ADC_TLB_FLUSH_DIRTY,
	ADC_READ_SYNC, ADC_READ_SUBMIT, ADC_POLL_LOAD, ADC_WRITE_PAGE,
	ADC_BATCH_READ_SUBMIT, ADC_BATCH_READ_POLL, ADC_BATCH_WRITE_SUBMIT,
	ADC_BATCH_WRITE_POLL, ADC_READ_CACHE_ASYNC, ADC_ADD_TO_SWAPCACHE,
	ADC_SG_PTE_SCAN, ADC_SG_FINISH, ADC_POLICY_PLAN, ADC_SWAP_MAJOR,
	ADC_SWAP_MINOR, ADC_STAGE_NR
};

DECLARE_STATIC_KEY_FALSE(hermit_adc_key);
void hermit_adc_record(enum hermit_adc_stage stage, u64 ns, u64 ticks);

struct hermit_adc_interval {
	enum hermit_adc_stage stage;
	enum hermit_adc_stage subtype;
	u64 start_ns;
	u64 start_ticks;
};

/* The original ADC used serialized TSC samples and a fixed GHz conversion.
 * Preserve reference ticks on stable TSC machines. Nanoseconds come from the
 * kernel monotonic clock, including on guests without a reliable TSC.
 */
static inline u64 hermit_adc_ticks(void)
{
	if (!boot_cpu_has(X86_FEATURE_CONSTANT_TSC) || check_tsc_unstable())
		return 0;
	barrier();
	return rdtsc_ordered();
}

static inline struct hermit_adc_interval hermit_adc_begin(enum hermit_adc_stage stage)
{
	struct hermit_adc_interval interval = { .stage = stage, .subtype = ADC_STAGE_NR };

	if (static_branch_unlikely(&hermit_adc_key)) {
		interval.start_ns = ktime_get_ns();
		interval.start_ticks = hermit_adc_ticks();
		barrier();
	}
	return interval;
}

static inline void hermit_adc_finish(struct hermit_adc_interval *interval)
{
	u64 ns, ticks = 0;

	if (!interval->start_ns)
		return;
	if (interval->start_ticks) {
		u64 end = hermit_adc_ticks();

		if (end > interval->start_ticks)
			ticks = end - interval->start_ticks;
	}
	ns = ktime_get_ns() - interval->start_ns;
	hermit_adc_record(interval->stage, ns, ticks);
	if (interval->subtype != ADC_STAGE_NR)
		hermit_adc_record(interval->subtype, ns, ticks);
}

/* Each invocation owns its timestamp, including nested or concurrent faults. */
#define HERMIT_ADC_SCOPE(which) \
	struct hermit_adc_interval __attribute__((__cleanup__(hermit_adc_finish))) \
		adc_interval = hermit_adc_begin(which)
#define hermit_adc_call(which, expr) ({ HERMIT_ADC_SCOPE(which); (expr); })
#else
#define HERMIT_ADC_SCOPE(which)
#define hermit_adc_call(which, expr) (expr)
#endif
#endif
