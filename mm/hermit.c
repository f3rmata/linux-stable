// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/hermit.h>
#include <linux/init.h>
#include <linux/kernel.h>

bool hmt_ctl_flags[NUM_HMT_CTL_FLAGS];
u32 hmt_ctl_vars[NUM_HMT_CTL_VARS];

static const char * const hmt_ctl_flag_names[NUM_HMT_CTL_FLAGS] = {
	"bypass_swapcache", "batch_swapout", "batch_tlb", "batch_io",
	"batch_account", "vaddr_swapout", "speculative_io",
	"speculative_lock", "lazy_poll", "apt_reclaim",
};
static const char * const hmt_ctl_var_names[NUM_HMT_CTL_VARS] = {
	"sthd_cnt", "reclaim_mode",
};

static int __init hermit_init(void)
{
	struct dentry *root;
	int i;

	root = debugfs_create_dir("hermit", NULL);
	if (IS_ERR(root))
		return PTR_ERR(root);
	for (i = 0; i < NUM_HMT_CTL_FLAGS; i++)
		debugfs_create_bool(hmt_ctl_flag_names[i], 0600, root,
				    &hmt_ctl_flags[i]);
	for (i = 0; i < NUM_HMT_CTL_VARS; i++)
		debugfs_create_u32(hmt_ctl_var_names[i], 0600, root,
				   &hmt_ctl_vars[i]);
	hmt_ctl_vars[HMT_STHD_CNT] = min_t(u32, num_online_cpus(), 4);
	return 0;
}
subsys_initcall(hermit_init);
