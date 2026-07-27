/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_H
#define _LINUX_HERMIT_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct mem_cgroup;

#define HMT_MAX_NR_STHDS 16
#define HMT_DEFAULT_RECLAIM_HEADROOM_PAGES 2048U

struct hmt_reclaim_work {
	struct work_struct work;
	struct mem_cgroup *memcg;
};

enum hmt_ctl_flag_type {
	HMT_BPS_SCACHE,
	HMT_BATCH_OUT,
	HMT_BATCH_TLB,
	HMT_BATCH_IO,
	HMT_BATCH_ACCOUNT,
	HMT_VADDR_OUT,
	HMT_SPEC_IO,
	HMT_SPEC_LOCK,
	HMT_LAZY_POLL,
	HMT_APT_RECLAIM,
	NUM_HMT_CTL_FLAGS,
};

enum hmt_ctl_var_type {
	HMT_STHD_CNT,
	HMT_RECLAIM_MODE,
	HMT_RECLAIM_HEADROOM_PAGES,
	NUM_HMT_CTL_VARS,
};

extern bool hmt_ctl_flags[NUM_HMT_CTL_FLAGS];
extern u32 hmt_ctl_vars[NUM_HMT_CTL_VARS];

static inline bool hmt_ctl_flag(enum hmt_ctl_flag_type type)
{
	return READ_ONCE(hmt_ctl_flags[type]);
}

static inline u32 hmt_ctl_var(enum hmt_ctl_var_type type)
{
	return READ_ONCE(hmt_ctl_vars[type]);
}

#endif
