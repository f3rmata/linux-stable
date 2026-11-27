// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/hermit_adc.h>
#include <linux/mm.h>
#include <linux/hermit.h>
#include <linux/hermit_backend.h>
#include <linux/hermit_policy.h>
#include <linux/hermit_stats.h>
#include <linux/userfaultfd_k.h>
#include <linux/ktime.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/mempolicy.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
#include "internal.h"
#include "swap_table.h"
#include "swap.h"

/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_folio_list.
 */
static const struct address_space_operations swap_aops = {
	.dirty_folio = noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio = migrate_folio,
#endif
};

struct address_space swap_space __read_mostly = {
	.a_ops = &swap_aops,
};

static bool enable_vma_readahead __read_mostly = true;

#define SWAP_RA_ORDER_CEILING 5

#define SWAP_RA_WIN_SHIFT (PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK ((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK (~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v) ((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v) (((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v) ((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)                         \
	(((addr) & PAGE_MASK) |                              \
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) | \
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma) \
	(atomic_long_read(&(vma)->swap_readahead_info) ?: 4)

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);

void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Free swap  = %ldkB\n", K(get_nr_swap_pages()));
	printk("Total swap = %lukB\n", K(total_swap_pages));
}

/**
 * swap_cache_get_folio - Looks up a folio in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * A found folio will be returned unlocked and with its refcount increased.
 *
 * Context: Caller must ensure @entry is valid and protect the swap device
 * with reference count or locks.
 * Return: Returns the found folio on success, NULL otherwise. The caller
 * must lock nd check if the folio still matches the swap entry before
 * use (e.g., folio_matches_swap_entry).
 */
struct folio *swap_cache_get_folio(swp_entry_t entry)
{
	unsigned long swp_tb;
	struct folio *folio;

	for (;;) {
		swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
					swp_cluster_offset(entry));
		if (!swp_tb_is_folio(swp_tb))
			return NULL;
		folio = swp_tb_to_folio(swp_tb);
		if (likely(folio_try_get(folio)))
			return folio;
	}

	return NULL;
}

/**
 * swap_cache_get_shadow - Looks up a shadow in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * Context: Caller must ensure @entry is valid and protect the swap device
 * with reference count or locks.
 * Return: Returns either NULL or an XA_VALUE (shadow).
 */
void *swap_cache_get_shadow(swp_entry_t entry)
{
	unsigned long swp_tb;

	swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
				swp_cluster_offset(entry));
	if (swp_tb_is_shadow(swp_tb))
		return swp_tb_to_shadow(swp_tb);
	return NULL;
}

/**
 * swap_cache_add_folio - Add a folio into the swap cache.
 * @folio: The folio to be added.
 * @entry: The swap entry corresponding to the folio.
 * @gfp: gfp_mask for XArray node allocation.
 * @shadowp: If a shadow is found, return the shadow.
 *
 * Context: Caller must ensure @entry is valid and protect the swap device
 * with reference count or locks.
 * The caller also needs to update the corresponding swap_map slots with
 * SWAP_HAS_CACHE bit to avoid race or conflict.
 */
void swap_cache_add_folio(struct folio *folio, swp_entry_t entry,
			  void **shadowp)
{
	HERMIT_ADC_SCOPE(ADC_ADD_TO_SWAPCACHE);
	void *shadow = NULL;
	unsigned long old_tb, new_tb;
	struct swap_cluster_info *ci;
	unsigned int ci_start, ci_off, ci_end;
	unsigned long nr_pages = folio_nr_pages(folio);

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapbacked(folio), folio);

	new_tb = folio_to_swp_tb(folio);
	ci_start = swp_cluster_offset(entry);
	ci_end = ci_start + nr_pages;
	ci_off = ci_start;
	ci = swap_cluster_lock(__swap_entry_to_info(entry), swp_offset(entry));
	do {
		old_tb = __swap_table_xchg(ci, ci_off, new_tb);
		WARN_ON_ONCE(swp_tb_is_folio(old_tb));
		if (swp_tb_is_shadow(old_tb))
			shadow = swp_tb_to_shadow(old_tb);
	} while (++ci_off < ci_end);

	folio_ref_add(folio, nr_pages);
	folio_set_swapcache(folio);
	folio->swap = entry;
	swap_cluster_unlock(ci);

	node_stat_mod_folio(folio, NR_FILE_PAGES, nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr_pages);

	if (shadowp)
		*shadowp = shadow;
}

/**
 * __swap_cache_del_folio - Removes a folio from the swap cache.
 * @ci: The locked swap cluster.
 * @folio: The folio.
 * @entry: The first swap entry that the folio corresponds to.
 * @shadow: shadow value to be filled in the swap cache.
 *
 * Removes a folio from the swap cache and fills a shadow in place.
 * This won't put the folio's refcount. The caller has to do that.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache
 * using the index of @entry, and lock the cluster that holds the entries.
 */
