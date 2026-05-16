/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_PROFILE_H
#define _LINUX_HERMIT_PROFILE_H

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/swap_stats.h>
#include <linux/types.h>

struct hermit_pf_profile_ctx {
	int adc_pf_bits;
	uint64_t pf_breakdown[NUM_ADC_PF_BREAKDOWN_TYPE];
};

static inline void hermit_pf_profile_init(struct hermit_pf_profile_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

static inline struct hermit_pf_profile_ctx *hermit_pf_current_ctx(void)
{
	return current->hermit_pf_ctx;
}

static inline struct hermit_pf_profile_ctx *
hermit_pf_push_ctx(struct hermit_pf_profile_ctx *ctx)
{
	struct hermit_pf_profile_ctx *old = current->hermit_pf_ctx;

	current->hermit_pf_ctx = ctx;
	return old;
}

static inline void hermit_pf_pop_ctx(struct hermit_pf_profile_ctx *old)
{
	current->hermit_pf_ctx = old;
}

static inline uint64_t *hermit_pf_current_breakdown(void)
{
	struct hermit_pf_profile_ctx *ctx = hermit_pf_current_ctx();

	return ctx ? ctx->pf_breakdown : NULL;
}

static inline int *hermit_pf_current_bits(void)
{
	struct hermit_pf_profile_ctx *ctx = hermit_pf_current_ctx();

	return ctx ? &ctx->adc_pf_bits : NULL;
}

#endif /* _LINUX_HERMIT_PROFILE_H */
