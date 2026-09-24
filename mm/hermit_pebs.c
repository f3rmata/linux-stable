// SPDX-License-Identifier: GPL-2.0
/*
 * Hermit PEBS page-size policy.
 *
 * Hardware-sampled (Intel PEBS) access tracking, aggregated per 2 MiB
 * virtual region inside a memcg, driving the per-folio RDMA transfer
 * order in the Hermit swap path. The sampling machinery mirrors MEMTIS
 * (SOSP'23) htmm_sampler.c; the decision model is described in
 * docs/motivation.md: amortize the per-WR fixed cost against the
 * read amplification 1/f (f = touched fraction of the transferred
 * folio at candidate order).
 */
#include <linux/kernel.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/hash.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/perf_event.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/hermit_pebs.h>
#include <asm/pgtable.h>

#include "../kernel/events/internal.h"

/* ---------- tunables (debugfs) ---------- */
u32 hermit_pebs_enabled;
u32 hermit_pebs_mode = HERMIT_PEBS_MODE_STATIC;
u32 hermit_pebs_force_order;
u32 hermit_pebs_fixed_cost_ns = 7000;
u32 hermit_pebs_bw_mibps = 12288;
u32 hermit_pebs_min_f = 50;		/* permille */
u32 hermit_pebs_hysteresis = 900;	/* permille */
u32 hermit_pebs_cooling_period = 100000;
u32 hermit_pebs_region_max = HERMIT_PEBS_MAX_REGIONS;

/* ---------- sampling period lists (from MEMTIS) ---------- */
#define HERMIT_PCOUNT 30
static const unsigned int hermit_period_list[HERMIT_PCOUNT] = {
	199, 293, 401, 499, 599, 701, 797, 907, 997, 1201,
	1399, 1601, 1801, 1999, 2503, 3001, 3499, 4001, 4507, 4999,
	6007, 7001, 7993, 9001, 10007, 12007, 13999, 16001, 17989, 19997,
};
#define HERMIT_PINSTCOUNT 5
static const unsigned int hermit_inst_period_list[HERMIT_PINSTCOUNT] = {
	100003, 300007, 600011, 1000003, 1500003,
};

struct hermit_pebs_sample {
	struct perf_event_header header;
	__u64 ip;
	__u32 pid, tid;
	__u64 addr;
};

static struct task_struct *hermit_pebsd;
static struct perf_event **hermit_pebs_events[HERMIT_PEBS_NR_EVENTS];
static DEFINE_MUTEX(hermit_pebs_mutex);
static pid_t hermit_pebs_pid = -1;
static unsigned int hermit_period_idx;
static unsigned int hermit_inst_period_idx;

/* global counters */
static atomic_long_t hermit_nr_sampled;
static atomic_long_t hermit_window_samples;
static atomic_long_t hermit_nr_throttled;
static atomic_long_t hermit_nr_lost;
static atomic_long_t hermit_order_decisions[HERMIT_PEBS_MAX_ORDER + 1];
static atomic_long_t hermit_order_unknown;

/* ---------- region table (per memcg) ---------- */

static u32 hermit_region_hash(struct mm_struct *mm, unsigned long vaddr)
{
	return hash_64((unsigned long)mm ^ (vaddr >> HERMIT_REGION_SHIFT), 8);
}

static struct hermit_region *hermit_region_lookup(struct hermit_pebs_state *st,
						  struct mm_struct *mm,
						  unsigned long vaddr)
{
	u32 hash = hermit_region_hash(mm, vaddr);
	struct hermit_region *r;

	hlist_for_each_entry(r, &st->buckets[hash], hnode)
		if (r->mm == mm && r->vaddr == vaddr)
			return r;
	return NULL;
}

