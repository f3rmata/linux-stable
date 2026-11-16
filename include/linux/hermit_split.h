/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_SPLIT_H
#define _LINUX_HERMIT_SPLIT_H

#include <linux/types.h>

struct folio;
struct list_head;
struct mem_cgroup;
struct seq_file;
struct dentry;

#ifdef CONFIG_HERMIT
/*
 * HERMIT_POLICY_SPLIT: split a large anonymous folio that reclaim selected
 * when PEBS shows that only an aligned subrange of it is hot, so the cold
 * remainder can be evicted at a small granularity.
 *
 * @folio must be locked and must not be in swapcache. @list receives the
 * tail folios the split creates, exactly like split_folio_to_list().
 *
 * Returns true when the folio may have been split; the caller must then
 * refresh folio_nr_pages() (a failed split can still change the order).
 */
bool hermit_split_try(struct folio *folio, struct list_head *list,
		      struct mem_cgroup *memcg);
void hermit_split_stats_show(struct seq_file *m);
void hermit_split_debugfs_init(struct dentry *root);
#else
static inline bool hermit_split_try(struct folio *folio, struct list_head *list,
				    struct mem_cgroup *memcg)
{
	return false;
}
#endif

#endif /* _LINUX_HERMIT_SPLIT_H */
