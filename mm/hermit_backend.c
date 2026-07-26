// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/hermit_backend.h>
#include <linux/huge_mm.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/swapops.h>
#include <linux/xarray.h>

static const struct hermit_backend_ops __rcu *hermit_backend_ops;
static DEFINE_MUTEX(hermit_backend_lock);
static DEFINE_XARRAY(hermit_remote_entries);
static unsigned long hermit_remote_order_mask = BIT(0);
static atomic_long_t hermit_order_stores[PMD_ORDER + 1];
static atomic_long_t hermit_order_loads[PMD_ORDER + 1];
static atomic_long_t hermit_order_fallbacks[PMD_ORDER + 1];
static atomic_long_t hermit_order_errors[PMD_ORDER + 1];

#define HERMIT_LARGE_ORDER_MASK (BIT(0) | GENMASK(PMD_ORDER, 2))

int hermit_register_backend(const struct hermit_backend_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->load || !ops->store ||
	    !(ops->supported_order_mask & BIT(0)))
		return -EINVAL;
	mutex_lock(&hermit_backend_lock);
	if (rcu_access_pointer(hermit_backend_ops))
		ret = -EBUSY;
	else
		rcu_assign_pointer(hermit_backend_ops, ops);
	mutex_unlock(&hermit_backend_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_register_backend);

void hermit_unregister_backend(const struct hermit_backend_ops *ops)
{
	mutex_lock(&hermit_backend_lock);
	if (rcu_access_pointer(hermit_backend_ops) == ops)
		RCU_INIT_POINTER(hermit_backend_ops, NULL);
	mutex_unlock(&hermit_backend_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(hermit_unregister_backend);

bool hermit_backend_ready(void)
{
	const struct hermit_backend_ops *ops;
	bool ready;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	ready = ops && ops->load && ops->store;
	rcu_read_unlock();
	return ready;
}
EXPORT_SYMBOL_GPL(hermit_backend_ready);

#define HERMIT_BACKEND_CALL(_member, _fallback, ...) ({ \
	const struct hermit_backend_ops *_ops; \
	int _ret = (_fallback); \
	rcu_read_lock(); \
	_ops = rcu_dereference(hermit_backend_ops); \
	if (_ops && _ops->_member) \
		_ret = _ops->_member(__VA_ARGS__); \
	rcu_read_unlock(); \
	_ret; \
})

int hermit_backend_load(struct hermit_io *io, bool async)
{
	return HERMIT_BACKEND_CALL(load, -EOPNOTSUPP, io, async);
}
EXPORT_SYMBOL_GPL(hermit_backend_load);

int hermit_backend_store(struct hermit_io *io)
{
	return HERMIT_BACKEND_CALL(store, -EOPNOTSUPP, io);
}
EXPORT_SYMBOL_GPL(hermit_backend_store);

int hermit_backend_poll(struct hermit_io *io, bool wait)
{
	return HERMIT_BACKEND_CALL(poll, 0, io, wait);
}
EXPORT_SYMBOL_GPL(hermit_backend_poll);

unsigned long hermit_backend_effective_order_mask(void)
{
	const struct hermit_backend_ops *ops;
	unsigned long mask = BIT(0);

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops)
		mask = READ_ONCE(hermit_remote_order_mask) &
			ops->supported_order_mask;
	rcu_read_unlock();
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

int hermit_backend_prepare_remote(swp_entry_t entry, unsigned int order)
{
	unsigned int i, nr_pages = 1U << order;
	int ret;

	for (i = 0; i < nr_pages; i++) {
		ret = xa_reserve(&hermit_remote_entries,
				 hermit_entry_offset(entry, i).val, GFP_NOIO);
		if (ret)
			goto rollback;
	}
	return 0;

rollback:
	while (i--)
		xa_release(&hermit_remote_entries,
			   hermit_entry_offset(entry, i).val);
	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_prepare_remote);

int hermit_backend_commit_remote(swp_entry_t entry, unsigned int order)
{
	unsigned int i, nr_pages = 1U << order;
	void *old;

	for (i = 0; i < nr_pages; i++) {
		old = xa_store(&hermit_remote_entries,
			       hermit_entry_offset(entry, i).val,
			       xa_mk_value(order + 1), GFP_NOWAIT);
		if (WARN_ON_ONCE(xa_is_err(old))) {
			hermit_backend_abort_remote(entry, order);
			return xa_err(old);
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(hermit_backend_commit_remote);

void hermit_backend_abort_remote(swp_entry_t entry, unsigned int order)
{
	unsigned int i;

	for (i = 0; i < (1U << order); i++)
		xa_erase(&hermit_remote_entries,
			 hermit_entry_offset(entry, i).val);
}
EXPORT_SYMBOL_GPL(hermit_backend_abort_remote);

int hermit_backend_entry_order(swp_entry_t entry)
{
	void *marker = xa_load(&hermit_remote_entries, entry.val);

	if (!marker || !xa_is_value(marker))
		return -ENOENT;
	return xa_to_value(marker) - 1;
}
EXPORT_SYMBOL_GPL(hermit_backend_entry_order);

bool hermit_backend_entry_remote(swp_entry_t entry)
{
	return hermit_backend_entry_order(entry) >= 0;
}
EXPORT_SYMBOL_GPL(hermit_backend_entry_remote);

bool hermit_backend_range_remote(swp_entry_t entry, unsigned int order)
{
	unsigned long extent_head;
	unsigned int i;
	int extent_order;

	extent_order = hermit_backend_entry_order(entry);
	if (extent_order < 0 || order > extent_order)
		return false;
	extent_head = swp_offset(entry) & ~((1UL << extent_order) - 1);
	if (swp_offset(entry) + (1UL << order) >
	    extent_head + (1UL << extent_order))
		return false;
	for (i = 0; i < (1U << order); i++)
		if (hermit_backend_entry_order(hermit_entry_offset(entry, i)) !=
		    extent_order)
			return false;
	return true;
}
EXPORT_SYMBOL_GPL(hermit_backend_range_remote);

void hermit_backend_invalidate(swp_entry_t entry)
{
	xa_erase(&hermit_remote_entries, entry.val);
}
EXPORT_SYMBOL_GPL(hermit_backend_invalidate);

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