static void hermit_region_touch(struct mem_cgroup *memcg, struct mm_struct *mm,
				u64 addr)
{
	struct hermit_pebs_state *st = memcg->hermit_pebs;
	struct hermit_region *r, *new = NULL;
	unsigned long vaddr = addr & ~((1UL << HERMIT_REGION_SHIFT) - 1);
	unsigned int bit = (addr >> PAGE_SHIFT) &
			   (HERMIT_REGION_NR_SUBPAGES - 1);
	u32 hash = hermit_region_hash(mm, vaddr);
	u32 clock, cooling_period = READ_ONCE(hermit_pebs_cooling_period);

	if (!st)
		return;

	spin_lock(&st->lock);
	clock = st->cooling_clock;
	r = hermit_region_lookup(st, mm, vaddr);
	if (r) {
		if ((int)(clock - r->cooling_clock) > HERMIT_PEBS_STALE_CLOCKS)
			memset(r->touched, 0, sizeof(r->touched));
		r->cooling_clock = clock;
		set_bit(bit, (unsigned long *)r->touched);
		list_move(&r->lru, &st->lru);
	} else if (st->nr_regions >= max_t(u32, 1, READ_ONCE(hermit_pebs_region_max))) {
		/* evict LRU tail and reuse its object */
		new = list_last_entry(&st->lru, struct hermit_region, lru);
		list_del(&new->lru);
		hlist_del(&new->hnode);
		st->nr_regions--;
	}
	st->nr_samples++;
	if (cooling_period && (st->nr_samples % cooling_period) == 0)
		st->cooling_clock++;
	spin_unlock(&st->lock);

	if (r)
		return;

	if (!new) {
		new = kzalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return;
	} else {
		mmdrop(new->mm);
		memset(new->touched, 0, sizeof(new->touched));
	}

	spin_lock(&st->lock);
	r = hermit_region_lookup(st, mm, vaddr);
	if (r) {
		spin_unlock(&st->lock);
		kfree(new);
		return;
	}
	mmgrab(mm);
	new->mm = mm;
	new->vaddr = vaddr;
	new->cooling_clock = st->cooling_clock;
	set_bit(bit, (unsigned long *)new->touched);
	list_add(&new->lru, &st->lru);
	hlist_add_head(&new->hnode, &st->buckets[hash]);
	st->nr_regions++;
	spin_unlock(&st->lock);
}

/* ---------- sampling ---------- */

static bool hermit_valid_va(u64 addr)
{
	return addr != 0 && !(addr >> (PGDIR_SHIFT + 9));
}

static void hermit_pebs_record(struct hermit_pebs_sample *s)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct mem_cgroup *memcg;

	if (!hermit_valid_va(s->addr))
		return;

	atomic_long_inc(&hermit_nr_sampled);
	atomic_long_inc(&hermit_window_samples);

	rcu_read_lock();
	task = find_task_by_vpid(s->tid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return;

	memcg = get_mem_cgroup_from_mm(mm);
	if (memcg) {
		if (memcg->hermit_pebs_enabled)
			hermit_region_touch(memcg, mm, s->addr);
		css_put(&memcg->css);
	}
	mmput(mm);
}

/* Records may straddle both allocation pages and the end of the ring. */
static void hermit_ring_copy(struct perf_buffer *rb, u64 pos,
			     void *dst, size_t len)
{
	unsigned int shift = PAGE_SHIFT + page_order(rb);
	size_t page_size = 1UL << shift;

	while (len) {
		size_t off = pos & (page_size - 1);
		size_t n = min(len, page_size - off);
		unsigned int pg = (pos >> shift) & (rb->nr_pages - 1);

		memcpy(dst, rb->data_pages[pg] + off, n);
		dst += n;
		pos += n;
		len -= n;
	}
}

