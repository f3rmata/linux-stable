#ifndef HERMIT_BACKEND_H_
#define HERMIT_BACKEND_H_

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/swap.h>

struct hermit_backend_ops {
	int (*load)(swp_entry_t entry, struct page *page, int cpu, bool async);
	int (*store)(swp_entry_t entry, struct page *page, int cpu, bool async);
	void (*invalidate_page)(swp_entry_t entry);
	void (*invalidate_area)(unsigned int type);
	int (*poll_load)(int cpu);
	int (*peek_load)(int cpu);
	int (*poll_store)(int cpu);
	int (*peek_store)(int cpu);
};

int hermit_register_backend(const struct hermit_backend_ops *ops);
int hermit_regsiter_backend(const struct hermit_backend_ops *ops);
void hermit_unregister_backend(const struct hermit_backend_ops *ops);

bool hermit_backend_ready(void);
int hermit_backend_load(swp_entry_t entry, struct page *page, int cpu,
			bool async);
int hermit_backend_store(swp_entry_t entry, struct page *page, int cpu,
			 bool async);
void hermit_backend_invalidate_page(swp_entry_t entry);
void hermit_backend_invalidate_area(unsigned int type);
int hermit_backend_poll_load(int cpu);
int hermit_backend_peek_load(int cpu);
int hermit_backend_poll_store(int cpu);
int hermit_backend_peek_store(int cpu);

#endif // HERMIT_BACKEND_H_
