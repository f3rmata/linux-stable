// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/hermit_backend.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/xarray.h>

static const struct hermit_backend_ops __rcu *hermit_backend_ops;
static DEFINE_MUTEX(hermit_backend_lock);
static DEFINE_XARRAY(hermit_remote_entries);

int hermit_register_backend(const struct hermit_backend_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->load || !ops->store)
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

int hermit_backend_load(swp_entry_t entry, struct page *page, int cpu,
			bool async)
{
	return HERMIT_BACKEND_CALL(load, -EOPNOTSUPP, entry, page, cpu, async);
}
EXPORT_SYMBOL_GPL(hermit_backend_load);

int hermit_backend_store(swp_entry_t entry, struct page *page, int cpu,
			 bool async)
{
	return HERMIT_BACKEND_CALL(store, -EOPNOTSUPP, entry, page, cpu, async);
}
EXPORT_SYMBOL_GPL(hermit_backend_store);

int hermit_backend_poll_load(int cpu)
{
	return HERMIT_BACKEND_CALL(poll_load, 0, cpu);
}
EXPORT_SYMBOL_GPL(hermit_backend_poll_load);

int hermit_backend_peek_load(int cpu)
{
	return HERMIT_BACKEND_CALL(peek_load, 0, cpu);
}
EXPORT_SYMBOL_GPL(hermit_backend_peek_load);

int hermit_backend_mark_remote(swp_entry_t entry)
{
	void *old;

	old = xa_store(&hermit_remote_entries, entry.val, xa_mk_value(1),
		       GFP_ATOMIC);
	return xa_err(old);
}
EXPORT_SYMBOL_GPL(hermit_backend_mark_remote);

bool hermit_backend_entry_remote(swp_entry_t entry)
{
	return xa_load(&hermit_remote_entries, entry.val) != NULL;
}
EXPORT_SYMBOL_GPL(hermit_backend_entry_remote);

void hermit_backend_invalidate(swp_entry_t entry)
{
	xa_erase(&hermit_remote_entries, entry.val);
}
EXPORT_SYMBOL_GPL(hermit_backend_invalidate);