static int hermit_pebs_read_events(void)
{
	int cpu, e;
	unsigned long count = 0;

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		if (!hermit_pebs_events[e])
			continue;
		for_each_possible_cpu(cpu) {
			struct perf_event *event = hermit_pebs_events[e][cpu];
			struct perf_buffer *rb;
			struct perf_event_mmap_page *up;
			u64 head, tail;
			unsigned int n = 0;

			if (!event || !event->rb)
				continue;
			rb = event->rb;
			up = rb->user_page;
			head = smp_load_acquire(&up->data_head);
			tail = READ_ONCE(up->data_tail);
			if (head - tail > perf_data_size(rb)) {
				atomic_long_inc(&hermit_nr_lost);
				tail = head;
			}
			while (head != tail && n++ < 512) {
				struct perf_event_header ph;

				if (head - tail < sizeof(ph))
					break;
				hermit_ring_copy(rb, tail, &ph, sizeof(ph));
				if (ph.size < sizeof(ph) || ph.size > perf_data_size(rb)) {
					atomic_long_inc(&hermit_nr_lost);
					tail = head;
					break;
				}
				if (ph.size > head - tail)
					break;
				if (ph.type == PERF_RECORD_SAMPLE &&
				    ph.size >= sizeof(struct hermit_pebs_sample)) {
					struct hermit_pebs_sample sample;

					hermit_ring_copy(rb, tail, &sample, sizeof(sample));
					hermit_pebs_record(&sample);
					count++;
				} else if (ph.type == PERF_RECORD_THROTTLE ||
					   ph.type == PERF_RECORD_UNTHROTTLE) {
					atomic_long_inc(&hermit_nr_throttled);
				} else if (ph.type == PERF_RECORD_LOST) {
					atomic_long_inc(&hermit_nr_lost);
				}
				tail += ph.size;
			}
			/* Finish copying before allowing the producer to reuse pages. */
			smp_store_release(&up->data_tail, tail);
		}
	}
	return count;
}

static void hermit_pebs_update_periods(void)
{
	int cpu, e;
	u64 period, inst_period;

	period = hermit_period_list[hermit_period_idx];
	inst_period = hermit_inst_period_list[hermit_inst_period_idx];

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		if (!hermit_pebs_events[e])
			continue;
		for_each_online_cpu(cpu) {
			struct perf_event *event = hermit_pebs_events[e][cpu];

			if (!event)
				continue;
			hermit_perf_event_set_period(event,
				e == 0 ? period : inst_period);
		}
	}
}

static void hermit_pebs_adapt(unsigned long window_samples)
{
	if (window_samples > 50000 &&
	    hermit_period_idx < HERMIT_PCOUNT - 1) {
		hermit_period_idx++;
		hermit_inst_period_idx =
			min_t(unsigned int, hermit_inst_period_idx + 1,
			      HERMIT_PINSTCOUNT - 1);
		hermit_pebs_update_periods();
	} else if (window_samples < 1000 && hermit_period_idx > 0) {
		hermit_period_idx--;
		if (hermit_inst_period_idx > 0)
			hermit_inst_period_idx--;
		hermit_pebs_update_periods();
	}
}

static int hermit_pebsd_main(void *arg)
{
	unsigned long last = jiffies;
	const unsigned long window = msecs_to_jiffies(2000);

	while (!kthread_should_stop()) {
		hermit_pebs_read_events();
		schedule_timeout_interruptible(usecs_to_jiffies(2000));
		if (time_after(jiffies, last + window)) {
			unsigned long samples =
				atomic_long_xchg(&hermit_window_samples, 0);

			last = jiffies;
			hermit_pebs_adapt(samples);
		}
	}
	return 0;
}

static void hermit_pebs_close_events(void)
{
	int cpu, e;

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		if (!hermit_pebs_events[e])
			continue;
		for_each_possible_cpu(cpu) {
			if (hermit_pebs_events[e][cpu]) {
				perf_event_disable(hermit_pebs_events[e][cpu]);
				hermit_perf_event_release(hermit_pebs_events[e][cpu]);
				hermit_pebs_events[e][cpu] = NULL;
			}
		}
		kfree(hermit_pebs_events[e]);
		hermit_pebs_events[e] = NULL;
	}
}