void __swap_cache_del_folio(struct swap_cluster_info *ci, struct folio *folio,
			    swp_entry_t entry, void *shadow)
{
	unsigned long old_tb, new_tb;
	unsigned int ci_start, ci_off, ci_end;
	unsigned long nr_pages = folio_nr_pages(folio);

	VM_WARN_ON_ONCE(__swap_entry_to_cluster(entry) != ci);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_writeback(folio), folio);

	new_tb = shadow_swp_to_tb(shadow);
	ci_start = swp_cluster_offset(entry);
	ci_end = ci_start + nr_pages;
	ci_off = ci_start;
	do {
		/* If shadow is NULL, we sets an empty shadow */
		old_tb = __swap_table_xchg(ci, ci_off, new_tb);
		WARN_ON_ONCE(!swp_tb_is_folio(old_tb) ||
			     swp_tb_to_folio(old_tb) != folio);
	} while (++ci_off < ci_end);

	folio->swap.val = 0;
	folio_clear_swapcache(folio);
	node_stat_mod_folio(folio, NR_FILE_PAGES, -nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr_pages);
}

/**
 * swap_cache_del_folio - Removes a folio from the swap cache.
 * @folio: The folio.
 *
 * Same as __swap_cache_del_folio, but handles lock and refcount. The
 * caller must ensure the folio is either clean or has a swap count
 * equal to zero, or it may cause data loss.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache.
 */
void swap_cache_del_folio(struct folio *folio)
{
	struct swap_cluster_info *ci;
	swp_entry_t entry = folio->swap;

	ci = swap_cluster_lock(__swap_entry_to_info(entry), swp_offset(entry));
	__swap_cache_del_folio(ci, folio, entry, NULL);
	swap_cluster_unlock(ci);

	put_swap_folio(folio, entry);
	folio_ref_sub(folio, folio_nr_pages(folio));
}

/**
 * __swap_cache_replace_folio - Replace a folio in the swap cache.
 * @ci: The locked swap cluster.
 * @old: The old folio to be replaced.
 * @new: The new folio.
 *
 * Replace an existing folio in the swap cache with a new folio. The
 * caller is responsible for setting up the new folio's flag and swap
 * entries. Replacement will take the new folio's swap entry value as
 * the starting offset to override all slots covered by the new folio.
 *
 * Context: Caller must ensure both folios are locked, and lock the
 * cluster that holds the old folio to be replaced.
 */
void __swap_cache_replace_folio(struct swap_cluster_info *ci, struct folio *old,
				struct folio *new)
{
	swp_entry_t entry = new->swap;
	unsigned long nr_pages = folio_nr_pages(new);
	unsigned int ci_off = swp_cluster_offset(entry);
	unsigned int ci_end = ci_off + nr_pages;
	unsigned long old_tb, new_tb;

	VM_WARN_ON_ONCE(!folio_test_swapcache(old) ||
			!folio_test_swapcache(new));
	VM_WARN_ON_ONCE(!folio_test_locked(old) || !folio_test_locked(new));
	VM_WARN_ON_ONCE(!entry.val);

	/* Swap cache still stores N entries instead of a high-order entry */
	new_tb = folio_to_swp_tb(new);
	do {
		old_tb = __swap_table_xchg(ci, ci_off, new_tb);
		WARN_ON_ONCE(!swp_tb_is_folio(old_tb) ||
			     swp_tb_to_folio(old_tb) != old);
	} while (++ci_off < ci_end);

	/*
	 * If the old folio is partially replaced (e.g., splitting a large
	 * folio, the old folio is shrunk, and new split sub folios replace
	 * the shrunk part), ensure the new folio doesn't overlap it.
	 */
	if (IS_ENABLED(CONFIG_DEBUG_VM) &&
	    folio_order(old) != folio_order(new)) {
		ci_off = swp_cluster_offset(old->swap);
		ci_end = ci_off + folio_nr_pages(old);
		while (ci_off++ < ci_end)
			WARN_ON_ONCE(swp_tb_to_folio(__swap_table_get(
					     ci, ci_off)) != old);
	}
}

/**
 * swap_cache_clear_shadow - Clears a set of shadows in the swap cache.
 * @entry: The starting index entry.
 * @nr_ents: How many slots need to be cleared.
 *
 * Context: Caller must ensure the range is valid, all in one single cluster,
 * not occupied by any folio, and lock the cluster.
 */
void __swap_cache_clear_shadow(swp_entry_t entry, int nr_ents)
{
	struct swap_cluster_info *ci = __swap_entry_to_cluster(entry);
	unsigned int ci_off = swp_cluster_offset(entry), ci_end;
	unsigned long old;

	ci_end = ci_off + nr_ents;
	do {
		old = __swap_table_xchg(ci, ci_off, null_to_swp_tb());
		WARN_ON_ONCE(swp_tb_is_folio(old));
	} while (++ci_off < ci_end);
}

/*
 * If we are the only user, then try to free up the swap cache.
 *
 * Its ok to check the swapcache flag without the folio lock
 * here because we are going to recheck again inside
 * folio_free_swap() _with_ the lock.
 * 					- Marcelo
 */
void free_swap_cache(struct folio *folio)
{
	if (folio_test_swapcache(folio) && !folio_mapped(folio) &&
	    folio_trylock(folio)) {
		folio_free_swap(folio);
		folio_unlock(folio);
	}
}

/*
 * Freeing a folio and also freeing any swap cache associated with
 * this folio if it is the last user.
 */
