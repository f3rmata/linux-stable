// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/hermit_backend.h>
#include <linux/huge_mm.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/srcu.h>
#include <linux/swapops.h>
#include <linux/xarray.h>

static const struct hermit_backend_ops __rcu *hermit_backend_ops;
static DEFINE_MUTEX(hermit_backend_lock);
DEFINE_STATIC_SRCU(hermit_backend_srcu);
static DEFINE_XARRAY(hermit_remote_entries);
struct hermit_remote_page {
	unsigned long offset;
	u64 generation;
	unsigned int order;
	bool valid;
};
/* Protected together with the logical map by hermit_remote_entries.xa_lock. */
static unsigned long *hermit_remote_used;
static unsigned long hermit_remote_capacity, hermit_remote_boundary;
static unsigned long hermit_remote_cursor;
static u64 hermit_remote_generation;

static unsigned long hermit_remote_order_mask = BIT(0);
static atomic_long_t hermit_order_stores[PMD_ORDER + 1];
static atomic_long_t hermit_order_loads[PMD_ORDER + 1];
static atomic_long_t hermit_order_fallbacks[PMD_ORDER + 1];
static atomic_long_t hermit_order_errors[PMD_ORDER + 1];

#define HERMIT_LARGE_ORDER_MASK (BIT(0) | GENMASK(PMD_ORDER, 2))

int hermit_register_backend(const struct hermit_backend_ops *ops)
{
	int ret = 0;

	/*
	 * ->poll is required: the speculative swapin path
	 * (hermit_swap_read_folio_poll) always polls an async load, and
	 * silently succeeding without ->poll would mark an in-flight folio
	 * uptodate.
	 */
	if (!ops || !ops->load || !ops->store || !ops->poll ||
	    !(ops->supported_order_mask & BIT(0)) || !ops->capacity_pages ||
	    !is_power_of_2(ops->boundary_pages) ||
	    !ops->submit_batch || !ops->poll_batch)
		return -EINVAL;
	mutex_lock(&hermit_backend_lock);
	if (rcu_access_pointer(hermit_backend_ops))
		ret = -EBUSY;
	else {
		unsigned long *old, *used = kvcalloc(BITS_TO_LONGS(ops->capacity_pages),
						 sizeof(unsigned long), GFP_KERNEL);

		if (!used) {
			ret = -ENOMEM;
		} else {
			xa_lock(&hermit_remote_entries);
			old = hermit_remote_used;
			hermit_remote_used = used;
			hermit_remote_capacity = ops->capacity_pages;
			hermit_remote_boundary = ops->boundary_pages;
			hermit_remote_cursor = 0;
			hermit_remote_generation++;
			xa_unlock(&hermit_remote_entries);
			kvfree(old);
			rcu_assign_pointer(hermit_backend_ops, ops);
		}
	}
	mutex_unlock(&hermit_backend_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_register_backend);

void hermit_unregister_backend(const struct hermit_backend_ops *ops)
{
	mutex_lock(&hermit_backend_lock);
	if (rcu_access_pointer(hermit_backend_ops) == ops)
		RCU_INIT_POINTER(hermit_backend_ops, NULL);
	/* Do not replace the physical allocator while an old async lease lives. */
	synchronize_srcu(&hermit_backend_srcu);
	mutex_unlock(&hermit_backend_lock);
}
EXPORT_SYMBOL_GPL(hermit_unregister_backend);

bool hermit_backend_ready(void)
{
	const struct hermit_backend_ops *ops;
	bool ready;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	ready = ops && ops->load && ops->store;
	srcu_read_unlock(&hermit_backend_srcu, idx);
	return ready;
}
EXPORT_SYMBOL_GPL(hermit_backend_ready);

#define HERMIT_BACKEND_CALL(_member, _fallback, ...) ({ \
	const struct hermit_backend_ops *_ops; \
	int _ret = (_fallback); \
	int _idx = srcu_read_lock(&hermit_backend_srcu); \
	_ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu); \
	if (_ops && _ops->_member) \
		_ret = _ops->_member(__VA_ARGS__); \
	srcu_read_unlock(&hermit_backend_srcu, _idx); \
	_ret; \
})

static int hermit_io_offset(struct hermit_io *io, bool write);