static int hermit_pebs_open_events(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_RAW,
		.size = sizeof(attr),
		.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
			       PERF_SAMPLE_ADDR,
		.exclude_kernel = 1,
		.exclude_hv = 1,
		.exclude_callchain_kernel = 1,
		.exclude_callchain_user = 1,
		.precise_ip = 1,
	};
	struct task_struct *task = NULL;
	int cpu, e, err = 0;

	if (hermit_pebs_pid != -1) {
		task = find_get_task_by_vpid(hermit_pebs_pid);
		if (!task)
			return -ESRCH;
		attr.enable_on_exec = 1;
	}

	for (e = 0; e < HERMIT_PEBS_NR_EVENTS; e++) {
		hermit_pebs_events[e] = kcalloc(nr_cpu_ids,
						sizeof(struct perf_event *),
						GFP_KERNEL);
		if (!hermit_pebs_events[e]) {
			err = -ENOMEM;
			goto out;
		}
	}

	attr.config = HERMIT_PEBS_L3_MISS;
	attr.sample_period = hermit_period_list[0];
	for_each_online_cpu(cpu) {
		struct perf_event *event;

		event = perf_event_create_kernel_counter(&attr, cpu, task,
							 NULL, NULL);
		if (IS_ERR(event)) {
			err = PTR_ERR(event);
			goto out;
		}
		err = hermit_perf_event_init(event, HERMIT_PEBS_RING_PAGES);
		if (err) {
			perf_event_release_kernel(event);
			goto out;
		}
		hermit_pebs_events[0][cpu] = event;
	}

	attr.config = HERMIT_PEBS_ALL_STORES;
	attr.sample_period = hermit_inst_period_list[0];
	for_each_online_cpu(cpu) {
		struct perf_event *event;

		event = perf_event_create_kernel_counter(&attr, cpu, task,
							 NULL, NULL);
		if (IS_ERR(event)) {
			err = PTR_ERR(event);
			goto out;
		}
		err = hermit_perf_event_init(event, HERMIT_PEBS_RING_PAGES);
		if (err) {
			perf_event_release_kernel(event);
			goto out;
		}
		hermit_pebs_events[1][cpu] = event;
	}
out:
	if (task)
		put_task_struct(task);
	if (err)
		hermit_pebs_close_events();
	return err;
}

int hermit_pebs_start_sampling(pid_t pid, int mode)
{
	struct task_struct *t;
	int ret;

	mutex_lock(&hermit_pebs_mutex);
	if (hermit_pebsd) {
		mutex_unlock(&hermit_pebs_mutex);
		return -EBUSY;
	}
	hermit_pebs_pid = pid;
	hermit_period_idx = 0;
	hermit_inst_period_idx = 0;
	atomic_long_set(&hermit_window_samples, 0);
	cpus_read_lock();
	ret = hermit_pebs_open_events();
	cpus_read_unlock();
	if (ret) {
		mutex_unlock(&hermit_pebs_mutex);
		return ret;
	}
	t = kthread_run(hermit_pebsd_main, NULL, "hermit_pebsd");
	if (IS_ERR(t)) {
		hermit_pebs_close_events();
		mutex_unlock(&hermit_pebs_mutex);
		return PTR_ERR(t);
	}
	hermit_pebsd = t;
	if (mode != -1)
		WRITE_ONCE(hermit_pebs_mode, mode);
	WRITE_ONCE(hermit_pebs_enabled, 1);
	mutex_unlock(&hermit_pebs_mutex);
	return 0;
}

void hermit_pebs_stop_sampling(void)
{
	mutex_lock(&hermit_pebs_mutex);
	if (hermit_pebsd) {
		kthread_stop(hermit_pebsd);
		hermit_pebsd = NULL;
	}
	hermit_pebs_close_events();
	hermit_pebs_enabled = 0;
	mutex_unlock(&hermit_pebs_mutex);
}

/* ---------- policy ---------- */

static unsigned long region_popcount(const u64 *touched,
				     unsigned long start, unsigned long nr)
{
	unsigned long w = start / 64;
	unsigned long b = start % 64;
	unsigned long cnt = 0;

	while (nr) {
		unsigned long take = min(nr, 64UL - b);
		u64 word = touched[w] >> b;

		if (take < 64)
			word &= (1UL << take) - 1;
		cnt += hweight64(word);
		nr -= take;
		w++;
		b = 0;
	}
	return cnt;
}