void free_folio_and_swap_cache(struct folio *folio)
{
	free_swap_cache(folio);
	if (!is_huge_zero_folio(folio))
		folio_put(folio);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct encoded_page **pages, int nr)
{
	struct folio_batch folios;
	unsigned int refs[PAGEVEC_SIZE];

	folio_batch_init(&folios);
	for (int i = 0; i < nr; i++) {
		struct folio *folio = page_folio(encoded_page_ptr(pages[i]));

		free_swap_cache(folio);
		refs[folios.nr] = 1;
		if (unlikely(encoded_page_flags(pages[i]) &
			     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
			refs[folios.nr] = encoded_nr_pages(pages[++i]);

		if (folio_batch_add(&folios, folio) == 0)
			folios_put_refs(&folios, refs);
	}
	if (folios.nr)
		folios_put_refs(&folios, refs);
}

static inline bool swap_use_vma_readahead(void)
{
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/**
 * swap_update_readahead - Update the readahead statistics of VMA or globally.
 * @folio: the swap cache folio that just got hit.
 * @vma: the VMA that should be updated, could be NULL for global update.
 * @addr: the addr that triggered the swapin, ignored if @vma is NULL.
 */
void swap_update_readahead(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long addr)
{
	bool readahead, vma_ra = swap_use_vma_readahead();

	/*
	 * At the moment, we don't support PG_readahead for anon THP
	 * so let's bail out rather than confusing the readahead stat.
	 */
	if (unlikely(folio_test_large(folio)))
		return;

	readahead = folio_test_clear_readahead(folio);
	if (vma && vma_ra) {
		unsigned long ra_val;
		int win, hits;

		ra_val = GET_SWAP_RA_VAL(vma);
		win = SWAP_RA_WIN(ra_val);
		hits = SWAP_RA_HITS(ra_val);
		if (readahead)
			hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
		atomic_long_set(&vma->swap_readahead_info,
				SWAP_RA_VAL(addr, win, hits));
	}

	if (readahead) {
		count_vm_event(SWAP_RA_HIT);
		if (!vma || !vma_ra)
			atomic_inc(&swapin_readahead_hits);
	}
}

struct folio *__read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
				      struct mempolicy *mpol, pgoff_t ilx,
				      bool *new_page_allocated,
				      bool skip_if_exists)
{
	HERMIT_ADC_SCOPE(ADC_READ_CACHE_ASYNC);
	struct swap_info_struct *si = __swap_entry_to_info(entry);
	struct folio *folio;
	struct folio *new_folio = NULL;
	struct folio *result = NULL;
	void *shadow = NULL;

	*new_page_allocated = false;
	for (;;) {
		int err;

		/*
		 * Check the swap cache first, if a cached folio is found,
		 * return it unlocked. The caller will lock and check it.
		 */
		folio = swap_cache_get_folio(entry);
		if (folio)
			goto got_folio;

		/*
		 * Just skip read ahead for unused swap slot.
		 */
		if (!swap_entry_swapped(si, entry))
			goto put_and_return;

		/*
		 * Get a new folio to read into from swap.  Allocate it now if
		 * new_folio not exist, before marking swap_map SWAP_HAS_CACHE,
		 * when -EEXIST will cause any racers to loop around until we
		 * add it to cache.
		 */
		if (!new_folio) {
			new_folio = folio_alloc_mpol(gfp_mask, 0, mpol, ilx,
						     numa_node_id());
			if (!new_folio)
				goto put_and_return;
		}

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry, 1);
		if (!err)
			break;
		else if (err != -EEXIST)
			goto put_and_return;

		/*
		 * Protect against a recursive call to __read_swap_cache_async()
		 * on the same entry waiting forever here because SWAP_HAS_CACHE
		 * is set but the folio is not the swap cache yet. This can
		 * happen today if mem_cgroup_swapin_charge_folio() below
		 * triggers reclaim through zswap, which may call
		 * __read_swap_cache_async() in the writeback path.
		 */
		if (skip_if_exists)
			goto put_and_return;

		/*
		 * We might race against __swap_cache_del_folio(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its folio to swap cache.
		 */
		schedule_timeout_uninterruptible(1);
	}

	/*
	 * The swap entry is ours to swap in. Prepare the new folio.
	 */
	__folio_set_locked(new_folio);
	__folio_set_swapbacked(new_folio);

	if (mem_cgroup_swapin_charge_folio(new_folio, NULL, gfp_mask, entry))
		goto fail_unlock;

	swap_cache_add_folio(new_folio, entry, &shadow);
	memcg1_swapin(entry, 1);

	if (shadow)
		workingset_refault(new_folio, shadow);

	/* Caller will initiate read into locked new_folio */
	folio_add_lru(new_folio);
	*new_page_allocated = true;
	folio = new_folio;
got_folio:
	result = folio;
	goto put_and_return;

fail_unlock:
	put_swap_folio(new_folio, entry);
	folio_unlock(new_folio);
put_and_return:
	if (!(*new_page_allocated) && new_folio)
		folio_put(new_folio);
	return result;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 *
 * get/put_swap_device() aren't needed to call this function, because
 * __read_swap_cache_async() call them and swap_read_folio() holds the
 * swap cache folio lock.
 */
struct folio *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
				    struct vm_area_struct *vma,
				    unsigned long addr, struct swap_iocb **plug)
{
	struct swap_info_struct *si;
	bool page_allocated;
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;

	si = get_swap_device(entry);
	if (!si)
		return NULL;

	mpol = get_vma_policy(vma, addr, 0, &ilx);
	folio = __read_swap_cache_async(entry, gfp_mask, mpol, ilx,
					&page_allocated, false);
	mpol_cond_put(mpol);

	if (page_allocated)
		swap_read_folio(folio, plug);

	put_swap_device(si);
	return folio;
}

static unsigned int swapin_readahead_pages(unsigned long prev_offset,
					   unsigned long offset, int hits,
					   int max_pages, int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = swapin_readahead_pages(READ_ONCE(prev_offset), offset, hits,
				       max_pages,
				       atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * Note: it is intentional that the same NUMA policy and interleave index
 * are used for every page of the readahead: neighbouring pages on swap
 * are fairly likely to have been swapped out from the same node.
 */
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				     struct mempolicy *mpol, pgoff_t ilx)
{
	struct folio *folio;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = __swap_entry_to_info(entry);
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	bool page_allocated;

	mask = swapin_nr_pages(offset) - 1;
	if (!mask)
		goto skip;

	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset) /* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset; offset++) {
		/* Ok, do the async read-ahead now */
		folio = __read_swap_cache_async(
			swp_entry(swp_type(entry), offset), gfp_mask, mpol, ilx,
			&page_allocated, false);
		if (!folio)
			continue;
		if (page_allocated) {
			swap_read_folio(folio, &splug);
			if (offset != entry_offset) {
				folio_set_readahead(folio);
				count_vm_event(SWAP_RA);
			}
		}
		folio_put(folio);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain(); /* Push any new pages onto the LRU now */
skip:
	/* The page was likely read above, so no need for plugging here */
	folio = __read_swap_cache_async(entry, gfp_mask, mpol, ilx,
					&page_allocated, false);
	if (unlikely(page_allocated))
		swap_read_folio(folio, NULL);
	return folio;
}

static int swap_vma_ra_win(struct vm_fault *vmf, unsigned long *start,
			   unsigned long *end)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	unsigned long faddr, prev_faddr, left, right;
	unsigned int max_win, hits, prev_win, win;

	max_win = 1 << min(READ_ONCE(page_cluster), SWAP_RA_ORDER_CEILING);
	if (max_win == 1)
		return 1;

	faddr = vmf->address;
	ra_val = GET_SWAP_RA_VAL(vma);
	prev_faddr = SWAP_RA_ADDR(ra_val);
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	win = swapin_readahead_pages(PFN_DOWN(prev_faddr), PFN_DOWN(faddr),
				     hits, max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info, SWAP_RA_VAL(faddr, win, 0));
	if (win == 1)
		return 1;

	if (faddr == prev_faddr + PAGE_SIZE)
		left = faddr;
	else if (prev_faddr == faddr + PAGE_SIZE)
		left = faddr - (win << PAGE_SHIFT) + PAGE_SIZE;
	else
		left = faddr - (((win - 1) / 2) << PAGE_SHIFT);
	right = left + (win << PAGE_SHIFT);
	if ((long)left < 0)
		left = 0;
	*start = max3(left, vma->vm_start, faddr & PMD_MASK);
	*end = min3(right, vma->vm_end, (faddr & PMD_MASK) + PMD_SIZE);

	return win;
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @targ_entry: swap entry of the targeted memory
 * @gfp_mask: memory allocation flags
 * @mpol: NUMA memory allocation policy to be applied
 * @targ_ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 * @vmf: fault information
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whose
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
static struct folio *swap_vma_readahead(swp_entry_t targ_entry, gfp_t gfp_mask,
					struct mempolicy *mpol,
					pgoff_t targ_ilx, struct vm_fault *vmf)
{
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	struct folio *folio;
	pte_t *pte = NULL, pentry;
	int win;
	unsigned long start, end, addr;
	swp_entry_t entry;
	pgoff_t ilx;
	bool page_allocated;

	win = swap_vma_ra_win(vmf, &start, &end);
	if (win == 1)
		goto skip;

	ilx = targ_ilx - PFN_DOWN(vmf->address - start);

	blk_start_plug(&plug);
	for (addr = start; addr < end; ilx++, addr += PAGE_SIZE) {
		struct swap_info_struct *si = NULL;

		if (!pte++) {
			pte = pte_offset_map(vmf->pmd, addr);
			if (!pte)
				break;
		}
		pentry = ptep_get_lockless(pte);
		if (!is_swap_pte(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		pte_unmap(pte);
		pte = NULL;
		/*
		 * Readahead entry may come from a device that we are not
		 * holding a reference to, try to grab a reference, or skip.
		 */
		if (swp_type(entry) != swp_type(targ_entry)) {
			si = get_swap_device(entry);
			if (!si)
				continue;
		}
		folio = __read_swap_cache_async(entry, gfp_mask, mpol, ilx,
						&page_allocated, false);
		if (si)
			put_swap_device(si);
		if (!folio)
			continue;
		if (page_allocated) {
			swap_read_folio(folio, &splug);
			if (addr != vmf->address) {
				folio_set_readahead(folio);
				count_vm_event(SWAP_RA);
			}
		}
		folio_put(folio);
	}
	if (pte)
		pte_unmap(pte);
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();
skip:
	/* The folio was likely read above, so no need for plugging here */
	folio = __read_swap_cache_async(targ_entry, gfp_mask, mpol, targ_ilx,
					&page_allocated, false);
	if (unlikely(page_allocated))
		swap_read_folio(folio, NULL);
	return folio;
}

#ifdef CONFIG_HERMIT
/* Snapshot (or recheck) a forward run of swap PTEs under one PTE lock while
 * the fault keeps its VMA alive. The run is PMD-contained, so one PTE page
 * covers it. Returns the length of the leading run of valid swap PTEs.
 */
static unsigned int hermit_swap_ptes(struct vm_fault *vmf, unsigned long start,
				     unsigned int nr, pte_t *values)
{
	HERMIT_ADC_SCOPE(ADC_SG_PTE_SCAN);
	spinlock_t *ptl;
	pte_t *pte;
	unsigned int i;

	unsigned int valid;

	/* Slots past the valid run never compare pte_same() with a swap PTE. */
	for (i = 0; i < nr; i++)
		values[i] = __pte(0);
	pte = pte_offset_map_lock(vmf->vma->vm_mm, vmf->pmd, start, &ptl);
	if (!pte)
		return 0;
	for (i = 0; i < nr; i++) {
		values[i] = ptep_get(pte + i);
		if (!is_swap_pte(values[i]) ||
		    non_swap_entry(pte_to_swp_entry(values[i])))
			break;
	}
	valid = i;
	for (; i < nr; i++)
		values[i] = __pte(0);
	pte_unmap_unlock(pte, ptl);
	return valid;
}

/* Complete one polled batch. Page 0 of batch 0 is the demand page. */
static void hermit_swapin_finish(struct hermit_batch *batch, int ret,
				 const bool *stale, bool first)
{
	HERMIT_ADC_SCOPE(ADC_SG_FINISH);
	unsigned int i;

	for (i = 0; i < batch->nr; i++) {
		struct folio *f = batch->folios[i];
		bool demand = first && !i;
		int err = ret ?: batch->status[i];

		if (!demand && stale[i]) {
			/* No PTE was installed; remove the unused speculative charge. */
			swap_cache_del_folio(f);
			folio_unlock(f);
		} else if (err) {
			/* Also handles partial submission failures at base-page granularity.
			 * The remote mapping remains authoritative, never read stale disk.
			 */
			swap_read_folio(f, NULL);
		} else {
			folio_mark_uptodate(f);
			count_vm_event(PSWPIN);
			count_memcg_folio_events(f, PSWPIN, 1);
			hermit_stat_inc(HMT_STAT_BACKEND_LOAD);
			hermit_backend_account(0, false, false, 0);
			if (!demand) {
				folio_set_readahead(f);
				count_vm_event(SWAP_RA);
			}
			folio_unlock(f);
		}
		if (!demand)
			folio_put(f);
	}
}

#define HERMIT_SWAPIN_BATCHES (HERMIT_SWAPIN_MAX / HERMIT_BATCH_MAX)

struct hermit_swapin_work {
	struct hermit_direct_batch direct;
	struct hermit_batch batches[HERMIT_SWAPIN_BATCHES];
	pte_t snapshots[HERMIT_SWAPIN_MAX], now[HERMIT_SWAPIN_MAX];
	struct page *raw_pages[HERMIT_SWAPIN_MAX];
	struct folio *prep_folios[HERMIT_SWAPIN_MAX];
	swp_entry_t prep_entries[HERMIT_SWAPIN_MAX];
	unsigned long prep_addrs[HERMIT_SWAPIN_MAX];
};

void hermit_direct_batch_free(struct hermit_direct_batch *db)
{
	if (!db)
		return;
	kfree(container_of(db, struct hermit_swapin_work, direct));
}

static void hermit_release_unused_pages(struct page **pages, unsigned int from,
					unsigned int to)
{
	unsigned int i;

	for (i = from; i < to; i++)
		if (pages[i])
			folio_put(page_folio(pages[i]));
}

static unsigned int hermit_alloc_pages_bulk(struct vm_area_struct *vma,
					    unsigned long addr, gfp_t gfp,
					    unsigned int want,
					    struct page **pages)
{
	HERMIT_ADC_SCOPE(ADC_ALLOC_PAGE);
	gfp_t gfp_pref = (gfp & ~__GFP_DIRECT_RECLAIM) | __GFP_NORETRY |
			 __GFP_NOWARN;
	struct mempolicy *mpol;
	pgoff_t ilx;
	unsigned int got = 0;

	memset(pages, 0, want * sizeof(*pages));
	mpol = get_vma_policy(vma, addr, 0, &ilx);
#ifdef CONFIG_NUMA
	if (!mpol || mpol->mode == MPOL_DEFAULT)
		got = alloc_pages_bulk_node(gfp_pref, numa_node_id(), want,
					    pages);
	else
		got = alloc_pages_bulk_mempolicy(gfp_pref, want, pages);
#else
	got = alloc_pages_bulk(gfp_pref, want, pages);
#endif
	if (!got) {
		struct folio *f =
			folio_alloc_mpol(gfp, 0, mpol, ilx, numa_node_id());
		if (f) {
			pages[0] = &f->page;
			got = 1;
		}
	}
	mpol_cond_put(mpol);
	return got;
}

static void hermit_fill_batches(struct hermit_batch *batch,
				struct folio **folios,
				const swp_entry_t *entries,
				const unsigned long *addrs, unsigned int count)
{
	unsigned int i;

	memset(batch, 0,
	       DIV_ROUND_UP(count, HERMIT_BATCH_MAX) * sizeof(*batch));
	for (i = 0; i < count; i++) {
		struct hermit_batch *cur = &batch[i / HERMIT_BATCH_MAX];

		cur->folios[cur->nr] = folios[i];
		cur->entries[cur->nr] = entries[i];
		cur->addresses[cur->nr++] = addrs[i];
	}
}

struct folio *hermit_swapin_pages(swp_entry_t entry, gfp_t gfp,
				  struct vm_fault *vmf,
				  struct swap_info_struct *si,
				  unsigned int predicted,
				  struct hermit_direct_batch **direct_out)
{
	struct hermit_swapin_work *w;
	struct hermit_batch *batch, one;
	struct folio *demand = NULL;
	pte_t *snapshots, *now, demand_pte[2];
	bool stale[HERMIT_BATCH_MAX];
	int ret[HERMIT_SWAPIN_BATCHES];
	unsigned long start = vmf->address & PAGE_MASK, end;
	gfp_t gfp_pref = (gfp & ~__GFP_DIRECT_RECLAIM) | __GFP_NORETRY |
			 __GFP_NOWARN;
	unsigned int i, b, nr = predicted, want = 0, got = 0, count = 0;
	unsigned int charged, nr_batches;
	bool direct_eligible;
	u64 time;

	*direct_out = NULL;

	/* Batches and PTE snapshots are kept off the fault stack.
	 * Without memory, fall back to a single-page swapcache batch.
	 */
	w = kmalloc(sizeof(*w), GFP_NOIO | __GFP_NOWARN);
	if (!w) {
		struct mempolicy *mpol;
		pgoff_t ilx;
		bool allocated;

		batch = &one;
		snapshots = &demand_pte[0];
		now = &demand_pte[1];
		nr = hermit_swap_ptes(vmf, start, 1, snapshots);
		if (!nr || pte_to_swp_entry(snapshots[0]).val != entry.val)
			return NULL;
		memset(batch, 0, sizeof(*batch));
		mpol = get_vma_policy(vmf->vma, start, 0, &ilx);
		demand = __read_swap_cache_async(entry, gfp, mpol, ilx,
						 &allocated, false);
		mpol_cond_put(mpol);
		if (!demand || !allocated)
			return demand;
		batch->folios[0] = demand;
		batch->entries[0] = entry;
		batch->addresses[0] = start;
		batch->nr = 1;
		batch->cpu = raw_smp_processor_id();
		vmf->hermit_batch_pages = 1;
		time = ktime_get_ns();
		ret[0] = hermit_backend_submit_batch(batch);
		if (!ret[0])
			ret[0] = hermit_backend_poll_batch(batch, true);
		stale[0] = false;
		hermit_swapin_finish(batch, ret[0], stale, true);
		hermit_latency_add(HMT_LAT_BACKEND_READ, ktime_get_ns() - time);
		return demand;
	}

	batch = w->batches;
	snapshots = w->snapshots;
	now = w->now;

	/* Clip to the VMA and the PMD before touching any PTE. */
	end = min3(vmf->vma->vm_end, (start | (PMD_SIZE - 1)) + 1,
		   start + (unsigned long)clamp_t(unsigned int, nr, 1,
						  HERMIT_SWAPIN_MAX) *
				   PAGE_SIZE);
	nr = hermit_swap_ptes(vmf, start, (end - start) >> PAGE_SHIFT,
			      snapshots);
	/* Demand PTE changed under us: let do_swap_page() retry the fault. */
	if (!nr || pte_to_swp_entry(snapshots[0]).val != entry.val)
		goto out_free;

	for (want = 0; want < nr; want++) {
		swp_entry_t next = pte_to_swp_entry(snapshots[want]);

		if (swp_type(next) != swp_type(entry) ||
		    !hermit_backend_entry_remote(next))
			break;
	}
	if (!want)
		goto out_free;

	/* Bulk-allocate order-0 pages once for the whole prefix. */
	got = hermit_alloc_pages_bulk(vmf->vma, start, gfp, want, w->raw_pages);
	if (!got)
		goto out_free;
	want = min(want, got);

	/*
	 * Direct-bypass SG path: when bypass_swapcache=Y and PTEBATCH is ready
	 * to map the prefetched pages immediately under the demand fault's PTE
	 * lock, keep exclusive folios out of swapcache altogether, charge memcg
	 * once per batch, and overlap LRU/shadow metadata with in-flight RDMA.
	 */
	direct_eligible = want >= 2 && hmt_ctl_flag(HMT_BPS_SCACHE) &&
			  !unlikely(userfaultfd_armed(vmf->vma)) &&
			  vmf->vma->anon_vma &&
			  hermit_policy_pte_install_ready(vmf, want);

	if (direct_eligible) {
		struct hermit_direct_batch *db = &w->direct;
		unsigned int ok_pages = 0;

		for (i = 0; i < want; i++) {
			swp_entry_t next = pte_to_swp_entry(snapshots[i]);
			struct folio *f;

			if (__swap_count(next) != 1)
				break;
			if (swapcache_prepare(next, 1))
				break;
			f = page_folio(w->raw_pages[i]);
			__folio_set_locked(f);
			__folio_set_swapbacked(f);
			f->swap = next;
			db->folios[count] = f;
			db->entries[count] = next;
			db->snapshots[count] = snapshots[i];
			db->ok[count] = false;
			w->prep_addrs[count] =
				start + (unsigned long)i * PAGE_SIZE;
			count++;
		}

		if (count >= 2) {
			charged = mem_cgroup_swapin_charge_batch(
				db->folios, db->entries, count, vmf->vma->vm_mm,
				gfp_pref);
			if (!charged && !mem_cgroup_swapin_charge_folio(
						db->folios[0], vmf->vma->vm_mm,
						gfp, db->entries[0]))
				charged = 1;
			for (i = charged; i < count; i++) {
				db->folios[i]->private = NULL;
				swapcache_clear(si, db->entries[i], 1);
				folio_unlock(db->folios[i]);
			}
			hermit_release_unused_pages(w->raw_pages, charged, got);
			count = charged;
			if (!count)
				goto out_free;

			vmf->hermit_batch_pages = count;
			nr_batches = DIV_ROUND_UP(count, HERMIT_BATCH_MAX);
			hermit_fill_batches(batch, db->folios, db->entries,
					    w->prep_addrs, count);

			if (hermit_swapin_cost_enabled() && count > 1) {
				for (i = 1; i < count; i++)
					hermit_policy_prefetched(
						vmf->vma->vm_mm,
						db->entries[i].val);
				hermit_swapin_cost_prefetched(count - 1);
			}

			time = ktime_get_ns();
			for (b = 0; b < nr_batches; b++) {
				batch[b].cpu = raw_smp_processor_id();
				ret[b] = hermit_backend_submit_batch(&batch[b]);
			}

			/* Overlap shadow/refault/LRU setup with in-flight RDMA. */
			for (i = 0; i < count; i++) {
				void *shadow;

				memcg1_swapin(db->entries[i], 1);
				shadow = swap_cache_get_shadow(db->entries[i]);
				if (shadow)
					workingset_refault(db->folios[i],
							   shadow);
				folio_add_lru(db->folios[i]);
			}

			for (b = 0; b < nr_batches; b++)
				if (!ret[b])
					ret[b] = hermit_backend_poll_batch(
						&batch[b], true);

			for (b = 0; b < nr_batches; b++) {
				for (i = 0; i < batch[b].nr; i++) {
					unsigned int k =
						b * HERMIT_BATCH_MAX + i;
					struct folio *f = db->folios[k];
					int err = ret[b] ?: batch[b].status[i];

					if (!err) {
						f->private = NULL;
						folio_mark_uptodate(f);
						db->ok[k] = true;
						ok_pages++;
						count_memcg_folio_events(
							f, PSWPIN, 1);
						hermit_backend_account(
							0, false, false, 0);
					} else if (k == 0) {
						/* Demand fallback: swap_read_folio unlocks f. */
						swap_read_folio(f, NULL);
						f->private = NULL;
						db->ok[0] = false;
					} else {
						f->private = NULL;
						db->ok[k] = false;
					}
				}
			}
			if (ok_pages) {
				count_vm_events(PSWPIN, ok_pages);
				hermit_stat_add(HMT_STAT_BACKEND_LOAD,
						ok_pages);
			}
			hermit_latency_add(HMT_LAT_BACKEND_READ,
					   ktime_get_ns() - time);

			/* Demand page unlocks so do_swap_page's folio_lock_or_retry()
			 * acquires it cleanly; tail folios stay locked until mapped
			 * or released after PTL unlock.
			 */
			if (db->ok[0])
				folio_unlock(db->folios[0]);
			db->nr = count;
			db->installed = 0;
			*direct_out = db;
			return db->folios[0];
		}

		/* Fewer than 2 exclusive uncached slots: undo and reuse raw_pages
		 * on the swapcache SG path below.
		 */
		for (i = 0; i < count; i++) {
			db->folios[i]->private = NULL;
			swapcache_clear(si, db->entries[i], 1);
			folio_unlock(db->folios[i]);
		}
		count = 0;
	}

	/*
	 * Optimized swapcache SG path: uses the bulk-allocated pages and a
	 * single batch memcg charge instead of per-page __read_swap_cache_async().
	 */
	for (i = 0; i < want; i++) {
		swp_entry_t next = pte_to_swp_entry(snapshots[i]);
		int err = swapcache_prepare(next, 1);

		if (err) {
			if (i == 0) {
				struct mempolicy *mpol;
				pgoff_t ilx;
				bool allocated;

				hermit_release_unused_pages(w->raw_pages, 0,
							    got);
				mpol = get_vma_policy(vmf->vma, start, 0, &ilx);
				demand = __read_swap_cache_async(entry, gfp,
								 mpol, ilx,
								 &allocated,
								 false);
				mpol_cond_put(mpol);
				if (!demand || !allocated)
					goto out_free;
				w->prep_folios[0] = demand;
				w->prep_entries[0] = entry;
				w->prep_addrs[0] = start;
				count = 1;
				got = 0;
				goto submit_swapcache;
			}
			break;
		}
		w->prep_folios[count] = page_folio(w->raw_pages[i]);
		__folio_set_locked(w->prep_folios[count]);
		__folio_set_swapbacked(w->prep_folios[count]);
		w->prep_entries[count] = next;
		w->prep_addrs[count] = start + (unsigned long)i * PAGE_SIZE;
		count++;
	}

	if (!count) {
		hermit_release_unused_pages(w->raw_pages, 0, got);
		goto out_free;
	}

	charged = mem_cgroup_swapin_charge_batch(w->prep_folios,
						 w->prep_entries, count,
						 vmf->vma->vm_mm, gfp_pref);
	if (!charged &&
	    !mem_cgroup_swapin_charge_folio(w->prep_folios[0], vmf->vma->vm_mm,
					    gfp, w->prep_entries[0]))
		charged = 1;
	for (i = charged; i < count; i++) {
		swapcache_clear(si, w->prep_entries[i], 1);
		folio_unlock(w->prep_folios[i]);
	}
	hermit_release_unused_pages(w->raw_pages, charged, got);
	count = charged;
	if (!count)
		goto out_free;

	for (i = 0; i < count; i++) {
		void *shadow = NULL;

		swap_cache_add_folio(w->prep_folios[i], w->prep_entries[i],
				     &shadow);
		memcg1_swapin(w->prep_entries[i], 1);
		if (shadow)
			workingset_refault(w->prep_folios[i], shadow);
		folio_add_lru(w->prep_folios[i]);
	}
	demand = w->prep_folios[0];

submit_swapcache:
	/* The pages actually read, not the prediction: allocation failure,
	 * swapcache hits and PTE holes shorten a batch. Run-length feedback in
	 * hermit_policy_installed() measures use against this.
	 */
	vmf->hermit_batch_pages = count;
	nr_batches = DIV_ROUND_UP(count, HERMIT_BATCH_MAX);
	hermit_fill_batches(batch, w->prep_folios, w->prep_entries,
			    w->prep_addrs, count);
	/* Cost instrumentation (debugfs swapin_cost_enable): remember the pages
	 * this fault reads for other addresses, so a later demand fault that
	 * finds one in swapcache is reported as a prefetch hit. Only the demand
	 * page (batch 0, page 0) is excluded.
	 */
	if (hermit_swapin_cost_enabled() && count > 1) {
		for (b = 0; b < nr_batches; b++)
			for (i = b ? 0 : 1; i < batch[b].nr; i++)
				hermit_policy_prefetched(
					vmf->vma->vm_mm,
					batch[b].entries[i].val);
		hermit_swapin_cost_prefetched(count - 1);
	}
	time = ktime_get_ns();
	/* Every batch is in flight before the fault waits on the first. */
	for (b = 0; b < nr_batches; b++) {
		batch[b].cpu = raw_smp_processor_id();
		ret[b] = hermit_backend_submit_batch(&batch[b]);
	}
	for (b = 0; b < nr_batches; b++)
		if (!ret[b])
			ret[b] = hermit_backend_poll_batch(&batch[b], true);
	/* One PTE-lock pass after completion decides which speculative pages
	 * became stale while their reads were in flight.
	 */
	hermit_swap_ptes(vmf, start, count, now);
	for (b = 0; b < nr_batches; b++) {
		for (i = 0; i < batch[b].nr; i++) {
			unsigned int k = b * HERMIT_BATCH_MAX + i;

			stale[i] = !pte_same(now[k], snapshots[k]);
		}
		hermit_swapin_finish(&batch[b], ret[b], stale, !b);
	}
	hermit_latency_add(HMT_LAT_BACKEND_READ, ktime_get_ns() - time);
out_free:
	kfree(w);
	return demand;
}
#endif

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct folio *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
			       struct vm_fault *vmf)
{
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;

	mpol = get_vma_policy(vmf->vma, vmf->address, 0, &ilx);
	folio = swap_use_vma_readahead() ?
			swap_vma_readahead(entry, gfp_mask, mpol, ilx, vmf) :
			swap_cluster_readahead(entry, gfp_mask, mpol, ilx);
	mpol_cond_put(mpol);

	return folio;
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", str_true_false(enable_vma_readahead));
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &enable_vma_readahead);
	if (ret)
		return ret;

	return count;
}
static struct kobj_attribute vma_ra_enabled_attr = __ATTR_RW(vma_ra_enabled);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	/* Swap cache writeback is LRU based, no tags for it */
	mapping_set_no_writeback_tags(&swap_space);
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init);
#endif
