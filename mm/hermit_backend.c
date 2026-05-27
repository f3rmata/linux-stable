#include <linux/hermit_backend.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>

static const struct hermit_backend_ops __rcu *hermit_backend_ops;
static DEFINE_MUTEX(hermit_backend_lock);

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

	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(hermit_unregister_backend);

bool hermit_backend_ready(void)
{
	const struct hermit_backend_ops *ops;
	bool ready;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	ready = ops && ops->load;
	rcu_read_unlock();

	return ready;
}
EXPORT_SYMBOL_GPL(hermit_backend_ready);

int hermit_backend_load(swp_entry_t entry, struct page *page, int cpu,
			bool async)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->load)
		ret = ops->load(entry, page, cpu, async);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_load);

int hermit_backend_store(swp_entry_t entry, struct page *page, int cpu,
			 bool async)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->store)
		ret = ops->store(entry, page, cpu, async);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_store);

void hermit_backend_invalidate_page(swp_entry_t entry)
{
	const struct hermit_backend_ops *ops;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->invalidate_page)
		ops->invalidate_page(entry);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(hermit_backend_invalidate_page);

void hermit_backend_invalidate_area(unsigned int type)
{
	const struct hermit_backend_ops *ops;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->invalidate_area)
		ops->invalidate_area(type);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(hermit_backend_invalidate_area);

int hermit_backend_poll_load(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->poll_load)
		ret = ops->poll_load(cpu);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_poll_load);

int hermit_backend_peek_load(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->peek_load)
		ret = ops->peek_load(cpu);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_peek_load);

int hermit_backend_poll_store(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = 0;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->poll_store)
		ret = ops->poll_store(cpu);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_poll_store);

int hermit_backend_peek_store(int cpu)
{
	const struct hermit_backend_ops *ops;
	int ret = -EOPNOTSUPP;

	rcu_read_lock();
	ops = rcu_dereference(hermit_backend_ops);
	if (ops && ops->peek_store)
		ret = ops->peek_store(cpu);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(hermit_backend_peek_store);