/* ns per 4 KiB page at full link bandwidth */
static u64 hermit_byte_cost_ns(void)
{
	u32 bw = READ_ONCE(hermit_pebs_bw_mibps);

	if (!bw)
		return 0;
	return div64_u64((u64)PAGE_SIZE * 1000000000ULL,
			 (u64)bw * (1ULL << 20));
}

static int hermit_choose_order(const u64 *touched, unsigned long addr,
		unsigned int folio_order, unsigned int cur_order,
		unsigned long allowed_mask, struct seq_file *m)
{
	u64 byte_cost = hermit_byte_cost_ns();
	u64 best_cost = U64_MAX, cur_cost = U64_MAX;
	unsigned long base;
	unsigned int o, max_o;
	int best = -1;
	bool cur_valid = false;

	if (!byte_cost)
		return -1;
	base = (addr >> PAGE_SHIFT) & (HERMIT_REGION_NR_SUBPAGES - 1);
	max_o = min(folio_order, (unsigned int)HERMIT_PEBS_MAX_ORDER);
	for (o = 0; o <= max_o; o++) {
		unsigned long nr = 1UL << o;
		unsigned long start = base & ~(nr - 1);
		unsigned long pc, f;
		u64 cost;

		/* order 1 (8 KiB) is not a THP order; remote_order_mask
		 * rejects bit 1, mirror that here.
		 */
		if (o == 1)
			continue;
		if (o != 0 && !(allowed_mask & BIT(o)))
			continue;
		pc = region_popcount(touched, start, nr);
		f = pc * 1000 / nr;
		f = max(f, (unsigned long)min_t(u32, READ_ONCE(hermit_pebs_min_f), 1000));
		if (!f)
			f = 1;
		cost = (u64)hermit_pebs_fixed_cost_ns >> o;
		cost += div64_u64(byte_cost * 1000, f);
		if (m)
			seq_printf(m, "%2u %5lu %11lu %13llu\n", o, nr, f, cost);
		if (cost <= best_cost) {
			best_cost = cost;
			best = o;
		}
		if (o == cur_order) {
			cur_cost = cost;
			cur_valid = true;
		}
	}

	if (best < 0)
		best = 0;
	/* hysteresis: keep the current order unless the new one is
	 * at least (1000 - hysteresis) permille cheaper.
	 */
	if (cur_valid && best != (int)cur_order &&
	    best_cost * 1000 > cur_cost * min_t(u32, READ_ONCE(hermit_pebs_hysteresis), 1000))
		best = cur_order;

	return best;
}

int hermit_order_policy(struct mem_cgroup *memcg, struct mm_struct *mm,
			unsigned long addr, unsigned int folio_order,
			unsigned int cur_order, unsigned long allowed_mask)
{
	struct hermit_pebs_state *st;
	struct hermit_region *r;
	u64 touched[HERMIT_REGION_WORDS];
	unsigned long vaddr;
	int best;

	if (!hermit_pebs_enabled ||
	    hermit_pebs_mode != HERMIT_PEBS_MODE_POLICY)
		return -1;
	st = memcg->hermit_pebs;
	if (!st)
		return -1;

	vaddr = addr & ~((1UL << HERMIT_REGION_SHIFT) - 1);
	spin_lock(&st->lock);
	r = hermit_region_lookup(st, mm, vaddr);
	if (!r) {
		spin_unlock(&st->lock);
		atomic_long_inc(&hermit_order_unknown);
		return -1;
	}
	if ((int)(st->cooling_clock - r->cooling_clock) > HERMIT_PEBS_STALE_CLOCKS) {
		spin_unlock(&st->lock);
		atomic_long_inc(&hermit_order_unknown);
		return -1;
	}
	memcpy(touched, r->touched, sizeof(touched));
	spin_unlock(&st->lock);

	best = hermit_choose_order(touched, addr, folio_order, cur_order,
				   allowed_mask, NULL);
	if (best < 0)
		return best;

	atomic_long_inc(&hermit_order_decisions[best]);
	return best;
}

/* ---------- syscalls ---------- */

