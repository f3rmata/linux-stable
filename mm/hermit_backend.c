#include <linux/hermit_backend.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>

static const struct hermit_backend_ops __rcu *hermit_backend_ops;
static DEFINE_MUTEX(hermit_backend_lock);
DEFINE_STATIC_SRCU(hermit_backend_srcu);

int hermit_register_backend(const struct hermit_backend_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->load)
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

int hermit_regsiter_backend(const struct hermit_backend_ops *ops)
{
	return hermit_register_backend(ops);
}
EXPORT_SYMBOL_GPL(hermit_regsiter_backend);

void hermit_unregister_backend(const struct hermit_backend_ops *ops)
{
	mutex_lock(&hermit_backend_lock);
	if (rcu_access_pointer(hermit_backend_ops) == ops)
		RCU_INIT_POINTER(hermit_backend_ops, NULL);
	mutex_unlock(&hermit_backend_lock);

	synchronize_srcu(&hermit_backend_srcu);
}
EXPORT_SYMBOL_GPL(hermit_unregister_backend);

bool hermit_backend_ready(void)
{
	const struct hermit_backend_ops *ops;
	bool ready;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	ready = ops && ops->load;
	srcu_read_unlock(&hermit_backend_srcu, idx);

	return ready;
}
EXPORT_SYMBOL_GPL(hermit_backend_ready);

int hermit_backend_load(swp_entry_t entry, struct page *page, int cpu,
			bool async)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->load)
		ret = ops->load(entry, page, cpu, async);
	srcu_read_unlock(&hermit_backend_srcu, idx);

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_load);

int hermit_backend_store(swp_entry_t entry, struct page *page, int cpu,
			 bool async)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->store)
		ret = ops->store(entry, page, cpu, async);
	srcu_read_unlock(&hermit_backend_srcu, idx);

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_store);

void hermit_backend_invalidate_page(swp_entry_t entry)
{
	const struct hermit_backend_ops *ops;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->invalidate_page)
		ops->invalidate_page(entry);
	srcu_read_unlock(&hermit_backend_srcu, idx);
}
EXPORT_SYMBOL_GPL(hermit_backend_invalidate_page);

void hermit_backend_invalidate_area(unsigned int type)
{
	const struct hermit_backend_ops *ops;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->invalidate_area)
		ops->invalidate_area(type);
	srcu_read_unlock(&hermit_backend_srcu, idx);
}
EXPORT_SYMBOL_GPL(hermit_backend_invalidate_area);

int hermit_backend_poll_load(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->poll_load)
		ret = ops->poll_load(cpu);
	srcu_read_unlock(&hermit_backend_srcu, idx);

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_poll_load);

int hermit_backend_peek_load(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->peek_load)
		ret = ops->peek_load(cpu);
	srcu_read_unlock(&hermit_backend_srcu, idx);

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_peek_load);

int hermit_backend_poll_store(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = 0;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->poll_store)
		ret = ops->poll_store(cpu);
	srcu_read_unlock(&hermit_backend_srcu, idx);

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_poll_store);

int hermit_backend_peek_store(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;
	int idx;

	idx = srcu_read_lock(&hermit_backend_srcu);
	ops = srcu_dereference(hermit_backend_ops, &hermit_backend_srcu);
	if (ops && ops->peek_store)
		ret = ops->peek_store(cpu);
	srcu_read_unlock(&hermit_backend_srcu, idx);

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_peek_store);
