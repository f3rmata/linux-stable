/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_BACKEND_H
#define _LINUX_HERMIT_BACKEND_H

#include <linux/swap.h>

struct page;

struct hermit_backend_ops {
	int (*load)(swp_entry_t entry, struct page *page, int cpu, bool async);
	int (*store)(swp_entry_t entry, struct page *page, int cpu, bool async);
	int (*poll_load)(int cpu);
	int (*peek_load)(int cpu);
};

int hermit_register_backend(const struct hermit_backend_ops *ops);
void hermit_unregister_backend(const struct hermit_backend_ops *ops);
bool hermit_backend_ready(void);
int hermit_backend_load(swp_entry_t entry, struct page *page, int cpu,
			bool async);
int hermit_backend_store(swp_entry_t entry, struct page *page, int cpu,
			 bool async);
int hermit_backend_poll_load(int cpu);
int hermit_backend_peek_load(int cpu);
int hermit_backend_mark_remote(swp_entry_t entry);
bool hermit_backend_entry_remote(swp_entry_t entry);
void hermit_backend_invalidate(swp_entry_t entry);

#endif