SYSCALL_DEFINE2(hermit_pebs_start, pid_t, pid, int, mode)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (mode != -1 && mode != HERMIT_PEBS_MODE_STATIC &&
	    mode != HERMIT_PEBS_MODE_POLICY)
		return -EINVAL;
	return hermit_pebs_start_sampling(pid, mode);
}

SYSCALL_DEFINE1(hermit_pebs_end, pid_t, pid)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	hermit_pebs_stop_sampling();
	return 0;
}

/* ---------- memcg interface ---------- */

void hermit_pebs_memcg_init(struct mem_cgroup *memcg)
{
	struct hermit_pebs_state *st;
	int i;

	memcg->hermit_pebs_enabled = false;
	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st) {
		memcg->hermit_pebs = NULL;
		return;
	}
	spin_lock_init(&st->lock);
	for (i = 0; i < HERMIT_PEBS_BUCKETS; i++)
		INIT_HLIST_HEAD(&st->buckets[i]);
	INIT_LIST_HEAD(&st->lru);
	memcg->hermit_pebs = st;
}

void hermit_pebs_memcg_exit(struct mem_cgroup *memcg)
{
	struct hermit_pebs_state *st = memcg->hermit_pebs;
	struct hermit_region *r, *tmp;

	if (!st)
		return;
	list_for_each_entry_safe(r, tmp, &st->lru, lru) {
		mmdrop(r->mm);
		kfree(r);
	}
	kfree(st);
	memcg->hermit_pebs = NULL;
}

ssize_t hermit_pebs_memcg_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));

	if (sysfs_streq(buf, "enabled"))
		memcg->hermit_pebs_enabled = true;
	else if (sysfs_streq(buf, "disabled"))
		memcg->hermit_pebs_enabled = false;
	else
		return -EINVAL;
	return nbytes;
}

int hermit_pebs_memcg_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	seq_puts(m, memcg->hermit_pebs_enabled ? "enabled\n" : "disabled\n");
	return 0;
}

/* ---------- debugfs ---------- */

static int hermit_pebs_stats_show(struct seq_file *m, void *v)
{
	int i;

	seq_puts(m, "order: decisions\n");
	for (i = 0; i <= HERMIT_PEBS_MAX_ORDER; i++)
		seq_printf(m, "%2d: %ld\n", i,
			   atomic_long_read(&hermit_order_decisions[i]));
	seq_printf(m, "unknown: %ld\n",
		   atomic_long_read(&hermit_order_unknown));
	seq_printf(m, "sampled: %ld\n", atomic_long_read(&hermit_nr_sampled));
	seq_printf(m, "throttled: %ld\n", atomic_long_read(&hermit_nr_throttled));
	seq_printf(m, "lost: %ld\n", atomic_long_read(&hermit_nr_lost));
	seq_printf(m, "period_idx: %u inst_period_idx: %u\n",
		   hermit_period_idx, hermit_inst_period_idx);
	return 0;
}

static int hermit_pebs_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, hermit_pebs_stats_show, NULL);
}

