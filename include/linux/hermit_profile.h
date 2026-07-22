/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_PROFILE_H
#define _LINUX_HERMIT_PROFILE_H

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/types.h>

enum hermit_pf_stage {
	HMT_PF_TRAP_TO_KERNEL,
	HMT_PF_LOOKUP_SWAPCACHE,
	HMT_PF_CGROUP_ACCOUNT,
	HMT_PF_PAGE_RECLAIM,
	HMT_PF_PAGE_IO,
	HMT_PF_SETPTE,
	HMT_PF_TOTAL,
	HMT_PF_POLL_LOAD,
	HMT_PF_NR_STAGES,
};

struct hermit_pf_profile_ctx {
	u32 flags;
	u64 stage_ns[HMT_PF_NR_STAGES];
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

#endif