int hermit_backend_load(struct hermit_io *io, bool async)
{
	const struct hermit_backend_ops *ops;
	int ret, idx = srcu_read_lock(&hermit_backend_srcu);

	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	ret = ops ? hermit_io_offset(io, false) : -ENODEV;
	if (!ret)
		ret = ops->load(io, async);
	if (!ret && async) {
		io->backend = ops;
		io->srcu_idx = idx;
	} else {
		srcu_read_unlock(&hermit_backend_srcu, idx);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_load);

int hermit_backend_store(struct hermit_io *io)
{
	const struct hermit_backend_ops *ops;
	int ret, idx = srcu_read_lock(&hermit_backend_srcu);

	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	ret = ops ? hermit_io_offset(io, true) : -ENODEV;
	if (!ret)
		ret = ops->store(io);
	srcu_read_unlock(&hermit_backend_srcu, idx);
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_store);

int hermit_backend_poll(struct hermit_io *io, bool wait)
{
	int ret;

	if (!io->backend)
		return -EINVAL;
	ret = io->backend->poll(io, wait);
	if (ret != -EAGAIN) {
		io->backend = NULL;
		srcu_read_unlock(&hermit_backend_srcu, io->srcu_idx);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_poll);

int hermit_backend_submit_batch(struct hermit_batch *batch)
{
	const struct hermit_backend_ops *ops;
	int idx, ret = 0;
	unsigned int i;

	if (!batch->nr || batch->nr > HERMIT_BATCH_MAX)
		return -EINVAL;
	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (!ops) {
		ret = -ENODEV;
		goto out;
	}
	for (i = 0; i < batch->nr; i++) {
		struct hermit_io io = { .entry = batch->entries[i] };

		ret = hermit_io_offset(&io, batch->write);
		if (ret)
			goto out;
		batch->offsets[i] = io.remote_offset;
		batch->status[i] = -EINPROGRESS;
	}
	ret = ops->submit_batch(batch);
	if (!ret) {
		batch->backend = ops;
		batch->srcu_idx = idx;
		return 0;
	}
out:
	/* submit_batch must drain any partial submission on error. */
	for (i = 0; i < batch->nr; i++)
		batch->status[i] = ret;
	srcu_read_unlock(&hermit_backend_srcu, idx);
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_submit_batch);

int hermit_backend_poll_batch(struct hermit_batch *batch, bool wait)
{
	int ret;

	if (!batch->backend)
		return -EINVAL;
	ret = batch->backend->poll_batch(batch, wait);
	if (ret != -EAGAIN) {
		batch->backend = NULL;
		srcu_read_unlock(&hermit_backend_srcu, batch->srcu_idx);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_poll_batch);

unsigned long hermit_backend_effective_order_mask(void)
{
	const struct hermit_backend_ops *ops;
	unsigned long mask = BIT(0);
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops)
		mask = READ_ONCE(hermit_remote_order_mask) &
			ops->supported_order_mask;
	srcu_read_unlock(&hermit_backend_srcu, idx);
	return mask | BIT(0);
}
EXPORT_SYMBOL_GPL(hermit_backend_effective_order_mask);

unsigned int hermit_backend_transfer_order(unsigned int folio_order)
{
	if (folio_order <= PMD_ORDER &&
	    (hermit_backend_effective_order_mask() & BIT(folio_order)))
		return folio_order;
	return 0;
}
EXPORT_SYMBOL_GPL(hermit_backend_transfer_order);

static swp_entry_t hermit_entry_offset(swp_entry_t entry, unsigned int index)
{
	return swp_entry(swp_type(entry), swp_offset(entry) + index);
}

/* The caller holds the swapcache folio locks (or HAS_CACHE reservations).
 * No reader can observe an entry being rewritten or freed during an I/O.
 * Pending entries are deliberately invisible to remote readers.
 */
static int hermit_prepare_entries(swp_entry_t *entries, unsigned int nr,
				 unsigned int order)
{
	struct hermit_remote_page **pages, *old;
	unsigned long start, align = roundup_pow_of_two(nr) - 1;
	unsigned int i, reserved = 0;
	int ret = -ENOMEM;

	pages = kcalloc(nr, sizeof(*pages), GFP_NOIO);
	if (!pages)
		return ret;
	for (i = 0; i < nr; i++) {
		pages[i] = kzalloc(sizeof(**pages), GFP_NOIO);
		if (!pages[i]) {
			ret = -ENOMEM;
			goto out;
		}
		ret = xa_reserve(&hermit_remote_entries, entries[i].val, GFP_NOIO);
		if (ret)
			goto out;
		reserved++;
	}
	xa_lock(&hermit_remote_entries);
	if (!hermit_remote_used || nr > hermit_remote_boundary) {
		ret = -ENOSPC;
		goto unlock;
	}
	start = bitmap_find_next_zero_area(hermit_remote_used,
		hermit_remote_capacity, hermit_remote_cursor, nr, align);
	if (start >= hermit_remote_capacity)
		start = bitmap_find_next_zero_area(hermit_remote_used,
			hermit_remote_capacity, 0, nr, align);
	if (start >= hermit_remote_capacity) {
		ret = -ENOSPC;
		goto unlock;
	}
	bitmap_set(hermit_remote_used, start, nr);
	hermit_remote_cursor = start + nr;
	for (i = 0; i < nr; i++) {
		pages[i]->offset = start + i;
		pages[i]->generation = hermit_remote_generation;
		pages[i]->order = order;
		old = __xa_store(&hermit_remote_entries, entries[i].val,
				 pages[i], GFP_NOWAIT);
		/* All nodes were reserved above while the entry was pinned. */
		if (WARN_ON_ONCE(xa_is_err(old))) {
			bitmap_clear(hermit_remote_used, start + i, nr - i);
			ret = xa_err(old);
			goto unlock;
		}
		if (old) {
			if (old->generation == hermit_remote_generation)
				__clear_bit(old->offset, hermit_remote_used);
			kfree(old);
		}
		pages[i] = NULL;
	}
	ret = 0;
unlock:
	xa_unlock(&hermit_remote_entries);
out:
	for (i = 0; i < reserved; i++)
		xa_release(&hermit_remote_entries, entries[i].val);
	for (i = 0; i < nr; i++)
		kfree(pages[i]);
	kfree(pages);
	return ret;
}

int hermit_backend_prepare_remote(swp_entry_t entry, unsigned int order)
{
	swp_entry_t *entries;
	unsigned int i, nr;
	int ret;

	if (order > PMD_ORDER)
		return -EINVAL;
	nr = 1U << order;
	entries = kmalloc_array(nr, sizeof(*entries), GFP_NOIO);
	if (!entries)
		return -ENOMEM;
	for (i = 0; i < nr; i++)
		entries[i] = hermit_entry_offset(entry, i);
	ret = hermit_prepare_entries(entries, nr, order);
	kfree(entries);
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_prepare_remote);

int hermit_backend_prepare_batch(struct hermit_batch *batch)
{
	if (!batch->nr || batch->nr > HERMIT_BATCH_MAX || !batch->write)
		return -EINVAL;
	return hermit_prepare_entries(batch->entries, batch->nr, 0);
}
EXPORT_SYMBOL_GPL(hermit_backend_prepare_batch);

int hermit_backend_commit_remote(swp_entry_t entry, unsigned int order)
{
	struct hermit_remote_page *p;
	unsigned int i;
	int ret = 0;

	xa_lock(&hermit_remote_entries);
	for (i = 0; i < (1U << order); i++) {
		p = xa_load(&hermit_remote_entries, hermit_entry_offset(entry, i).val);
		if (!p || p->generation != hermit_remote_generation) {
			ret = -ESTALE;
			break;
		}
		p->valid = true;
	}
	xa_unlock(&hermit_remote_entries);
	if (ret)
		hermit_backend_abort_remote(entry, order);
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_commit_remote);

void hermit_backend_invalidate(swp_entry_t entry)
{
	struct hermit_remote_page *p;

	xa_lock(&hermit_remote_entries);
	p = __xa_erase(&hermit_remote_entries, entry.val);
	if (p) {
		if (p->generation == hermit_remote_generation)
			__clear_bit(p->offset, hermit_remote_used);
		kfree(p);
	}
	xa_unlock(&hermit_remote_entries);
}
EXPORT_SYMBOL_GPL(hermit_backend_invalidate);

void hermit_backend_abort_remote(swp_entry_t entry, unsigned int order)
{
	unsigned int i;

	for (i = 0; i < (1U << order); i++)
		hermit_backend_invalidate(hermit_entry_offset(entry, i));
}
EXPORT_SYMBOL_GPL(hermit_backend_abort_remote);

int hermit_backend_entry_order(swp_entry_t entry)
{
	struct hermit_remote_page *p;
	int order;

	xa_lock(&hermit_remote_entries);
	p = xa_load(&hermit_remote_entries, entry.val);
	/* Old backend generations remain remote-only, never local fallback. */
	order = p && p->valid ? p->order : -ENOENT;
	xa_unlock(&hermit_remote_entries);
	return order;
}
EXPORT_SYMBOL_GPL(hermit_backend_entry_order);

bool hermit_backend_entry_remote(swp_entry_t entry)
{
	return hermit_backend_entry_order(entry) >= 0;
}
EXPORT_SYMBOL_GPL(hermit_backend_entry_remote);

static int hermit_io_offset(struct hermit_io *io, bool write)
{
	struct hermit_remote_page *p;
	unsigned int i;
	int ret = 0;

	xa_lock(&hermit_remote_entries);
	for (i = 0; i < (1U << io->folio_order); i++) {
		p = xa_load(&hermit_remote_entries, hermit_entry_offset(io->entry, i).val);
		if (!p || (!write && !p->valid) ||
		    p->generation != hermit_remote_generation) {
			ret = -ESTALE;
			break;
		}
		if (!i)
			io->remote_offset = p->offset;
		else if (p->offset != io->remote_offset + i) {
			ret = -ERANGE;
			break;
		}
	}
	xa_unlock(&hermit_remote_entries);
	return ret;
}

int hermit_backend_offset(swp_entry_t entry, unsigned long *offset)
{
	struct hermit_io io = { .entry = entry };
	int ret = hermit_io_offset(&io, false);

	if (!ret)
		*offset = io.remote_offset;
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_offset);

bool hermit_backend_range_remote(swp_entry_t entry, unsigned int order)
{
	unsigned int i;

	/* Coverage is independent of the order of the original WRITE. */
	for (i = 0; i < (1U << order); i++)
		if (!hermit_backend_entry_remote(hermit_entry_offset(entry, i)))
			return false;
	return true;
}
EXPORT_SYMBOL_GPL(hermit_backend_range_remote);

void hermit_backend_account(unsigned int order, bool store, bool fallback,
			    int error)
{
	if (order > PMD_ORDER)
		return;
	if (fallback)
		atomic_long_inc(&hermit_order_fallbacks[order]);
	if (error)
		atomic_long_inc(&hermit_order_errors[order]);
	else if (store)
		atomic_long_inc(&hermit_order_stores[order]);
	else
		atomic_long_inc(&hermit_order_loads[order]);
}
EXPORT_SYMBOL_GPL(hermit_backend_account);

static int hermit_order_mask_get(void *data, u64 *value)
{
	*value = READ_ONCE(hermit_remote_order_mask);
	return 0;
}

static int hermit_order_mask_set(void *data, u64 value)
{
	if (!(value & BIT(0)) || value & ~HERMIT_LARGE_ORDER_MASK)
		return -EINVAL;
	WRITE_ONCE(hermit_remote_order_mask, (unsigned long)value);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(hermit_order_mask_fops, hermit_order_mask_get,
			 hermit_order_mask_set, "0x%llx\n");

static int hermit_effective_mask_get(void *data, u64 *value)
{
	*value = hermit_backend_effective_order_mask();
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(hermit_effective_mask_fops,
			 hermit_effective_mask_get, NULL, "0x%llx\n");

static int hermit_order_stats_show(struct seq_file *m, void *unused)
{
	unsigned int order;

	seq_puts(m, "order size_bytes stores loads fallback_4k errors\n");
	for (order = 0; order <= PMD_ORDER; order++) {
		if (order == 1)
			continue;
		seq_printf(m, "%u %lu %ld %ld %ld %ld\n", order,
			   PAGE_SIZE << order,
			   atomic_long_read(&hermit_order_stores[order]),
			   atomic_long_read(&hermit_order_loads[order]),
			   atomic_long_read(&hermit_order_fallbacks[order]),
			   atomic_long_read(&hermit_order_errors[order]));
	}
	return 0;
}

static int hermit_order_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, hermit_order_stats_show, inode->i_private);
}

static const struct file_operations hermit_order_stats_fops = {
	.owner = THIS_MODULE,
	.open = hermit_order_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int hermit_backend_debugfs_init(struct dentry *root)
{
	debugfs_create_file("remote_order_mask", 0600, root, NULL,
			    &hermit_order_mask_fops);
	debugfs_create_file("effective_order_mask", 0400, root, NULL,
			    &hermit_effective_mask_fops);
	debugfs_create_file("order_stats", 0400, root, NULL,
			    &hermit_order_stats_fops);
	return 0;
}