static const struct file_operations hermit_pebs_stats_fops = {
	.open		= hermit_pebs_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* test hook: write 8 hex u64 words (512-bit touched bitmap), read back the
 * policy evaluation for a 2 MiB folio at base 0.
 */
static u64 hermit_test_touched[HERMIT_REGION_WORDS];
static unsigned long hermit_test_allowed = ~0UL;

static int hermit_test_eval_show(struct seq_file *m, void *v)
{
	int best;

	seq_puts(m, "order pages  f(permille)  cost(ns/page)\n");
	best = hermit_choose_order(hermit_test_touched, 0,
		HERMIT_PEBS_MAX_ORDER, HERMIT_PEBS_MAX_ORDER + 1,
		hermit_test_allowed, m);
	seq_printf(m, "decision: %d\n", best);
	return 0;
}

static int hermit_test_eval_open(struct inode *inode, struct file *file)
{
	return single_open(file, hermit_test_eval_show, NULL);
}

static ssize_t hermit_test_eval_write(struct file *file,
				      const char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	char buf[256];
	char *p = buf;
	u64 words[HERMIT_REGION_WORDS];
	int i, n;

	if (len >= sizeof(buf))
		return -E2BIG;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	memset(words, 0, sizeof(words));
	for (i = 0; i < HERMIT_REGION_WORDS; i++) {
		if (sscanf(p, "%llx%n", &words[i], &n) != 1)
			break;
		p += n;
	}
	if (i < HERMIT_REGION_WORDS)
		return -EINVAL;

	memcpy(hermit_test_touched, words, sizeof(words));
	return len;
}

static const struct file_operations hermit_test_eval_fops = {
	.open		= hermit_test_eval_open,
	.read		= seq_read,
	.write		= hermit_test_eval_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int hermit_ring_test_show(struct seq_file *m, void *unused)
{
	struct perf_buffer *rb;
	struct perf_event *event;
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_CPU_CLOCK,
		.disabled = 1,
	};
	u8 result[32];
	unsigned int i, j;
	bool pass = true;
	const u64 starts[] = { 0, PAGE_SIZE - 8, 2 * PAGE_SIZE - 8 };

	rb = kzalloc(sizeof(*rb) + 2 * sizeof(void *), GFP_KERNEL);
	if (!rb)
		return -ENOMEM;
	rb->nr_pages = 2;
	for (i = 0; i < 2; i++) {
		rb->data_pages[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!rb->data_pages[i]) {
			while (i--)
				kfree(rb->data_pages[i]);
			kfree(rb);
			return -ENOMEM;
		}
		for (j = 0; j < PAGE_SIZE; j++)
			((u8 *)rb->data_pages[i])[j] = (u8)(j + 31 * i);
	}
	for (i = 0; i < ARRAY_SIZE(starts); i++) {
		hermit_ring_copy(rb, starts[i], result, sizeof(result));
		for (j = 0; j < sizeof(result); j++) {
			u64 pos = (starts[i] + j) % (2 * PAGE_SIZE);
			u8 expected = (u8)(pos % PAGE_SIZE + 31 * (pos / PAGE_SIZE));

			if (result[j] != expected)
				pass = false;
		}
	}
	for (i = 0; i < 2; i++)
		kfree(rb->data_pages[i]);
	kfree(rb);
	/* Exercise real perf allocation/detach without requiring a PEBS PMU. */
	for (i = 0; i < 2; i++) {
		int ret;

		event = perf_event_create_kernel_counter(&attr, -1, current, NULL, NULL);
		if (IS_ERR(event))
			return PTR_ERR(event);
		ret = hermit_perf_event_init(event, 2);
		if (ret) {
			perf_event_release_kernel(event);
			return ret;
		}
		hermit_perf_event_release(event);
	}
	seq_printf(m, "%s\n", pass ? "pass" : "fail");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hermit_ring_test);

void hermit_pebs_debugfs_init(struct dentry *root)
{
	debugfs_create_file("pebs_ring_test", 0400, root, NULL,
			    &hermit_ring_test_fops);
	debugfs_create_u32("pebs_enabled", 0600, root, &hermit_pebs_enabled);
	debugfs_create_u32("pebs_mode", 0600, root, &hermit_pebs_mode);
	debugfs_create_u32("pebs_force_order", 0600, root,
			   &hermit_pebs_force_order);
	debugfs_create_u32("pebs_fixed_cost_ns", 0600, root,
			   &hermit_pebs_fixed_cost_ns);
	debugfs_create_u32("pebs_bw_mibps", 0600, root, &hermit_pebs_bw_mibps);
	debugfs_create_u32("pebs_min_f", 0600, root, &hermit_pebs_min_f);
	debugfs_create_u32("pebs_hysteresis", 0600, root,
			   &hermit_pebs_hysteresis);
	debugfs_create_u32("pebs_cooling_period", 0600, root,
			   &hermit_pebs_cooling_period);
	debugfs_create_u32("pebs_region_max", 0600, root,
			   &hermit_pebs_region_max);
	debugfs_create_file("pebs_order_stats", 0400, root, NULL,
			    &hermit_pebs_stats_fops);
	debugfs_create_file("pebs_test_eval", 0600, root, NULL,
			    &hermit_test_eval_fops);
}
